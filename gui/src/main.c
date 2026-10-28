/*
 * 聊天室快照面板 —— LVGL 9.6 / fbdev + evdev / RK3566
 *
 * 数据链：聊天服务器 -> room_broadcast() -> write(/dev/chatsnap) -> 内核 kfifo -> 本面板 read()
 * 面板只读设备（O_RDONLY|O_NONBLOCK），触摸用来清屏 / 刷新统计（顺便验证 evdev 触摸链路）。
 * 驱动没装/没权限时面板照常运行，只提示"未就绪"并每 50ms 重试连接（不阻塞界面）。
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <lvgl/lvgl.h>

#include "chatsnap_uapi.h"   /* 与驱动共用同一份 ioctl 定义，别手抄命令号 */

LV_FONT_DECLARE(lv_font_chat_16);   /* 自生成中文字库（含 ASCII） */
LV_FONT_DECLARE(lv_font_chat_24);

#define FB_DEV    "/dev/fb0"
#define TOUCH_DEV "/dev/input/event1"
#define SNAP_DEV  CHATSNAP_DEV          /* "/dev/chatsnap" */

/* 设备路径可用环境变量覆盖：驱动没装时也能用 FIFO/文件回放来调界面
 *   mkfifo /tmp/fake_snap && CHATSNAP_DEV=/tmp/fake_snap ./chat_gui */
static const char* snap_path(void) {
  const char* e = getenv("CHATSNAP_DEV");
  return (e && *e) ? e : SNAP_DEV;
}

#define MAX_LINES 200                   /* 屏上最多保留多少条消息 */
#define GUI_LINE_MAX 1024   /* 别叫 LINE_MAX：<limits.h> 里已有 POSIX 的 _POSIX2_LINE_MAX */

static int       g_fd = -1;
static lv_obj_t* g_list;                /* 消息滚动区 */
static lv_obj_t* g_dev_lb;              /* 驱动连接状态 */
static lv_obj_t* g_stat_lb;             /* 驱动统计 */
static char      g_cur[GUI_LINE_MAX];        /* 半行缓冲：设备给的是字节流 */
static size_t    g_cur_len;
static unsigned  g_shown;

/* 等价 MCU 上 SysTick 每 1ms 调 lv_tick_inc */
static uint32_t tick_cb(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* ============================ 消息区 ============================ */

static void list_push(const char* text) {
  lv_obj_t* lb = lv_label_create(g_list);
  lv_label_set_text(lb, text);
  lv_obj_set_style_text_font(lb, &lv_font_chat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(lb, lv_color_hex(0xDCE3EA), LV_PART_MAIN);
  lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lb, lv_pct(100));

  if (++g_shown > MAX_LINES) {                 /* 超上限删最旧那条，内存不涨 */
    lv_obj_delete(lv_obj_get_child(g_list, 0));
    g_shown--;
  }
  lv_obj_scroll_to_view(lb, LV_ANIM_OFF);      /* 自动跟到最新 */
}

static void list_clear(void) {
  lv_obj_clean(g_list);
  g_shown = 0;
  g_cur_len = 0;
}

/* 把设备给出的字节流按 \n 切成行 */
static void feed(const char* buf, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char c = buf[i];
    if (c == '\n') {
      g_cur[g_cur_len] = '\0';
      if (g_cur_len)
        list_push(g_cur);
      g_cur_len = 0;
    } else if (c == '\r') {
      continue;
    } else if (g_cur_len < GUI_LINE_MAX - 1) {
      g_cur[g_cur_len++] = c;
    } else {
      g_cur[g_cur_len] = '\0';                 /* 病态超长行：先吐出去，别把缓冲撑爆 */
      list_push(g_cur);
      g_cur_len = 0;
    }
  }
}

/* ============================ 设备 ============================ */

static void dev_status(const char* txt, uint32_t color) {
  lv_label_set_text(g_dev_lb, txt);
  lv_obj_set_style_text_color(g_dev_lb, lv_color_hex(color), LV_PART_MAIN);
}

static void dev_attach(void) {
  if (g_fd >= 0)
    return;
  g_fd = open(snap_path(), O_RDONLY | O_NONBLOCK);
  if (g_fd >= 0) {
    char msg[128];
    snprintf(msg, sizeof msg, "驱动: 已连接 %s", snap_path());
    dev_status(msg, 0x81C784);
  }
}

/* 每 50ms 非阻塞读一次；没有数据立刻返回，绝不卡住 UI */
static void dev_poll(lv_timer_t* t) {
  (void)t;
  if (g_fd < 0) {
    dev_attach();
    return;
  }
  char buf[1024];
  for (;;) {
    ssize_t n = read(g_fd, buf, sizeof buf);
    if (n > 0) {
      feed(buf, (size_t)n);
      continue;
    }
    if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK)
      break;
    if (errno == EINTR)
      continue;
    close(g_fd);                     /* 驱动被卸载等 -> 回到未连接，稍后自动重试 */
    g_fd = -1;
    {
      char msg[128];
      snprintf(msg, sizeof msg, "驱动: 未就绪，等待 %s", snap_path());
      dev_status(msg, 0xFFB300);
    }
    break;
  }
}

static void stat_show(void) {
  if (g_fd < 0) {
    lv_label_set_text(g_stat_lb, "写入 --   读出 --   丢弃 --   缓存 --");
    return;
  }
  struct chatsnap_stat st;
  if (ioctl(g_fd, CHATSNAP_GET_STAT, &st) < 0)
    return;
  lv_label_set_text_fmt(g_stat_lb,
                        "写入 %llu B   读出 %llu B   丢弃 %llu 条   缓存 %u/%u B",
                        (unsigned long long)st.in, (unsigned long long)st.out,
                        (unsigned long long)st.drop, (unsigned)st.len, (unsigned)st.size);
}

static void stat_timer(lv_timer_t* t) {
  (void)t;
  stat_show();
}

/* ============================ 触摸按钮 ============================ */

static void on_clear(lv_event_t* e) {
  (void)e;
  list_clear();
  if (g_fd >= 0) {
    struct chatsnap_stat st;
    (void)ioctl(g_fd, CHATSNAP_CLEAR, &st);      /* 顺带把驱动缓冲也清了 */
  }
  list_push("[面板] 已清空屏幕与驱动缓冲");
  stat_show();
}

static void on_stat(lv_event_t* e) {
  (void)e;
  stat_show();
}

/* ============================ 界面 ============================ */

static lv_obj_t* mk_label(lv_obj_t* parent, const char* txt, const lv_font_t* font,
                          uint32_t color) {
  lv_obj_t* lb = lv_label_create(parent);
  lv_obj_set_style_text_font(lb, font, LV_PART_MAIN);   /* 含中文的标签必须用中文字体 */
  lv_obj_set_style_text_color(lb, lv_color_hex(color), LV_PART_MAIN);
  lv_label_set_text(lb, txt);
  return lb;
}

static lv_obj_t* mk_button(lv_obj_t* parent, const char* txt, int x, lv_event_cb_t cb) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, 210, 68);
  lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, 148);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x1F6FEB), LV_PART_MAIN);
  lv_obj_set_style_radius(b, 10, LV_PART_MAIN);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* t = mk_label(b, txt, &lv_font_chat_16, 0xFFFFFF);
  lv_obj_center(t);
  return b;
}

static void build_ui(void) {
  lv_obj_t* scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E1116), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_t* title = mk_label(scr, "聊天室快照面板", &lv_font_chat_24, 0xFFFFFF);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 18);

  lv_obj_t* sub = mk_label(scr, "LVGL 9.6 · fbdev /dev/fb0 · 数据源 /dev/chatsnap",
                           &lv_font_chat_16, 0x7A8794);
  lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 24, 56);

  g_dev_lb = mk_label(scr, "驱动: 未就绪，等待 /dev/chatsnap", &lv_font_chat_16, 0xFFB300);
  lv_obj_align(g_dev_lb, LV_ALIGN_TOP_LEFT, 24, 88);

  g_stat_lb = mk_label(scr, "写入 --   读出 --   丢弃 --   缓存 --", &lv_font_chat_16, 0x4FC3F7);
  lv_obj_align(g_stat_lb, LV_ALIGN_TOP_LEFT, 24, 114);

  mk_button(scr, "清空屏幕", 24, on_clear);
  mk_button(scr, "刷新统计", 254, on_stat);

  g_list = lv_obj_create(scr);
  lv_obj_set_size(g_list, 720, 1000);
  lv_obj_align(g_list, LV_ALIGN_TOP_MID, 0, 238);
  lv_obj_set_style_bg_color(g_list, lv_color_hex(0x151A21), LV_PART_MAIN);
  lv_obj_set_style_border_width(g_list, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(g_list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(g_list, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_row(g_list, 8, LV_PART_MAIN);
  lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_ACTIVE);

  lv_screen_load(scr);   /* 漏了这行 = 屏幕上只有默认主题的空白屏 */
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);   /* 被 Ctrl+C 杀掉时日志才不丢 */
  printf("[INFO] 聊天室快照面板启动\n");

  lv_init();
  lv_tick_set_cb(tick_cb);

  lv_display_t* disp = lv_linux_fbdev_create();
  if (disp == NULL) {
    fprintf(stderr, "[ERR] 创建 fbdev display 失败\n");
    return 1;
  }
  if (lv_linux_fbdev_set_file(disp, FB_DEV) != LV_RESULT_OK) {
    fprintf(stderr, "[ERR] 打开 %s 失败（桌面还占着？）\n", FB_DEV);
    return 1;
  }
  printf("[INFO] display %dx%d\n", lv_display_get_horizontal_resolution(disp),
         lv_display_get_vertical_resolution(disp));

#if LV_USE_EVDEV
  if (lv_evdev_create(LV_INDEV_TYPE_POINTER, TOUCH_DEV) == NULL)
    fprintf(stderr, "[WARN] 触摸 %s 打不开（不在 input 组？），界面仍可显示\n", TOUCH_DEV);
  else
    printf("[INFO] touch %s\n", TOUCH_DEV);
#endif

  build_ui();
  dev_attach();
  list_push("[面板] 等待聊天室消息…");
  lv_timer_create(dev_poll, 50, NULL);       /* 50ms 拉一次设备 */
  lv_timer_create(stat_timer, 1000, NULL);   /* 1s 刷一次统计 */

  for (;;) {
    uint32_t idle = lv_timer_handler();
    if (idle < 1)
      idle = 1;
    if (idle > 30)
      idle = 30;
    usleep(idle * 1000);
  }
  return 0;
}
