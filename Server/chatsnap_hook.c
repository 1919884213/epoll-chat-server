#include "chatsnap_hook.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>   /* strerror */
#include <unistd.h>

#define SNAP_DEV "/dev/chatsnap"

static int g_fd = -1;

void chatsnap_attach(void) {
  /* O_NONBLOCK：驱动侧本来也不阻塞，这里再加一道，保证任何情况下不拖慢广播路径 */
  g_fd = open(SNAP_DEV, O_WRONLY | O_NONBLOCK);
  if (g_fd < 0)
    fprintf(stderr, "[WARN] %s 不可用(%s)，快照通道关闭，服务器继续运行\n",
            SNAP_DEV, strerror(errno));
  else
    printf("[INFO] 聊天快照通道 -> %s\n", SNAP_DEV);
}

void chatsnap_emit(const char* data, size_t len) {
  if (g_fd < 0 || len == 0)
    return;
  /* 驱动缓冲满是"整条丢弃 + 计数"，业务层不重试、不报错 —— 但返回值要接住，
   * 否则碰到 glibc 的 warn_unused_result 会报警告（(void) 强制转换压不住） */
  ssize_t n = write(g_fd, data, len);
  (void)n;
}

void chatsnap_detach(void) {
  if (g_fd >= 0) {
    close(g_fd);
    g_fd = -1;
  }
}
