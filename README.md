# Linux File Store Server

一个基于 Linux `epoll` + 固定 worker 线程池的 TCP 服务器，目标演进为**嵌入式 Linux 文件服务器 / OTA 固件中心**（RK3566、ESP32、STM32 链路）。

当前完成 **阶段 0 + 1**：打通 `epoll` 事件多路复用与线程池，并用 `PING/ECHO` 验证全链路。

## 架构

```
                    主线程 (epoll_wait)
                          │  只做事件多路复用，不阻塞
          ┌───────────────┴────────────────┐
          │ listenfd 就绪                   │ clientfd 就绪
          ▼                                 ▼
   accept 新连接                     查连接表找到绑定的 worker
   round-robin 绑定 worker                  │
   记入 fd->worker 连接表              pool_submit(worker, 处理任务)
          │                                 │
          └───────────────┬─────────────────┘
                          ▼
                   worker 线程
        非阻塞读循环到 EAGAIN → 处理 → 重新武装 EPOLLONESHOT
```

关键设计：

- **单 epoll 主线程**：一个线程管理全部连接，只负责事件通知，不阻塞在 I/O 上。
- **每连接绑定固定 worker**：accept 时按 round-robin 分配 worker，该连接后续读写都在同一线程，避免同一 fd 被并发读写。
- **`EPOLLONESHOT`**：fd 触发一次后自动失活，worker 处理完用 `EPOLL_CTL_MOD` 重新武装，保证同一 fd 同一时刻只有一个任务在飞。
- **`EPOLLET` 边沿触发**：必须循环 `read` 直到 `EAGAIN`，一次性读干净内核缓冲区。
- **连接表**：`fd -> worker` 映射，断开时 `EPOLL_CTL_DEL` + `close` + 回收表项。

## 目录结构

```
.
├── main.c            # 程序入口：创建线程池、初始化服务器、启动事件循环
├── main.h            # 全局配置（端口、worker 数、缓冲区大小等）
├── protocol.h        # 自定义二进制协议占位（命令枚举 + 定长包头）
├── Server/
│   ├── Server.c      # TCP 初始化 + epoll 事件循环 + 连接处理
│   └── Server.h
├── Client/
│   ├── Client.c      # 并发测试客户端（PING -> PONG）
│   └── Client.h
├── pool/
│   ├── pool.c        # 线程池：每 worker 独立任务队列
│   └── pool.h
└── CMakeLists.txt
```

## 编译

```bash
cmake -S . -B build
cmake --build build
```

产物：`build/Pro`（服务器）、`build/Client`（测试客户端）。

## 运行

启动服务器：

```bash
./build/Pro
```

另开终端，用测试客户端发起并发连接（参数为连接数，默认 3）：

```bash
./build/Client 6
```

预期服务器日志：

```
[INFO] server listening on port 8080
[INFO] client 127.0.0.1:38890 connected -> worker 0
[INFO] client 127.0.0.1:38892 connected -> worker 1
[INFO] worker 0 fd 5: PING -> PONG
[INFO] client fd 5 disconnected
```

客户端日志：

```
[client 0] PING -> PONG
```

## 线程池 API

```c
Threadpool* pool_create(int num);                 // 创建 num 个 worker
int pool_submit(Threadpool* pool, int worker_index,
                void (*taskfunc)(void*), void* arg); // 投递任务到指定 worker
void pool_destroy(Threadpool* pool);              // 处理完剩余任务后关闭
```

## 路线图

- [x] 阶段 0：修复基础（线程池模块化、epoll 边沿触发、进程分离）
- [x] 阶段 1：epoll 对接固定 worker 线程池（PING/ECHO 验证）
- [ ] 阶段 2：文件服务器（`LIST / UPLOAD / DOWNLOAD / DELETE`，二进制协议）
- [ ] 阶段 3：断点续传（偏移量）
- [ ] 阶段 4：部署到 RK3566，ESP32 下载固件 → STM32 OTA
