#include "Server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "main.h"

/* 每个连接的处理上下文，作为任务参数传给 worker */
typedef struct Conn {
  int fd;
  int epfd;
  int worker_index;
} Conn;

/* fd -> 绑定的 worker 下标，-1 表示空闲；由主线程在 accept 时分配 */
static int g_worker_of_fd[MAX_CONN];
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

/* 关闭连接：从 epoll 摘除、关闭 fd、回收连接表项与上下文 */
static void close_conn(Conn* c) {
  epoll_ctl(c->epfd, EPOLL_CTL_DEL, c->fd, NULL);
  close(c->fd);
  if (c->fd >= 0 && c->fd < MAX_CONN)
    g_worker_of_fd[c->fd] = -1;
  printf("[INFO] client fd %d disconnected\n", c->fd);
  free(c);
}

/* 判断一行内容（忽略尾部空白）是否为 PING */
static int is_ping(const char* buf, ssize_t n) {
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                   buf[n - 1] == ' ' || buf[n - 1] == '\t'))
    n--;
  return n == 4 && strncmp(buf, "PING", 4) == 0;
}

/* worker 中的连接处理：边沿触发需读到 EAGAIN，处理完重新武装 ONESHOT */
static void handle_conn(void* arg) {
  Conn* c = (Conn*)arg;
  char buf[BUF_SIZE];

  while (1) {
    ssize_t n = read(c->fd, buf, sizeof(buf));
    if (n > 0) {
      if (is_ping(buf, n)) {
        const char* pong = "PONG\n";
        write(c->fd, pong, strlen(pong));
        printf("[INFO] worker %d fd %d: PING -> PONG\n", c->worker_index,
               c->fd);
      } else {
        /* 非 PING 消息原样回显，便于客户端校验往返 */
        if (write(c->fd, buf, n) < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
          break;
        printf("[INFO] worker %d fd %d: echo %ld bytes\n", c->worker_index,
               c->fd, (long)n);
      }
    } else if (n == 0) {
      /* 对端关闭 */
      close_conn(c);
      return;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;  // 数据已读干净
      if (errno == EINTR)
        continue;
      close_conn(c);
      return;
    }
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
  printf("[INFO] server listening on port %d\n", PORT);
  return serverfd;
}

void server_run(int serverfd, Threadpool* pool) {
  set_nonblock(serverfd);
  for (int i = 0; i < MAX_CONN; i++)
    g_worker_of_fd[i] = -1;

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
   * Reactor 主循环：主线程只做 IO 事件分发，不处理业务。
   *   - 监听 fd 就绪  -> 循环 accept，把新连接绑到一个 worker 并注册进 epoll
   *   - 客户端 fd 就绪 -> 打包成 Conn 任务，投递到该 fd 所属 worker 的队列
   * 真正的 read/write 在 worker 线程的 handle_conn 中执行。
   */
  while (1) {
    /* -1 表示无事件时永久阻塞，直到有 fd 就绪才返回；返回就绪个数 nfds */
    int nfds = epoll_wait(g_epfd, events, 64, -1);
    if (nfds < 0) {
      if (errno == EINTR)  // 被信号中断，属正常情况，重试即可
        continue;
      die("epoll_wait");
    }

    for (int i = 0; i < nfds; i++) {
      int fd = events[i].data.fd;  // 事件对应的 fd

      if (fd == serverfd) {
        /*
         * 监听套接字可读：有新连接到来。
         * 监听 fd 用水平触发（EPOLLIN），但这里仍循环 accept 到 EAGAIN，
         * 以便一次事件就把 accept 队列清空，避免惊群/遗漏。
         */
        while (1) {
          struct sockaddr_in cli;
          socklen_t len = sizeof(cli);
          int cfd = accept(serverfd, (struct sockaddr*)&cli, &len);
          if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
              break;  // 已无待处理连接，退出循环
            if (errno == EINTR)
              continue;  // 被信号打断，重试本次 accept
            perror("accept");
            break;
          }
          if (cfd >= MAX_CONN) {  // fd 超出连接表容量，无法登记
            fprintf(stderr, "[WARN] fd %d exceeds MAX_CONN, rejected\n", cfd);
            close(cfd);
            continue;
          }
          set_nonblock(cfd);  // 客户端 fd 设为非阻塞，配合 ET 一次读干净

          /* round-robin 绑定一个 worker，使各 worker 负载尽量均衡 */
          int wi = next_worker;
          next_worker = (next_worker + 1) % pool->num;
          g_worker_of_fd[cfd] = wi;  // 记录归属，后续事件据此投递

          /*
           * 注册客户端 fd：
           *   EPOLLET      边沿触发，只在状态变化时通知一次，需读到 EAGAIN
           *   EPOLLONESHOT 触发一次后自动失效，防止同一连接被多个线程并发处理
           * 处理完须在 handle_conn 中 MOD 重新武装，否则连接不再被触发。
           */
          struct epoll_event cev;
          cev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
          cev.data.fd = cfd;
          if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, cfd, &cev) < 0) {
            perror("epoll_ctl add clientfd");
            close(cfd);
            g_worker_of_fd[cfd] = -1;  // 注册失败，回滚归属记录
            continue;
          }
          printf("[INFO] client %s:%d connected -> worker %d\n",
                 inet_ntoa(cli.sin_addr), ntohs(cli.sin_port), wi);
        }
      } else {
        /* 客户端 fd 可读：把该 fd 的处理任务投递给它绑定的 worker */
        int wi = g_worker_of_fd[fd];
        if (wi < 0)
          continue;  // 连接已被回收（fd 已复用/关闭），忽略此过期事件
        Conn* c = (Conn*)malloc(sizeof(Conn));  // 任务上下文
        if (c == NULL)
          continue;  // 内存不足，丢弃本次事件（连接仍可等待下次触发）
        c->fd = fd;
        c->epfd = g_epfd;
        c->worker_index = wi;
        /* 入队失败（如池已停止）则立即关闭连接，避免资源泄漏 */
        if (pool_submit(pool, wi, handle_conn, c) != 0)
          close_conn(c);
      }
    }
  }
}
