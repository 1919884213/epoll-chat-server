#ifndef CHATSNAP_HOOK_H
#define CHATSNAP_HOOK_H

#include <stddef.h>

/*
 * 聊天室 → 内核快照通道的用户态接入。
 * 驱动没装、/dev/chatsnap 不存在时全部降级为空操作，服务器照常运行。
 */

/* 启动时打开设备（失败只告警，不阻止服务器启动）。在 server_run 前调用 */
void chatsnap_attach(void);

/* 把一条广播原样送进内核环形缓冲。非阻塞：驱动的缓冲满会整条丢弃并计数 */
void chatsnap_emit(const char* data, size_t len);

/* 退出时关闭；也可不调用，进程结束由内核回收 */
void chatsnap_detach(void);

#endif
