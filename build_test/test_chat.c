/* 聊天状态机功能测试：把临时文件当 socket，驱动 chat_on_bytes，
 * 验证昵称、建房、加入、广播、退房、QUIT 全链路。 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "Chat.h"
#include "main.h"

static int fake_sock(const char* path) {
  unlink(path);
  return open(path, O_RDWR | O_CREAT, 0644);
}

static void feed(int fd, const char* s) {
  int r = chat_on_bytes(fd, s, strlen(s));
  printf("  feed(\"%s\") -> %d\n", s, r);
}

static const char* dump(const char* path) {
  static char buf[8192];
  int fd = open(path, O_RDONLY);
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  buf[n > 0 ? n : 0] = '\0';
  return buf;
}

int main(void) {
  int A = fake_sock("chat_A.tmp");
  int B = fake_sock("chat_B.tmp");
  printf("fake fds: A=%d B=%d\n", A, B);

  chat_init(MAX_CONN);
  chat_fd_assign(A, 0);
  chat_fd_assign(B, 1);
  chat_on_accept(A);
  chat_on_accept(B);

  printf("--- 昵称注册（含重名测试）---\n");
  feed(A, "alice\n");
  feed(B, "alice\n");   /* 重名应被拒 */
  feed(B, "bob\n");

  printf("--- 建房与加入 ---\n");
  feed(A, "JOIN lobby\n");
  feed(B, "JOIN lobby\n");
  feed(B, "hello everyone\n");
  feed(A, "hi bob\n");

  printf("--- 换房 ---\n");
  feed(B, "JOIN secret\n");
  feed(A, "ping from lobby\n");  /* bob 已离开 lobby，不应收到 */
  feed(B, "/quit");
  feed(B, "\n");

  printf("--- 断开回收 ---\n");
  feed(A, "QUIT\n");

  printf("\n===== A 收到的内容 =====\n%s\n", dump("chat_A.tmp"));
  printf("===== B 收到的内容 =====\n%s\n", dump("chat_B.tmp"));

  chat_on_close(A);
  chat_on_close(B);
  return 0;
}
