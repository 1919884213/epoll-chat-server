#ifndef SERVER_H
#define SERVER_H

#include "arpa/inet.h"
#include "pool.h"
#include "sys/socket.h"

/* 初始化 TCP 服务器：socket -> setsockopt -> bind -> listen，返回监听 fd */
int Server_init(void);

/* 启动事件循环：epoll 负责事件多路复用，就绪事件投递给绑定的 worker */
void server_run(int serverfd, Threadpool* pool);

#endif
