#ifndef CHAT_H
#define CHAT_H

#include <stddef.h>

/*
 * 聊天核心：昵称/房间/成员管理 + 行协议解析 + 广播。
 * 与线程模型解耦：Server 的 worker 线程把「某 fd 收到的原始字节」
 * 交给 chat_on_bytes()，本模块内部按连接缓冲、切行、驱动状态机。
 *
 * 锁策略：一把全局互斥锁保护所有共享状态（连接表、房间表、
 * fd->worker 归属表）。广播路径全程持锁，房间内发送天然串行，
 * 避免了旧实现中 g_worker_of_fd 主线程/worker 线程双写的竞态。
 * 锁内只做非阻塞 write（丢弃极端情况下的拥塞消息），不会死锁。
 */

/* 初始化各状态表容量（cap = MAX_CONN），在 server_run 前调用 */
void chat_init(int cap);

/* ---- fd -> worker 归属表（原 Server.c 的 g_worker_of_fd，已加锁） ---- */
void chat_fd_assign(int fd, int worker); /* accept 时登记归属 */
int  chat_fd_get(int fd);                /* 查归属，-1 = 空闲 */
void chat_fd_release(int fd);            /* 断开时回收，置 -1 */

/* 新连接建立：置为「等待昵称」状态并发送欢迎语（主线程 accept 后调用） */
void chat_on_accept(int fd);

/* 处理来自 fd 的一段原始字节（自动跨次缓冲拼行）。
 * 返回：0 继续；1 要求关闭该连接；-1 fd 未登记（过期事件，忽略） */
int chat_on_bytes(int fd, const char* buf, size_t len);

/* 连接关闭：退出房间、广播离开消息、回收槽位（调用方负责 close(fd)） */
void chat_on_close(int fd);

#endif
