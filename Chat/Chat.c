#include "Chat.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "main.h"

/* ============================ 数据结构 ============================ */

#define NICK_LEN   24
#define ROOM_NAME  24
#define ROOM_CAP   8      /* 房间数量上限 */
#define LINE_MAX   1024   /* 单条聊天消息最大长度 */

typedef enum {
  ST_NICK = 0,   /* 刚连上：等待设置昵称 */
  ST_MENU,       /* 主菜单：JOIN room / LIST / QUIT */
  ST_ROOM        /* 已进房：普通行=聊天消息，/quit 退房，/exit 断线 */
} State;

typedef struct {
  int   used;                 /* 槽位是否被占用（fd 是否登记） */
  int   worker;               /* 绑定的 worker 下标，-1 空闲 */
  State st;
  char  nick[NICK_LEN];
  int   room_idx;             /* -1 = 不在房间 */
  char  line[LINE_MAX];       /* 半行缓存：ET 读到的字节可能不含完整行 */
  int   line_len;
  int   overflow;             /* 超长行标记：剩余部分直接丢弃 */
} Conn;

typedef struct {
  int  used;
  char name[ROOM_NAME];
  int  members[MAX_CONN];     /* 成员 fd 列表 */
  int  member_cnt;
} Room;

static Conn   g_conn[MAX_CONN];
static Room   g_room[ROOM_CAP];
static int    g_room_cnt = 0;
static size_t g_cap = 0;

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ============================ 基础工具 ============================ */

/* 非阻塞写：socket 缓冲区满就丢弃（聊天室允许丢消息，绝不允许卡线程）。
 * 简单重试若干次，给内核一点排空时间。 */
static void send_to(int fd, const char* data, size_t len) {
  /* 持全局锁发送，重试次数必须小：8 次 ≈ 8ms 封顶，防止拖死整个服务器 */
  for (int tries = 0; tries < 8 && len > 0;) {
    ssize_t n = write(fd, data, len);
    if (n > 0) {
      data += n;
      len -= (size_t)n;
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      usleep(1000);
      tries++;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    return; /* 对端已断开等错误：静默忽略，事件循环会负责回收 */
  }
}

static void send_str(int fd, const char* s) {
  send_to(fd, s, strlen(s));
}

/* 房间内广播（调用方必须持锁） */
static void room_broadcast(Room* r, const char* data, size_t len) {
  for (int i = 0; i < r->member_cnt; i++)
    send_to(r->members[i], data, len);
}

/* ============================ 状态机 ============================ */

static void print_menu(int fd) {
  send_str(fd,
           "命令:\n"
           "  JOIN <房间名>   加入/创建房间\n"
           "  LIST          列出房间与人数\n"
           "  QUIT          断开连接\n");
}

static Room* room_find(const char* name) {
  for (int i = 0; i < g_room_cnt; i++)
    if (g_room[i].used && strcmp(g_room[i].name, name) == 0)
      return &g_room[i];
  return NULL;
}

static void do_join(Conn* c, int fd, const char* name) {
  while (*name == ' ')
    name++;
  if (name[0] == '\0') {
    send_str(fd, "[错误] 用法: JOIN <房间名>\n");
    return;
  }
  Room* r = room_find(name);
  if (r == NULL) {
    /* 优先复用已回收的空洞槽位 */
    r = NULL;
    for (int i = 0; i < g_room_cnt; i++)
      if (!g_room[i].used) {
        r = &g_room[i];
        break;
      }
    if (r == NULL) {
      if (g_room_cnt >= ROOM_CAP) {
        send_str(fd, "[错误] 房间数量已达上限\n");
        return;
      }
      r = &g_room[g_room_cnt++];
    }
    r->used = 1;
    snprintf(r->name, sizeof(r->name), "%s", name);
    r->member_cnt = 0;
  }
  /* 同名重入直接当作已在房内 */
  if (c->room_idx >= 0 && &g_room[c->room_idx] == r) {
    c->st = ST_ROOM;
    send_str(fd, "你已经在这个房间了\n");
    return;
  }
  /* 已在别的房间则先退出 */
  if (c->room_idx >= 0) {
    Room* old = &g_room[c->room_idx];
    for (int i = 0; i < old->member_cnt; i++)
      if (old->members[i] == fd) {
        old->members[i] = old->members[old->member_cnt - 1];
        old->member_cnt--;
        break;
      }
    char msg[256];
    int n = snprintf(msg, sizeof(msg), "[系统] %s 离开了 %s\n", c->nick,
                     old->name);
    if (old->member_cnt > 0)
      room_broadcast(old, msg, (size_t)n);
    else
      old->used = 0; /* 空房间回收槽位（保留空洞，room_find 会跳过） */
    c->room_idx = -1;
  }
  r->members[r->member_cnt++] = fd;
  c->room_idx = (int)(r - g_room);
  c->st = ST_ROOM;

  char head[256];
  int hn = snprintf(head, sizeof(head), "已进入房间 [%s]，当前 %d 人：",
                    r->name, r->member_cnt);
  for (int i = 0; i < r->member_cnt; i++) {
    hn += snprintf(head + hn, sizeof(head) - hn, " %s", g_conn[r->members[i]].nick);
    if (hn >= (int)sizeof(head) - 32)
      break;
  }
  hn += snprintf(head + hn, sizeof(head) - hn, "\n");
  send_to(fd, head, (size_t)hn);

  char msg[256];
  int n = snprintf(msg, sizeof(msg), "[系统] %s 加入了房间\n", c->nick);
  room_broadcast(r, msg, (size_t)n);
}

static void do_list(int fd) {
  send_str(fd, "房间列表:\n");
  for (int i = 0; i < g_room_cnt; i++) {
    if (!g_room[i].used)
      continue;
    char line[96];
    int n = snprintf(line, sizeof(line), "  [%s] %d 人\n", g_room[i].name,
                     g_room[i].member_cnt);
    send_to(fd, line, (size_t)n);
  }
  send_str(fd, "输入 JOIN <房间名> 加入\n");
}

static void leave_room(Conn* c, int fd) {
  if (c->room_idx < 0)
    return;
  Room* r = &g_room[c->room_idx];
  for (int i = 0; i < r->member_cnt; i++) {
    if (r->members[i] == fd) {
      r->members[i] = r->members[r->member_cnt - 1];
      r->member_cnt--;
      break;
    }
  }
  char msg[256];
  int n = snprintf(msg, sizeof(msg), "[系统] %s 离开了房间\n", c->nick);
  room_broadcast(r, msg, (size_t)n);
  if (r->member_cnt == 0)
    r->used = 0;
  c->room_idx = -1;
  c->st = ST_MENU;
  print_menu(fd);
}

/* 处理一条完整命令/消息（调用方持锁） */
static void handle_line(Conn* c, int fd, char* line) {
  /* 去掉首尾空白 */
  while (*line == ' ' || *line == '\t')
    line++;
  size_t len = strlen(line);
  while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' ||
                     line[len - 1] == '\r'))
    line[--len] = '\0';
  if (len == 0)
    return;

  switch (c->st) {
    case ST_NICK: {
      if (len >= NICK_LEN) {
        send_str(fd, "[错误] 昵称太长(≤23字符)，请重新输入:\n");
        return;
      }
      if (strpbrk(line, "\r\n") != NULL) { /* 防御：昵称不允许换行 */
        send_str(fd, "[错误] 昵称含非法字符，请重新输入:\n");
        return;
      }
      /* 昵称查重 */
      for (size_t i = 0; i < g_cap; i++) {
        if (g_conn[i].used && strcmp(g_conn[i].nick, line) == 0) {
          send_str(fd, "[错误] 昵称已被占用，换一个:\n");
          return;
        }
      }
      snprintf(c->nick, sizeof(c->nick), "%s", line);
      c->st = ST_MENU;
      char msg[128];
      int n = snprintf(msg, sizeof(msg), "欢迎 %s！\n", c->nick);
      send_to(fd, msg, (size_t)n);
      print_menu(fd);
      return;
    }
    case ST_MENU: {
      if (strlen(line) >= 4 && strncasecmp(line, "JOIN", 4) == 0 &&
          (line[4] == '\0' || line[4] == ' ')) {
        do_join(c, fd, line[4] ? line + 5 : "");
      } else if (strcasecmp(line, "LIST") == 0) {
        do_list(fd);
      } else if (strcasecmp(line, "QUIT") == 0) {
        send_str(fd, "再见！\n");
        g_conn[fd].used = -1; /* 标记：让 chat_on_bytes 返回 1 要求关闭 */
      } else {
        send_str(fd, "未知命令。JOIN <房间名> / LIST / QUIT\n");
      }
      return;
    }
    case ST_ROOM: {
      if (strcasecmp(line, "/quit") == 0 || strcasecmp(line, "QUIT") == 0) {
        leave_room(c, fd);
        return;
      }
      if (strcasecmp(line, "/exit") == 0 || strcasecmp(line, "EXIT") == 0) {
        g_conn[fd].used = -1; /* 触发关闭 */
        return;
      }
      if ((strlen(line) >= 4 && strncasecmp(line, "JOIN", 4) == 0 &&
           (line[4] == '\0' || line[4] == ' ')) ||
          strcasecmp(line, "LIST") == 0) {
        /* 房内允许直接换房/查房 */
        if (strcasecmp(line, "LIST") == 0)
          do_list(fd);
        else
          do_join(c, fd, line[4] ? line + 5 : "");
        return;
      }
      char msg[LINE_MAX + 96];
      int n = snprintf(msg, sizeof(msg), "[%s] %s: %s\n",
                       g_room[c->room_idx].name, c->nick, line);
      if (n < 0)
        return;
      room_broadcast(&g_room[c->room_idx], msg, (size_t)n);
      return;
    }
  }
}

/* ============================ 对外接口 ============================ */

void chat_init(int cap) {
  g_cap = (size_t)cap;
  for (int i = 0; i < cap; i++) {
    g_conn[i].used = 0;
    g_conn[i].worker = -1;
    g_conn[i].room_idx = -1;
  }
  memset(g_room, 0, sizeof(g_room));
}

void chat_fd_assign(int fd, int worker) {
  pthread_mutex_lock(&g_mtx);
  if (fd >= 0 && fd < (int)g_cap)
    g_conn[fd].worker = worker;
  pthread_mutex_unlock(&g_mtx);
}

int chat_fd_get(int fd) {
  pthread_mutex_lock(&g_mtx);
  int w = (fd >= 0 && fd < (int)g_cap) ? g_conn[fd].worker : -1;
  pthread_mutex_unlock(&g_mtx);
  return w;
}

void chat_fd_release(int fd) {
  pthread_mutex_lock(&g_mtx);
  if (fd >= 0 && fd < (int)g_cap) {
    g_conn[fd].worker = -1;
    g_conn[fd].used = 0;
  }
  pthread_mutex_unlock(&g_mtx);
}

void chat_on_accept(int fd) {
  pthread_mutex_lock(&g_mtx);
  if (fd < 0 || fd >= (int)g_cap) {
    pthread_mutex_unlock(&g_mtx);
    return;
  }
  Conn* c = &g_conn[fd];
  c->used = 1;
  c->st = ST_NICK;
  c->room_idx = -1;
  c->nick[0] = '\0';
  c->line_len = 0;
  c->overflow = 0;
  pthread_mutex_unlock(&g_mtx);
  send_str(fd, "欢迎来到 epoll 聊天室，请设置昵称:\n");
}

int chat_on_bytes(int fd, const char* buf, size_t len) {
  pthread_mutex_lock(&g_mtx);
  if (fd < 0 || fd >= (int)g_cap || !g_conn[fd].used) {
    pthread_mutex_unlock(&g_mtx);
    return -1;
  }
  Conn* c = &g_conn[fd];
  int close_req = 0;

  for (size_t i = 0; i < len && !close_req; i++) {
    char ch = buf[i];
    if (ch == '\n') {
      if (c->line_len < LINE_MAX)
        c->line[c->line_len] = '\0';
      if (!c->overflow && c->line_len > 0)
        handle_line(c, fd, c->line);
      if (c->used == -1)
        close_req = 1;
      c->line_len = 0;
      c->overflow = 0;
    } else if (c->line_len < LINE_MAX - 1) {
      c->line[c->line_len++] = ch;
    } else {
      c->overflow = 1; /* 超长行：保留前半截也没意义，标记后整行丢弃 */
    }
  }
  pthread_mutex_unlock(&g_mtx);
  return close_req;
}

void chat_on_close(int fd) {
  pthread_mutex_lock(&g_mtx);
  if (fd < 0 || fd >= (int)g_cap || !g_conn[fd].used) {
    pthread_mutex_unlock(&g_mtx);
    return;
  }
  Conn* c = &g_conn[fd];
  if (c->st == ST_ROOM)
    leave_room(c, fd);
  c->used = 0;
  c->worker = -1;
  c->nick[0] = '\0';
  c->line_len = 0;
  c->overflow = 0;
  pthread_mutex_unlock(&g_mtx);
}
