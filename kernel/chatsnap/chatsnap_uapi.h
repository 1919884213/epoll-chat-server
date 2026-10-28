/* SPDX-License-Identifier: GPL-2.0 */
/*
 * chatsnap 用户态接口定义 —— 内核驱动与用户态工具共用。
 * 两边的 ioctl 命令号必须由同一份宏生成，否则 _IOC 编码（方向/大小/类型）
 * 不一致，ioctl 会直接被内核判成 ENOTTY。
 */
#ifndef CHATSNAP_UAPI_H
#define CHATSNAP_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define CHATSNAP_DEV    "/dev/chatsnap"
#define CHATSNAP_MAGIC  'C'

struct chatsnap_stat {
	__u32 size;  /* 环形缓冲总容量（字节，已向上取到 2 的幂） */
	__u32 len;   /* 当前已缓存字节数 */
	__u64 in;    /* 累计写入字节数 */
	__u64 out;   /* 累计读出字节数 */
	__u64 drop;  /* 因缓冲满而整条丢弃的次数 */
};

/* 清空缓冲并复位计数 */
#define CHATSNAP_CLEAR    _IO(CHATSNAP_MAGIC, 1)
/* 取统计信息：参数 struct chatsnap_stat * */
#define CHATSNAP_GET_STAT _IOR(CHATSNAP_MAGIC, 2, struct chatsnap_stat)

#endif /* CHATSNAP_UAPI_H */
