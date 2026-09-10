#ifndef CLIENT_H
#define CLIENT_H

/* 连接到 SERVERADDR:PORT，成功返回 fd，失败返回 -1 */
int client_connect(void);

/* 测试线程入口：arg 指向本连接的编号(int*) */
void* client_worker(void* arg);

#endif
