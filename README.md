# epoll 多房间聊天室（Reactor + 线程池）

基于 Linux `epoll`（边沿触发 + EPOLLONESHOT）+ 固定 worker 线程池的多房间 TCP 聊天室，C 语言实现，约 1000 行，零第三方依赖。

支持：昵称注册（查重）、房间创建/加入/换房/列表、房间内实时广播、加入/离开系统通知、断线自动退房、跨包半行粘包处理。

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
- **全局聊天锁**：房间表/成员表/连接归属表由一把 pthread mutex 保护，广播路径全程持锁串行发送，修掉了早期 `g_worker_of_fd` 主线程与 worker 双写的 data race。
- **按连接行缓冲**：TCP 是字节流，一次 `read` 可能拿到半行或多行；状态机把字节流切成 `\n` 结尾的完整行再解析，超长行（>1KB）整行丢弃防内存滥用。
- **持锁发送设重试上限**（8 次 × 1ms）：慢客户端最多拖 8ms，不会拖死整个房间。

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

## 目录结构

```
.
├── main.c            # 入口：创建线程池、初始化服务器、启动事件循环
├── main.h            # 全局配置（端口、worker 数、缓冲区大小等）
├── Server/           # TCP 初始化 + epoll Reactor 主循环
├── Chat/             # 聊天核心：状态机 + 房间/成员管理 + 广播（与线程模型解耦）
├── Client/           # 交互式客户端（单线程 select 同时监听 stdin 与 socket）
├── pool/             # 线程池：每 worker 独立任务队列，按 index 定向投递
├── build_test/       # 聊天状态机功能测试（临时文件模拟 socket）
└── CMakeLists.txt
```

## 编译与运行

```bash
cmake -S . -B build && cmake --build build

./build/Pro            # 服务器，默认 0.0.0.0:8080
./build/Client         # 交互式聊天客户端（另开 N 个终端 = N 个用户）
./build/Client 50      # 并发模式：50 个连接批量注册测试
```

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
- [ ] 二进制协议定长包头（`protocol.h` 已预留），聊天内容 UTF-8 安全分帧
- [ ] 房间锁分片：全局锁 -> per-room 锁，提高多房间并发吞吐
- [ ] 私信 / 房间密码 / 历史消息环形缓冲
- [ ] 部署嵌入式 Linux（RK3566），与 ESP32 OTA 链路打通
