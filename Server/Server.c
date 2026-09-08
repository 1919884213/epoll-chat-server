#include "Server.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "main.h"


/* 输出错误信息并退出进程 */
static void die(const char *msg){
    perror(msg);
    exit(EXIT_FAILURE);
}

/*
 * 初始化 TCP 服务器
 * 依次完成 socket -> setsockopt -> bind -> listen
 * 返回监听套接字文件描述符，失败时退出进程
 */
int Server_init(void){
    /* 创建 TCP 套接字 */
    int serverfd = socket(AF_INET,SOCK_STREAM,0);
    if(serverfd < 0)
        die("socket");

    /* 允许端口复用，避免 TIME_WAIT 状态下重启失败 */
    int opt = 1;
    if(setsockopt(serverfd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt)) < 0){
        close(serverfd);
        die("setsockopt");
    }

    /* 绑定到本地所有地址的 PORT 端口 */
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr = htonl(INADDR_ANY)
    };

    if(bind(serverfd,(struct sockaddr*)&addr,sizeof(addr)) < 0){
        close(serverfd);
        die("bind");
    }

    /* 开始监听，等待连接 */
    if(listen(serverfd,MAX_NUM) < 0){
        close(serverfd);
        die("listen");
    }
    printf("开始监听，端口%d\n",PORT);
    return serverfd;
}