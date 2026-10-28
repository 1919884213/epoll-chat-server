#include "Server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "Chat.h"
#include "main.h"

/* 每个连接的处理上下文，作为任务参数传给 worker */
typedef struct Conn {
  int fd;
  int epfd;
  int worker_index;
} Conn;

/* 全局 epoll 实例，供 worker 重新武装 fd 时使用 */
static int g_epfd = -1;

static void die(const char* msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}

/* 将 fd 设为非阻塞 */
static void set_nonblock(int fd) {
  int opt = fcntl(fd, F_GETFL);
  if (opt < 0)
    die("fcntl F_GETFL");
  if (fcntl(fd, F_SETFL, opt | O_NONBLOCK) < 0)
    die("fcntl F_SETFL");
}

/* 关闭连接：聊天状态清理、从 epoll 摘除、关闭 fd、回收归属表项 */
static void close_conn(Conn* c) {
  chat_on_close(c->fd);
  epoll_ctl(c->epfd, EPOLL_CTL_DEL, c->fd, NULL);
  close(c->fd);
  chat_fd_release(c->fd);
  printf("[INFO] client fd %d disconnected\n", c->fd);
  free(c);
}

/* worker 中的连接处理：边沿触发需读到 EAGAIN，
 * 把原始字节喂给聊天状态机，处理完重新武装 ONESHOT */
static void handle_conn(void* arg) {
  Conn* c = (Conn*)arg;
  char buf[BUF_SIZE];
  int close_req = 0;

  while (!close_req) {
    ssize_t n = read(c->fd, buf, sizeof(buf));
    if (n > 0) {
      int r = chat_on_bytes(c->fd, buf, (size_t)n);
      if (r == 1)
        close_req = 1; /* 用户 QUIT / 主动断开 */
      /* r == -1：fd 未登记（过期事件），继续读干净即可 */
    } else if (n == 0) {
      close_conn(c); /* 对端关闭 */
      return;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break; /* 数据已读干净 */
      if (errno == EINTR)
        continue;
      close_conn(c);
      return;
    }
  }

  if (close_req) {
    close_conn(c);
    return;
  }

  /* 重新武装，等待下一次可读事件 */
  struct epoll_event ev;
  ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
  ev.data.fd = c->fd;
  if (epoll_ctl(c->epfd, EPOLL_CTL_MOD, c->fd, &ev) < 0) {
    if (errno != EEXIST)
      close_conn(c);
  }
}

/* 初始化 TCP 服务器 */
int Server_init(void) {
  int serverfd = socket(AF_INET, SOCK_STREAM, 0);
  if (serverfd < 0)
    die("socket");

  int opt = 1;
  if (setsockopt(serverfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    close(serverfd);
    die("setsockopt");
  }

  struct sockaddr_in addr = {.sin_family = AF_INET,
                             .sin_port = htons(PORT),
                             .sin_addr.s_addr = htonl(INADDR_ANY)};

  if (bind(serverfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(serverfd);
    die("bind");
  }
  if (listen(serverfd, MAX_NUM) < 0) {
    close(serverfd);
    die("listen");
  }
  printf("[INFO] chat server listening on port %d\n", PORT);
  return serverfd;
}

void server_run(int serverfd, Threadpool* pool) {
  set_nonblock(serverfd);
  chat_init(MAX_CONN);

  g_epfd = epoll_create1(0);
  if (g_epfd < 0)
    die("epoll_create1");

  struct epoll_event ev;
  ev.data.fd = serverfd;
  ev.events = EPOLLIN;  // 监听套接字用水平触发即可
  if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, serverfd, &ev) < 0)
    die("epoll_ctl add listenfd");

  struct epoll_event events[64];
  int next_worker = 0;  // round-robin 游标，决定下一个连接分给哪个 worker

  /*
   * Reactor 主循环：主线程只做 IO 事件分发，不处理聊天业务。
   *   - 监听 fd 就绪  -> 循环 accept，把新连接绑到一个 worker 并注册进 epoll
   *   - 客户端 fd 就绪 -> 打包成 Conn 任务，投递到该 fd 所属 worker 的队列
   * 真正的 read/协议解析/广播在 worker 线程的 handle_conn 中执行。
   */
  while (1) {
    int nfds = epoll_wait(g_epfd, events, 64, -1);
    if (nfds < 0) {
      if (errno == EINTR)
        continue;
      die("epoll_wait");
    }

    for (int i = 0; i < nfds; i++) {
      int fd = events[i].data.fd;

      if (fd == serverfd) {
        /* 新连接：循环 accept 到 EAGAIN，一次事件清空 accept 队列 */
        while (1) {
          struct sockaddr_in cli;
          socklen_t len = sizeof(cli);
          int cfd = accept(serverfd, (struct sockaddr*)&cli, &len);
          if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
              break;
            if (errno == EINTR)
              continue;
            perror("accept");
            break;
          }
          if (cfd >= MAX_CONN) {
            fprintf(stderr, "[WARN] fd %d exceeds MAX_CONN, rejected\n", cfd);
            close(cfd);
            continue;
          }
          set_nonblock(cfd);

          int wi = next_worker;
          next_worker = (next_worker + 1) % pool->num;
          chat_fd_assign(cfd, wi);

          struct epoll_event cev;
          cev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
          cev.data.fd = cfd;
          if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, cfd, &cev) < 0) {
            perror("epoll_ctl add clientfd");
            close(cfd);
            chat_fd_release(cfd);
            continue;
          }
          chat_on_accept(cfd);  /* 发送欢迎语，进入昵称设置状态 */
          printf("[INFO] client %s:%d connected -> worker %d\n",
                 inet_ntoa(cli.sin_addr), ntohs(cli.sin_port), wi);
        }
      } else {
        /* 客户端 fd 可读：投递给它绑定的 worker */
        int wi = chat_fd_get(fd);
        if (wi < 0)
          continue;  // 连接已被回收，忽略过期事件
        Conn* c = (Conn*)malloc(sizeof(Conn));
        if (c == NULL)
          continue;
        c->fd = fd;
        c->epfd = g_epfd;
        c->worker_index = wi;
        if (pool_submit(pool, wi, handle_conn, c) != 0)
          close_conn(c);
      }
    }
  }
}
