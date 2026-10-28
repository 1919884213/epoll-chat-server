// SPDX-License-Identifier: GPL-2.0
/*
 * chatsnap —— 聊天室快照字符设备
 *
 * 一条单向的内核快照通道：
 *   生产者：聊天服务器进程 open("/dev/chatsnap", O_WRONLY|O_NONBLOCK)
 *           每广播一条消息就 write() 一行（行以 \n 结尾）
 *   消费者：监控端 open(O_RDONLY) 后 read()/poll()，例如 cat /dev/chatsnap
 *   缓冲  ：内核 kfifo 环形缓冲，满了一整条丢弃并计数
 *
 * 记录边界：一次 write() 就是一条记录。写入路径持锁，所以消费者读到的
 * 永远是完整行；只要用户态读缓冲 >= 单条最大长度（本项目单行 <= 1024），
 * 一次 read 就不会把一行拆开。
 *
 * 并发说明（本驱动唯一需要解释的设计点）：
 *   write/read/ioctl/sysfs 全部运行在进程上下文，没有任何中断上下文参与，
 *   所以统一用一把 mutex（可睡眠）。将来若把数据源搬进中断处理函数
 *   （例如 GPIO 中断里直接写快照），才必须换成 spin_lock_irqsave()。
 */
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/version.h>   /* LINUX_VERSION_CODE / KERNEL_VERSION() */
#include <linux/wait.h>

#include "chatsnap_uapi.h"

#define DEF_BUF_KB 64

static unsigned int buf_kb = DEF_BUF_KB;
module_param(buf_kb, uint, 0444);
MODULE_PARM_DESC(buf_kb, "ring buffer size in KB (default 64, rounded up to a power of 2)");

static dev_t          g_dev;
static struct cdev    g_cdev;
static struct class  *g_class;
static struct device *g_device;

static struct kfifo      g_fifo;
static wait_queue_head_t g_wq;
static DEFINE_MUTEX(g_lock);   /* 保护 kfifo 与下面三个计数：全在进程上下文 */

static unsigned long g_in;     /* 累计写入字节 */
static unsigned long g_out;    /* 累计读出字节 */
static unsigned long g_drop;   /* 因缓冲满整条丢弃的次数 */

/* ============================ file_operations ============================ */

static int chatsnap_open(struct inode *ino, struct file *filp)
{
	unsigned int acc = filp->f_flags & O_ACCMODE;

	/* 一个 fd 只能选一种身份：只读=消费者，只写=生产者 */
	if (acc == O_RDWR)
		return -EINVAL;

	/* rmmod 时若仍有 fd 打开，引用计数会让卸载返回 EBUSY 而不是把内核写崩 */
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	pr_info("chatsnap: opened as %s\n",
		acc == O_WRONLY ? "producer" : "consumer");
	return 0;
}

static int chatsnap_release(struct inode *ino, struct file *filp)
{
	module_put(THIS_MODULE);
	return 0;
}

static ssize_t chatsnap_read(struct file *filp, char __user *ubuf, size_t count,
			     loff_t *ppos)
{
	unsigned int len, copied = 0;
	int ret;

	if (count == 0)
		return 0;

	/* 没数据就阻塞。注意等待前必须先放锁，否则生产者永远拿不到锁 —— 死锁 */
	for (;;) {
		if (mutex_lock_interruptible(&g_lock))
			return -ERESTARTSYS;
		if (!kfifo_is_empty(&g_fifo))
			break;
		mutex_unlock(&g_lock);
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(g_wq, !kfifo_is_empty(&g_fifo));
		if (ret)
			return ret;
	}

	len = kfifo_len(&g_fifo);
	if (len > (unsigned int)count)
		len = (unsigned int)count;   /* 用户缓冲偏小会拆包，见文件头说明 */

	ret = kfifo_to_user(&g_fifo, ubuf, len, &copied);
	if (ret == 0)
		g_out += copied;
	mutex_unlock(&g_lock);

	return ret ? ret : (ssize_t)copied;
}

static ssize_t chatsnap_write(struct file *filp, const char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	unsigned int copied = 0;
	int ret;

	if (count == 0)
		return 0;
	if (count > kfifo_size(&g_fifo))
		return -EMSGSIZE;   /* 单条比整个缓冲还大，永远放不进去 */

	if (mutex_lock_interruptible(&g_lock))
		return -ERESTARTSYS;

	if (kfifo_avail(&g_fifo) < count) {
		/*
		 * 缓冲满：整条丢弃，保留最旧数据（监控场景最需要的是"最近发生了什么"）。
		 * 返回 count 而不是错误：快照是旁路通道，不能因为满了就让业务层重试或报错。
		 */
		g_drop++;
		mutex_unlock(&g_lock);
		return (ssize_t)count;
	}

	ret = kfifo_from_user(&g_fifo, ubuf, count, &copied);
	mutex_unlock(&g_lock);
	if (ret)
		return ret;         /* -EFAULT：用户态地址不可读 */

	g_in += copied;
	wake_up_interruptible(&g_wq);   /* 唤醒阻塞中的消费者 */
	return (ssize_t)copied;
}

/*
 * poll 必须是非阻塞的，所以这里用 trylock：拿不到锁就报"暂无数据"，
 * 但 poll_wait 已经登记过等待队列，投放数据的一方一定会唤醒我们，
 * 不存在丢唤醒。
 */
static __poll_t chatsnap_poll(struct file *filp, struct poll_table_struct *wait)
{
	__poll_t mask = 0;

	poll_wait(filp, &g_wq, wait);
	if (mutex_trylock(&g_lock)) {
		if (!kfifo_is_empty(&g_fifo))
			mask |= EPOLLIN | EPOLLRDNORM;
		mutex_unlock(&g_lock);
	}
	return mask;
}

static long chatsnap_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct chatsnap_stat st;

	if (_IOC_TYPE(cmd) != CHATSNAP_MAGIC)
		return -ENOTTY;

	if (mutex_lock_interruptible(&g_lock))
		return -ERESTARTSYS;

	switch (cmd) {
	case CHATSNAP_CLEAR:
		/* kfifo_reset 要求没有并发读写者 —— 持 g_lock 期间满足 */
		kfifo_reset(&g_fifo);
		g_in = g_out = g_drop = 0;
		mutex_unlock(&g_lock);
		return 0;

	case CHATSNAP_GET_STAT:
		st.size = kfifo_size(&g_fifo);
		st.len  = kfifo_len(&g_fifo);
		st.in   = g_in;
		st.out  = g_out;
		st.drop = g_drop;
		break;

	default:
		mutex_unlock(&g_lock);
		return -ENOTTY;
	}
	mutex_unlock(&g_lock);

	if (copy_to_user((void __user *)arg, &st, sizeof(st)))
		return -EFAULT;
	return 0;
}

/* /sys/class/chatsnap/chatsnap/stat：一行文本，脚本可直接读 */
static ssize_t stat_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	unsigned int size, len;
	unsigned long in, out, drop;

	mutex_lock(&g_lock);
	size = kfifo_size(&g_fifo);
	len  = kfifo_len(&g_fifo);
	in   = g_in;
	out  = g_out;
	drop = g_drop;
	mutex_unlock(&g_lock);

	return sysfs_emit(buf, "size=%u len=%u in=%lu out=%lu drop=%lu\n",
			  size, len, in, out, drop);
}
static DEVICE_ATTR_RO(stat);

static const struct file_operations chatsnap_fops = {
	.owner          = THIS_MODULE,
	.open           = chatsnap_open,
	.release        = chatsnap_release,
	.read           = chatsnap_read,
	.write          = chatsnap_write,
	.poll           = chatsnap_poll,
	.unlocked_ioctl = chatsnap_ioctl,
	.llseek         = noop_llseek,  /* no_llseek 在 6.12+ 被删；noop_llseek 6.1/7.x 都在 */
};

/* ============================ 模块生命周期 ============================ */

static int __init chatsnap_init(void)
{
	int ret;

	if (buf_kb == 0)          /* module_param 是信任边界，别让 0 变成一个字节的缓冲 */
		buf_kb = DEF_BUF_KB;

	ret = alloc_chrdev_region(&g_dev, 0, 1, "chatsnap");
	if (ret)
		return ret;

	cdev_init(&g_cdev, &chatsnap_fops);
	g_cdev.owner = THIS_MODULE;
	ret = cdev_add(&g_cdev, g_dev, 1);
	if (ret)
		goto err_chrdev;

	/* class_create 6.4 起去掉了 owner 参数：目标板 6.1.141 要两个，新内核要一个 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	g_class = class_create("chatsnap");
#else
	g_class = class_create(THIS_MODULE, "chatsnap");
#endif
	if (IS_ERR(g_class)) {
		ret = PTR_ERR(g_class);
		goto err_cdev;
	}

	/* udev 会在 /dev 下自动建节点；没有 udev 时用 dmesg 里的主次设备号手 mknod */
	g_device = device_create(g_class, NULL, g_dev, NULL, "chatsnap");
	if (IS_ERR(g_device)) {
		ret = PTR_ERR(g_device);
		goto err_class;
	}

	ret = kfifo_alloc(&g_fifo, (size_t)buf_kb * 1024, GFP_KERNEL);
	if (ret)
		goto err_dev;

	init_waitqueue_head(&g_wq);

	ret = device_create_file(g_device, &dev_attr_stat);
	if (ret)
		goto err_fifo;

	pr_info("chatsnap: loaded, %u KB ring (actual %u B), major=%d minor=%d\n",
		buf_kb, kfifo_size(&g_fifo), MAJOR(g_dev), MINOR(g_dev));
	return 0;

err_fifo:
	kfifo_free(&g_fifo);
err_dev:
	device_destroy(g_class, g_dev);
err_class:
	class_destroy(g_class);
err_cdev:
	cdev_del(&g_cdev);
err_chrdev:
	unregister_chrdev_region(g_dev, 1);
	return ret;
}

static void __exit chatsnap_exit(void)
{
	device_remove_file(g_device, &dev_attr_stat);
	kfifo_free(&g_fifo);
	device_destroy(g_class, g_dev);
	class_destroy(g_class);
	cdev_del(&g_cdev);
	unregister_chrdev_region(g_dev, 1);
	pr_info("chatsnap: unloaded\n");
}

module_init(chatsnap_init);
module_exit(chatsnap_exit);

MODULE_LICENSE("GPL");   /* 内核模块惯例，与本项目 MIT 许可不冲突 */
MODULE_AUTHOR("Lin Rendong");
MODULE_DESCRIPTION("chat room snapshot char device (kfifo ring + blocking read + poll)");
MODULE_VERSION("1.0");
