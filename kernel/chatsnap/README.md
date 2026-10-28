# chatsnap —— 聊天室快照字符设备

`/dev/chatsnap` 是聊天服务器的**旁路可观测通道**：服务器把每条广播 `write()` 进来，
监控端 `read()`/`poll()` 取走。缓冲在内核 `kfifo` 里，**服务器进程被 kill -9 之后，
缓冲里的最后 N 条消息依然读得到** —— 这是它相对"直接写日志文件"的唯一实质优势。

## 数据流

```
Chat.c: room_broadcast()  ──write()──▶  /dev/chatsnap  ──kfifo──▶  chatsnap_mon / cat
                                          (内核环形缓冲)
```

`open` 身份由 `O_ACCMODE` 决定：`O_WRONLY` = 生产者（服务器），`O_RDONLY` = 消费者（监控端）。

## 驱动实现覆盖的点

| 能力 | 位置 |
|---|---|
| `module_init/exit`、`obj-m` Kbuild | `chatsnap.c` / `Makefile` |
| 主次设备号 + `cdev` + `class_create/device_create`（udev 自动建节点） | `chatsnap_init()` |
| `file_operations`：open/read/write/poll/release/ioctl | `chatsnap_fops` |
| `copy_to_user/copy_from_user`（用 `kfifo_to_user/from_user`） | read/write |
| 阻塞读 + 等待队列 + `wake_up_interruptible` | `chatsnap_read` / `chatsnap_write` |
| `poll`/`EPOLLIN`（可被 epoll 纳管） | `chatsnap_poll` |
| `ioctl` + `_IO/_IOR` 编码，与用户态共用同一份头 | `chatsnap_uapi.h` |
| sysfs 属性文件 `DEVICE_ATTR_RO(stat)` | `/sys/class/chatsnap/chatsnap/stat` |
| `module_param` 加载参数 | `buf_kb` |
| `try_module_get/module_put`：fd 未关时 `rmmod` 返回 EBUSY | open/release |

并发策略：全部路径在**进程上下文**，统一一把 `mutex`（可睡眠）。没有中断上下文，
所以**不需要** `spin_lock_irqsave`。

## 编译

**实测状态（2026-10-07）**：本模块已在构建宿主（Ubuntu 26.04 / 内核 7.0 头）**编译通过、零警告**；
但那份 `.ko` 是给宿主内核编的，**不能 insmod 到板子上**。上板还需一份 **6.1.141** 的内核源码树 —— 板子本身
`/lib/modules/6.1.141/build` 不存在（无编译头），只有 gcc/make；`/proc/config.gz` 可读，能拿到板子的确切配置。

```bash
# 宿主侧自检（对宿主自己的内核，仅验证代码/API 拼写，产物不要拿去板子用）
make                                   # KDIR 默认 /lib/modules/$(uname -r)/build

# 目标板（拿到 6.1.141 内核树之后；板子内核没开 CONFIG_MODVERSIONS，只需 vermagic 串一致）
make KDIR=<6.1.141 内核树> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
# 或者把内核树放到板子上本地编：make
```

**跨版本 API 注意**（模块里已做兼容）：

- `class_create()` 6.4 起去掉 `owner` 参数 → 已加 `LINUX_VERSION_CODE` 分支；用 `#if` 判版本必须
  `#include <linux/version.h>`，否则报 `missing binary operator before token "("`。
- `no_llseek` 在 6.12+ 被删除 → 统一用 `noop_llseek`（6.1 与 7.x 都在）。

## 加载与验收

**实测结果（2026-10-07，宿主内核 7.0，x86_64；模块在此编出并加载）**：

- 功能验收 `scripts/chatsnap_accept.sh` → **12 PASS / 0 FAIL**：加载+udev 建节点、sysfs 统计、阻塞读双向收发、
  空缓冲阻塞（`cat` 超时 rc=124，证明不是忙等）、溢出（`in=65553 out=20 drop=13336`，满时整条丢弃且无 oops）、
  ioctl 统计/清空、**占用中 `rmmod` 返回 `Module chatsnap is in use`**、释放后卸载干净且节点被清。
- 端到端集成 `scripts/chatsnap_integ.sh` → **5 PASS / 0 FAIL**：真客户端 → 真服务器 → `room_broadcast()` →
  `chatsnap_emit()` → 内核 kfifo → 监控端读回，监控端实测收到
  `[lobby] itest: hello from integration test`，驱动统计 `in=105 out=105 len=0`。
- 加载时 `module verification failed ... tainting kernel` 是**未签名模块的正常提示**，不影响功能。

```bash
sudo insmod chatsnap.ko buf_kb=64
sudo insmod chatsnap.ko buf_kb=64
ls -l /dev/chatsnap                       # 1) 节点存在
dmesg | tail -3                           #    看 major/minor 与 "loaded"
cat /sys/class/chatsnap/chatsnap/stat     # 2) sysfs 属性可读

# 3) 双向 + 阻塞：终端 A / 终端 B
cat /dev/chatsnap                         # A：阻塞住，CPU 0%
echo hello > /dev/chatsnap                # B：A 立刻打印 hello

# 4) 非阻塞语义
timeout 3 cat /dev/chatsnap; echo $?      # 无数据超时退出（124），不烧 CPU

# 5) 溢出保护：灌 2 倍缓冲，不崩、drop 增长
for i in $(seq 1 20000); do echo "line $i"; done > /dev/chatsnap
cat /sys/class/chatsnap/chatsnap/stat     # drop > 0，dmesg 无 oops

# 6) ioctl + poll
sudo ./ChatSnapMon -s                     # 统计
sudo ./ChatSnapMon                        # poll 跟踪实时流

# 7) 卸载语义
cat /dev/chatsnap &                       # 占住一个 fd
sudo rmmod chatsnap                       # 应报 EBUSY，不崩
kill %1; sudo rmmod chatsnap              # 关掉后卸载干净，/dev/chatsnap 消失
```

## 后续阶段（未做）

- 阶段 2：改成 platform driver + 板子 dts 加节点（`compatible/snapshot-size`），
  在 `probe()` 里用 `of_property_read_u32` 取参数 —— 把"设备树 + probe 时机"补成实测经历。
- 阶段 3：GPIO 按键 → `request_irq` → `schedule_work` → LED 指示服务器活动，
  这条才涉及真正的中断上下文与下半部。
