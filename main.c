#include <stdio.h>
#include <unistd.h>

#include "Server.h"
#include "main.h"
#include "pool.h"

int main(void) {
  Threadpool* pool = pool_create(WORKER_NUM);
  if (pool == NULL) {
    fprintf(stderr, "pool_create failed\n");
    return 1;
  }

  int serverfd = Server_init();
  server_run(serverfd, pool);  // 事件循环，正常情况不会返回

  close(serverfd);
  pool_destroy(pool);
  return 0;
}
