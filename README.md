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

## 客户端与任务是如何绑定的

核心数据是连接表 `g_worker_of_fd[]`：**下标 = fd，值 = 该 fd 所属的 worker 下标**（`-1` 表示空闲）。整个绑定贯穿一条完整链路，分 4 步：

### 1. 分配归属（主线程 accept 时登记）
```c
int wi = next_worker;                          // 选一个 worker 下标
next_worker = (next_worker + 1) % pool->num;   // round-robin，负载均衡
g_worker_of_fd[cfd] = wi;                      // 记录 fd -> worker 归属
```
建立**第一层绑定：fd → worker 下标**。每个新连接按轮流算法分给一个 worker，所有 `fd` 初始为 `-1`。

### 2. 任务打包（主线程，客户端可读事件到来时）
```c
int wi = g_worker_of_fd[fd];    // 反查该 fd 属于哪个 worker
Conn* c = malloc(sizeof(Conn)); // 打包任务上下文
c->fd = fd;                     // 要处理哪个 fd
c->worker_index = wi;           // 该交给哪个 worker
pool_submit(pool, wi, handle_conn, c);  // 投进 worker wi 的任务队列
```
建立**第二层绑定：任务对象（Conn）→ worker 下标**。`Conn` 把"要处理的 fd"和"负责它的 worker"打包在一起，作为任务参数。

### 3. 真正执行（worker 线程从自己的队列取任务）
```c
// pool_submit 内部 / worker_loop：
while (队列非空) {
  Task* t = 出队(worker wi 的队列);
  t->fn(t->arg);   // 即 handle_conn(c)，c->fd 就是要处理的连接
}
```
每个 worker **只处理自己队列里的任务**，而这些任务都是主线程按 `worker_index` 精确投递的。因此 worker `wi` 只会处理"被登记为 `wi` 的那些 fd"。

### 4. 重新武装（worker 处理完，回到 epoll）
```c
ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
epoll_ctl(c->epfd, EPOLL_CTL_MOD, c->fd, &ev);  // 重新触发，等下一次可读
```

### 完整数据流（以 fd=100 为例）
```
1. accept(100)       ->  g_worker_of_fd[100] = 2        （fd→worker 映射）
2. epoll 报 100 可读 ->  wi = g_worker_of_fd[100] = 2
3. 打包              ->  Conn{fd=100, worker_index=2}
4. 投递              ->  pool_submit(pool, 2, handle_conn, Conn) 进 worker2 队列
5. 执行              ->  worker2 取出任务 -> handle_conn(c) -> read(100) -> PONG
```

### 为什么这样设计
- **同一 fd 永远只被同一个 worker 处理**（`worker_index` 贯穿全程），对 fd 的读写天然串行，不会出现两个线程同时操作一个 fd。
- epoll 只负责"通知主线程哪个 fd 有数据"，主线程只做"派活给对应 worker"，**事件分发与业务处理完全解耦**。

> ⚠️ 已知隐患：`g_worker_of_fd[]` 由主线程写入（accept / close 回滚），但 `close_conn` 在 worker 线程也会把表项改回 `-1`，存在数据竞争，后续需加锁或改用原子操作。

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
