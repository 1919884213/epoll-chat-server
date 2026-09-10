#include "Client.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "main.h"

int client_connect(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return -1;
  }
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(PORT)};
  inet_pton(AF_INET, SERVERADDR, &addr.sin_addr);
  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("connect");
    close(fd);
    return -1;
  }
  return fd;
}

void* client_worker(void* arg) {
  int id = *(int*)arg;
  free(arg);

  int fd = client_connect();
  if (fd < 0)
    return NULL;

  char buf[128];
  for (int i = 0; i < 3; i++) {
    const char* msg = "PING\n";
    if (write(fd, msg, strlen(msg)) < 0) {
      perror("write");
      break;
    }
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = '\0';
      printf("[client %d] PING -> %s", id, buf);
    } else if (n == 0) {
      printf("[client %d] server closed\n", id);
      break;
    } else {
      perror("read");
      break;
    }
  }

  close(fd);
  return NULL;
}

int main(int argc, char** argv) {
  int conns = argc > 1 ? atoi(argv[1]) : 3;
  if (conns <= 0)
    conns = 3;

  printf("[client] connecting %d times to %s:%d\n", conns, SERVERADDR, PORT);
  for (int i = 0; i < conns; i++) {
    int* id = (int*)malloc(sizeof(int));
    if (id == NULL)
      continue;
    *id = i;
    pthread_t t;
    if (pthread_create(&t, NULL, client_worker, id) != 0) {
      perror("pthread_create");
      free(id);
      continue;
    }
    pthread_detach(t);
  }

  sleep(3);  // 等待所有测试线程完成
  return 0;
}
