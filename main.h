#ifndef MAIN_H
#define MAIN_H

/* 服务器监听端口 */
#define PORT 8080
/* listen  backlog */
#define MAX_NUM 1024
/* 测试客户端默认连接的服务器地址 */
#define SERVERADDR "127.0.0.1"

/* 工作线程数量 */
#define WORKER_NUM 4
/* 连接表容量，同时也是可管理的最大 fd */
#define MAX_CONN 1024
/* 单连接读写缓冲区大小 */
#define BUF_SIZE 4096

#endif
