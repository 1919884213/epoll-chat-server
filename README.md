<div align="center">

# epoll-chat-server

**Reactor 聊天服务器 + 自研 Linux 字符设备 + 板上 LVGL 面板**

`~1800 行 C` · `epoll ET + EPOLLONESHOT` · `固定 worker 线程池` · `Linux 字符设备驱动` · `LVGL 9.6` · `CMake`

[![OS](https://img.shields.io/badge/OS-Linux-informational?style=flat-square)](#架构)
[![Lang](https://img.shields.io/badge/Lang-C-blue?style=flat-square)](#目录结构)
[![IO](https://img.shields.io/badge/IO-epoll%20ET%20%2B%20ONESHOT-orange?style=flat-square)](#架构)
[![Pool](https://img.shields.io/badge/pool-fixed%20worker-blueviolet?style=flat-square)](#架构)
[![Kernel](https://img.shields.io/badge/kernel-chardev%20%2B%20kfifo-critical?style=flat-square)](#内核快照通道-chatsnap)
[![GUI](https://img.shields.io/badge/GUI-LVGL%209.6%20%2B%20fbdev-yellowgreen?style=flat-square)](#lvgl-面板)
[![Build](https://img.shields.io/badge/Build-CMake-green?style=flat-square)](#快速开始)
[![Verified](https://img.shields.io/badge/RK3566-16%2F16%20passed-success?style=flat-square)](#测试与验收)

</div>

---

## 项目简介

一个 Linux 多房间 TCP 聊天服务器，**三层一条链**：

```
用户态网络服务  →  内核字符设备  →  嵌入式 GUI
 epoll Reactor      /dev/chatsnap      LVGL 9.6 面板
                    (kfifo 环形缓冲)    720×1280 屏
```

- **第一层**：`epoll`（边沿触发 + `EPOLLONESHOT`）+ 固定 worker 线程池的 Reactor；单线程分发、多线程处理业务，纯 C 约 1000 行，服务器核心零第三方依赖。
- **第二层**：聊天广播的一份副本写进**自研 Linux 字符设备** `/dev/chatsnap`（kfifo 环形缓冲 + 阻塞读 + `poll` + `ioctl` + sysfs）。它是**旁路可观测通道**，不参与业务 —— 驱动缺失/无权限时服务器照常运行。
- **第三层**：RK3566 板载 720×1280 屏上的 **LVGL 9.6 面板**读这个设备，实时滚动显示聊天内容，带驱动统计与触摸按钮。

为什么要有第二层 ：**用户态服务对内核完全不可见** —— 进程被 `kill -9` 之后，最后一批聊天记录与运行状态无从取证；kfifo 在内核内存里，进程死了缓冲还在，监控端/面板照样读得到。代价是多一次 `copy_*_user`，所以它的定位是**可观测性 + 驱动能力落地**，不是性能优化。

功能：昵称注册（查重）、房间创建/加入/换房/列表、房间内实时广播、加入/离开系统通知、断线自动退房、跨包半行粘包处理。

## 亮点速览

| 亮点 | 关键实现 | 实测证据 |
|---|---|---|
| Reactor 并发模型 | 单 epoll 主线程 + 每连接绑定固定 worker + ET/ONESHOT | 50 并发连接不崩，空转 CPU 0.9% |
| 内核态旁路通道 | `/dev/chatsnap`：kfifo + 等待队列 + `poll` + ioctl + sysfs | 板子 **16/16** 项验收通过 |
| 业务零侵入 | 只在广播唯一出口加一行 `chatsnap_emit()`，缺驱动自动降级 | 无驱动时服务器照常服务 |
| 端到端闭环 | 真客户端 → 服务器 → 内核缓冲 → 板上 LVGL 面板 | 写 113 B / 读 113 B 无丢失 |
| 中文字库 | 扫真实语料自建子集，分字号收字 | 正文 3536 字 2.98 MB，标题 8 字 88 KB |
| 交叉编译 | CMake + 工具链文件 → aarch64 | 产物仅依赖 `libc.so.6` |

## 架构

```
                     主线程 (epoll_wait)
                           │  只做事件分发，不阻塞、不处理业务
           ┌───────────────┴────────────────┐
           │ listenfd 就绪                   │ clientfd 就绪
           ▼                                 ▼
    循环 accept 到 EAGAIN             查连接表找到绑定的 worker
    round-robin 绑定 worker                    │
    登记 fd->worker 归属               pool_submit(worker, 处理任务)
    注册 EPOLLET|EPOLLONESHOT                  │
           │                                   ▼
           │                            worker 线程
           │                 非阻塞读循环到 EAGAIN → 喂给聊天状态机
           │                 chat_on_bytes(): 按连接缓冲、切行、驱动协议
           │                 处理完 EPOLL_CTL_MOD 重新武装 ONESHOT
           └──────────────────────────────────┘
```

关键设计：

- **单 epoll 主线程 Reactor**：一个线程管理全部连接，事件分发与业务处理解耦。
- **每连接绑定固定 worker**（round-robin 分配）：同一 fd 的读写天然串行，无并发操作同一 socket 的问题。
- **`EPOLLONESHOT`**：fd 触发一次后自动失活，worker 处理完重新武装，保证同一连接同一时刻只有一个任务在飞。
- **`EPOLLET` 边沿触发**：循环 `read` 直到 `EAGAIN`，一次性读干净内核缓冲区。
- **全局聊天锁**：房间表/成员表/连接归属表由一把 pthread mutex 保护；广播路径全程持锁串行发送，修掉了早期 `g_worker_of_fd` 主线程与 worker 双写的 data race。
- **按连接行缓冲**：TCP 是字节流，一次 `read` 可能拿到半行或多行；状态机把字节流切成 `\n` 结尾的完整行再解析，超长行（>1KB）整行丢弃防内存滥用。
- **持锁发送设重试上限**（8 次 × 1ms）：慢客户端最多拖 8ms，不会拖死整个房间。

## 三层数据链（端到端）

```
Chat.c: room_broadcast()  ──chatsnap_emit()──▶  write(/dev/chatsnap)
    （广播唯一出口，持锁）                            │
                                                    ▼
                                       chatsnap.ko：kfifo 环形缓冲
                                                    │
                        ┌───────────────────────────┴──────────────────────┐
                        ▼                                                  ▼
        板载 LVGL 面板 read()（50ms 轮询）                      ChatSnapMon read()/poll()
        渲染到 720×1280 屏                                    终端跟踪 / -s 统计 / -c 清空
```

## 目录结构

```
.
├── main.c              # 入口：线程池 + 服务器初始化 + 事件循环（+ 快照通道 attach/detach）
├── main.h              # 全局配置（端口、worker 数、缓冲区大小等）
├── protocol.h          # 二进制协议预留（定长包头）
├── Server/             # TCP 初始化 + epoll Reactor 主循环
│   └── chatsnap_hook.c # 快照通道的用户态接入（打开/写入/关闭，失败自动降级）
├── Chat/               # 聊天核心：状态机 + 房间/成员管理 + 广播（与线程模型解耦）
├── Client/             # 交互式客户端（单线程 select 同时监听 stdin 与 socket）
├── pool/               # 线程池：每 worker 独立任务队列，按 index 定向投递
├── build_test/         # 聊天状态机功能测试（临时文件模拟 socket）
├── kernel/chatsnap/    # 自研字符设备驱动：kfifo + 阻塞读 + poll + ioctl + sysfs
├── gui/                # LVGL 9.6 面板（fbdev + evdev）+ 自建中文子集字库
│   └── src/fonts/      # charset.txt + lv_font_chat_{16,24}.c（生成物，随仓库提交）
├── tools/              # ChatSnapMon：命令行跟踪快照流 / 看统计
├── scripts/            # 验收脚本（驱动 12 项 / 端到端 5 项）+ udev 设备权限规则
└── CMakeLists.txt
```

## 快速开始

**依赖**：Linux + gcc + CMake + pthread。服务器与监控端**零第三方库**；只有 LVGL 面板需要 LVGL 源码树。

### 1) 服务器 + 客户端

```bash
cmake -S . -B build && cmake --build build -j8

./build/Pro             # 服务器，监听 0.0.0.0:8080（前台跑，事件循环不返回）
./build/Client          # 交互式客户端（另开 N 个终端 = N 个用户）
./build/Client 50       # 并发模式：50 个连接批量注册（连通性/压力）
```

> 后台跑服务器时加 `stdbuf -o0`，否则 stdout 是块缓冲、日志会全部卡在缓冲区里：
> `stdbuf -o0 -e0 ./build/Pro > pro.log 2>&1 &`

### 2) 内核快照通道（可选，缺了也不影响服务器）

```bash
cd kernel/chatsnap
make KDIR=<目标板内核树> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-   # 交叉编译
# 板子上本地编：make
sudo insmod chatsnap.ko buf_kb=64

ls -l /dev/chatsnap                       # udev 自动建节点
./ChatSnapMon                             # 跟踪实时快照流
./ChatSnapMon -s                          # 统计：写入/读出/丢弃/缓存
./ChatSnapMon -c                          # 清空缓冲并复位计数
sudo rmmod chatsnap                       # 被占用时返回 EBUSY
```

**让非 root 用户能用设备**（默认节点是 `root:root 0600`，否则面板/监控端要 sudo）：

```bash
sudo cp scripts/99-chatsnap.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo rmmod chatsnap && sudo insmod chatsnap.ko
```

### 3) LVGL 面板（RK3566 / 720×1280 / fbdev + evdev）

```bash
cd gui
cmake -S . -B build-arm64 -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake
cmake --build build-arm64 -j8              # 产物 build-arm64/chat_gui

sudo systemctl stop display-manager        # 桌面让位
CHATSNAP_DEV=/dev/chatsnap ./build-arm64/chat_gui
```

**没装驱动也能调界面**（FIFO 回放）：

```bash
mkfifo /tmp/fake && CHATSNAP_DEV=/tmp/fake ./chat_gui &
printf '[系统] alice 加入了房间\n[lobby] alice: 大家好\n' > /tmp/fake
```

## 客户端协议（行文本）

```
连接          -> "请设置昵称"
<昵称>        -> 欢迎 + 菜单
JOIN <房间>   -> 建房/加入/换房，房间内其他人收到 [系统] 通知
LIST          -> 房间列表与人数
<普通文本>    -> 房间内广播 "[房间] 昵称: 内容"
/quit  QUIT   -> 退房回菜单（未进房时 QUIT=断线）
/exit EXIT    -> 断开连接
```

纯文本行协议，`nc <板子IP> 8080` 或任意语言的 TCP 客户端都能直接连。

## 内核快照通道 (chatsnap)

### 驱动能力

| 能力 | 实现位置 |
|---|---|
| 模块生命周期 + Kbuild | `module_init/exit`、`obj-m` |
| 主次设备号 + `cdev` + 类/设备 | `alloc_chrdev_region`、`cdev_add`、`class_create/device_create`（udev 自动建节点） |
| 设备操作集 | `file_operations`：open / read / write / poll / release / unlocked_ioctl |
| 环形缓冲 | `kfifo` + `kfifo_to_user/from_user`（不自己写 ring） |
| 阻塞读 | 等待队列 `wait_event_interruptible` + `wake_up_interruptible` |
| 就绪通知 | `.poll` 返回 `EPOLLIN`，可被 epoll/select 纳管 |
| 控制面 | `unlocked_ioctl`（`_IO/_IOR` 编码，与用户态共用 `chatsnap_uapi.h`）+ sysfs `DEVICE_ATTR_RO(stat)` |
| 参数与引用计数 | `module_param(buf_kb)`；`try_module_get/module_put` → 被占用时 `rmmod` 返回 EBUSY |

**并发策略**：读/写/ioctl/sysfs 全部运行在**进程上下文**，因此统一用一把 `mutex`（可睡眠）。这里没有中断上下文，所以**不需要** `spin_lock_irqsave` —— 真把数据源搬进中断处理函数时才需要换。

**缓冲满的策略**：整条丢弃并计数（`drop`），`write()` 仍返回成功。理由：旁路通道**不能因为自己满了就让业务层重试或阻塞**。

### 为什么绕内核一圈（设计权衡）

| 理由 | 说明 |
|---|---|
| 崩溃后可取证 | 用户态管道/日志缓冲随进程消失；kfifo 在内核内存里，服务器被 `kill -9` 后监控端仍能读走最后 N 条 |
| 权限隔离 | 设备节点 `0660`（root:video），聊天内容不是任何本地进程都能抓；比全局可读的日志文件更可控 |
| 多消费者 + 统一就绪语义 | 面板与命令行监控端可同时读；`.poll` 让设备与 socket 进同一个 epoll（同一套 waitqueue 机制） |
| 代价（要承认） | 多一次 `copy_from_user/copy_to_user`、多一层内核依赖；纯做日志时用户态 ring buffer 更快 —— 定位是可观测性，不是性能优化 |

## LVGL 面板

- **显示**：`lv_linux_fbdev` 直写 `/dev/fb0`（720×1280、32bpp、stride 2880，与 `LV_COLOR_DEPTH 32` 对齐，无需位深转换）。
- **触摸**：`lv_evdev` 读 `/dev/input/event1`（电容屏 MT protocol B，由 LVGL 自带驱动处理）。
- **取数**：`lv_timer` 每 **50ms 非阻塞**读一次设备（不另开线程、不阻塞 UI），按 `\n` 切行贴进 flex 列表；上限 200 行，超了删最旧，自动滚到最新。
- **统计**：每秒 `ioctl(CHATSNAP_GET_STAT)` 刷「写入/读出/丢弃/缓存」。
- **交互**：两个触摸按钮 —— 清空屏幕（连带清驱动缓冲）/ 刷新统计。
- 设备被卸载/打不开时自动降级为「未就绪」并持续重试，不崩。

### 中文字库（踩过的坑）

LVGL 内置的 `lv_font_source_han_sans_sc_16_cjk` **不能直接用**：实测只覆盖 **1456 码位**，而本项目协议文案里的 238 个汉字**缺 101 个** —— 连"聊""间""开""离""驱"都没有（它按日文样本集思路生成）。

用 `lv_font_conv` 自建子集（`gui/src/fonts/`）：

- **正文 16px**：3536 个常用字（2.98 MB）。字表来源 = **扫真实语料**（笔记 + 项目源码中出现 ≥2 次的汉字）∪ 源码字符串字面量 ∪ 聊天高频词 —— **不要凭印象手写 `--symbols`**（第一次手写漏了 `·`、`源`、`…`，屏上直接出方框）。
- **标题 24px**：只收界面里真正用到的 8 个字（88 KB）。不同字号各自收字，省约 4 倍体积。
- 校验方式：解析生成 `.c` 的 cmap 反推覆盖集，再拿"运行时真正会显示的那些句子"逐字比对。

## 测试与验收

```bash
sudo bash scripts/chatsnap_accept.sh    # 字符设备功能：12 项
sudo bash scripts/chatsnap_integ.sh     # 端到端集成：5 项
```

| 环境 | 结果 |
|---|---|
| 宿主（Ubuntu / x86_64） | 功能 **12/12** · 端到端 **5/5** |
| **KickPi RK3566 / 内核 6.1.141 / aarch64** | **16/16**（模块 vermagic 实测 `6.1.141 SMP mod_unload aarch64`） |

关键实测读数：

| 验收点 | 读数 |
|---|---|
| 阻塞语义（空缓冲 `cat`） | 超时 `rc=124`，**真阻塞而非忙等**（不烧 CPU） |
| 溢出保护 | `in=65553 out=20 drop=13336`，无 oops/BUG |
| 端到端读回 | 写 `113 B` / 读 `113 B` / 残留 `0`，监控端读到聊天行 |
| 卸载语义 | 有 fd 占用时 `rmmod: Module chatsnap is in use`；释放后卸载干净、节点被清 |
| 面板渲染 | 15 行中文 y 间隔恒为 29px，无重叠、无缺字（列表区墨迹像素 1272 → 27821） |
| 交叉编译产物 | `readelf -d` 仅 `libc.so.6` + `ld-linux-aarch64.so.1` |

## 设计权衡与已知限制

- **全局锁**：房间表/成员表/归属表共用一把 mutex，多房间广播吞吐受限于此；per-room 锁是明确的下一步。
- **快照有损**：缓冲满整条丢弃，只为呈现「最近发生了什么」，不保证完整审计。
- **字库上限**：正文 3536 个常用字覆盖日常输入；生僻字仍会缺（全量需 ~2.3 MB 全字库，或改用 FreeType 动态渲染）。
- **单机部署**：无跨机分发、无持久化；重启即清空（阶段性设计）。
- 面板与服务器当前**各自独立部署**（面板读本机设备节点），未做远程拉取。

## 线程池 API

```c
Threadpool* pool_create(int num);                    // 创建 num 个 worker
int pool_submit(Threadpool* pool, int worker_index,  // 定向投递到指定 worker
                void (*taskfunc)(void*), void* arg);
void pool_destroy(Threadpool* pool);                 // 处理完剩余任务后关闭
```

## 演进方向

- [x] epoll ET + ONESHOT + 固定 worker 线程池（round-robin 绑定）
- [x] 多房间聊天室（昵称/建房/广播/换房/断线回收/粘包处理）
- [x] 自研字符设备内核快照通道（kfifo + 阻塞读 + poll + ioctl + sysfs）
- [x] 板载 LVGL 面板 + 自建中文子集字库（RK3566 实机验证）
- [ ] 二进制协议定长包头（`protocol.h` 已预留），聊天内容 UTF-8 安全分帧
- [ ] 房间锁分片：全局锁 → per-room 锁，提高多房间并发吞吐
- [ ] 把设备当 epoll 事件源（不只做旁路）：硬件事件与 socket 统一复用同一 Reactor
- [ ] 私信 / 房间密码 / 历史消息环形缓冲

## 许可

[MIT](LICENSE)
