#include "Client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "main.h"

static int client_connect(const char* host, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return -1;
  }
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
  if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
    fprintf(stderr, "invalid address: %s\n", host);
    close(fd);
    return -1;
  }
  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("connect");
    close(fd);
    return -1;
  }
  return fd;
}

/*
 * 交互式聊天客户端：用 select() 同时监听标准输入与服务端 socket，
 * 单线程即可做到「边收消息边敲字」。
 *   ./Client [连接数] [服务器IP] [端口]
 * 连接数 > 1 时退化为并发压测模式（每个连接一个线程，只发 PING）。
 */
int main(int argc, char** argv) {
  int conns = argc > 1 ? atoi(argv[1]) : 1;
  const char* host = argc > 2 ? argv[2] : SERVERADDR;
  int port = argc > 3 ? atoi(argv[3]) : PORT;

  if (conns > 1) {
    printf("[client] 并发模式：%d 个连接到 %s:%d\n", conns, host, port);
    for (int i = 0; i < conns; i++) {
      int fd = client_connect(host, port);
      if (fd >= 0) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "user_%d\nQUIT\n", i);
        /* 简单登记昵称后退出不粘住 */
        write(fd, cmd, strlen(cmd));
        printf("[client %d] connected (fd %d)\n", i, fd);
      }
      usleep(10000);
    }
    sleep(1);
    return 0;
  }

  int fd = client_connect(host, port);
  if (fd < 0)
    return 1;
  printf("[client] 已连接 %s:%d，输入昵称开始聊天 (QUIT 退出)\n", host, port);

  char buf[BUF_SIZE];
  while (1) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    FD_SET(fd, &rfds);

    int maxfd = (fd > STDIN_FILENO ? fd : STDIN_FILENO) + 1;
    int r = select(maxfd, &rfds, NULL, NULL, NULL);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      perror("select");
      break;
    }

    if (FD_ISSET(fd, &rfds)) {
      ssize_t n = read(fd, buf, sizeof(buf) - 1);
      if (n <= 0) {
        printf("\n[client] 服务端已关闭连接\n");
        break;
      }
      buf[n] = '\0';
      fwrite(buf, 1, (size_t)n, stdout);
      fflush(stdout);
    }

    if (FD_ISSET(STDIN_FILENO, &rfds)) {
      if (!fgets(buf, sizeof(buf), stdin))
        break;
      size_t len = strlen(buf);
      if (len > 0 && buf[len - 1] == '\n')
        buf[--len] = '\0';
      if (write(fd, buf, len) < 0 || write(fd, "\n", 1) < 0) {
        perror("write");
        break;
      }
      if (strcasecmp(buf, "QUIT") == 0 || strcasecmp(buf, "/exit") == 0)
        break;
    }
  }

  close(fd);
  return 0;
}
