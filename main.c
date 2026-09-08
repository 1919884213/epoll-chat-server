#include "main.h"

static void
set_nonblock(int fd)
{
  int opt = fcntl(fd, F_GETFL);
  fcntl(fd, F_SETFL, opt | O_NONBLOCK);
};

struct sockaddr_in clientAddr;
socklen_t client_len = sizeof(clientAddr);
static char buff[32];

int main(){
  int serverfd = Server_init(), clientfd;
  /*设置为非阻塞*/
  set_nonblock(serverfd);
  /*创建 作为epoll_ctl的参数*/
  int epollfd, nfds;
  struct epoll_event ev, events[5];
  epollfd = epoll_create1(0);
  /*将fd加入池 有客户端连接时触发*/
  ev.data.fd = serverfd;
  ev.events = EPOLLIN; // fd就绪事件
  /*开始监听*/
  epoll_ctl(epollfd, EPOLL_CTL_ADD, serverfd, &ev);
  /*客户端逻辑*/
  pid_t pid = fork();
  if (pid == 0)
    for (int i = 0; i < 2; i++)
      client_init();
  else {
    while (1) {
      int nfds = epoll_wait(epollfd, events, 5, -1);
      /*第一次循环只走这个过程
       * 第n次循环时不仅有新的连接，可能还有旧客户端的消息*/
      for (int i = 0; i < nfds; i++) {
        if (events[i].data.fd == serverfd) {
          clientfd =
            accept(serverfd, (struct sockaddr*)&clientAddr, &client_len);
          set_nonblock(clientfd);
          printf("客户端%d已经连接\n", ntohs(clientAddr.sin_port));
          /*将clientfd加入epoll池*/
          ev.data.fd = clientfd;
          ev.events = EPOLLIN | EPOLLET;
          epoll_ctl(epollfd, EPOLL_CTL_ADD, clientfd, &ev);
        } else if (events[i].events & EPOLLIN) {
          clientfd = events[i].data.fd;
          memset(buff, 0, sizeof(buff));
          read(clientfd, buff, sizeof(buff));
          printf("客户端消息:%s\n", buff);
        }
      }
    }
  }

  close(serverfd);
  return 0;
}
