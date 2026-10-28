#ifndef POOL_H
#define POOL_H

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* 任务节点：保存待执行的函数及其参数 */
typedef struct Task {
  void (*func)(void* arg);
  void* arg;
  struct Task* next;
} Task;

/* 单个工作线程：拥有独立的任务队列，实现"连接固定绑定某个worker" */
typedef struct Worker {
  pthread_t tid;
  Task* head;  // 队列哨兵节点，head->next 为首个任务
  Task* tail;  // 指向队尾任务
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  int stop;  // 置1表示该线程处理完剩余任务后退出
} Worker;

/* 线程池：由 num 个相互独立的 Worker 组成 */
typedef struct Threadpool {
  Worker* workers;
  int num;
} Threadpool;

/* 创建线程池，num 为工作线程数量，失败返回 NULL */
Threadpool* pool_create(int num);

/* 向指定 worker 的队列投递任务，成功返回0，失败返回-1 */
int pool_submit(Threadpool* pool, int worker_index, void (*taskfunc)(void*),
                void* arg);

/* 关闭线程池：处理完剩余任务后回收所有线程与内存 */
void pool_destroy(Threadpool* pool);

#endif
