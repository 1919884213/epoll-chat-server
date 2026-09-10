#include "pool.h"

/* 工作线程入口：循环从本线程的队列取任务并执行 */
static void* worker_loop(void* arg) {
  Worker* w = (Worker*)arg;
  while (1) {
    pthread_mutex_lock(&w->mutex);
    // 队列为空且未要求停止时阻塞等待；用 while 防止虚假唤醒
    while (w->head->next == NULL && !w->stop)
      pthread_cond_wait(&w->cond, &w->mutex);
    // 队列已空且收到停止信号：退出线程
    if (w->head->next == NULL && w->stop) {
      pthread_mutex_unlock(&w->mutex);
      break;
    }
    // 取出队首任务
    Task* t = w->head->next;
    w->head->next = t->next;
    if (w->tail == t)  // 取出的是最后一个任务，队列变空
      w->tail = w->head;
    pthread_mutex_unlock(&w->mutex);
    // 在锁外执行任务，避免长时间持锁
    t->func(t->arg);
    free(t);
  }
  return NULL;
}
//创建线程池
Threadpool* pool_create(int num) {
  if (num <= 0)
    return NULL;
  Threadpool* pool = (Threadpool*)malloc(sizeof(Threadpool));
  if (pool == NULL)
    return NULL;
  pool->num = num;
  pool->workers = (Worker*)malloc(sizeof(Worker) * num); //申请每个线程的空间
  if (pool->workers == NULL) {
    free(pool);
    return NULL;
  }
  //构建num个线程
  for (int i = 0; i < num; i++) {
    Worker* w = &pool->workers[i];
    // 哨兵节点，队首队尾均指向它表示队列为空
    w->head = (Task*)malloc(sizeof(Task));
    if (w->head == NULL) {
      // 回收已初始化完成的 worker
      for (int j = 0; j < i; j++) {
        pthread_mutex_destroy(&pool->workers[j].mutex);
        pthread_cond_destroy(&pool->workers[j].cond);
        free(pool->workers[j].head);
      }
      free(pool->workers);
      free(pool);
      return NULL;
    }
    w->head->next = NULL;
    w->tail = w->head;
    w->stop = 0;
    // 先初始化同步对象再创建线程，避免线程使用未初始化的锁/条件变量
    pthread_mutex_init(&w->mutex, NULL);
    pthread_cond_init(&w->cond, NULL);
    if (pthread_create(&w->tid, NULL, worker_loop, w) != 0) {
      pthread_mutex_destroy(&w->mutex);
      pthread_cond_destroy(&w->cond);
      free(w->head);
      pool->num = i;  // 仅销毁已成功创建的部分
      pool_destroy(pool);
      return NULL;
    }
  }
  return pool;
}

int pool_submit(Threadpool* pool, int worker_index, void (*taskfunc)(void*),
                void* arg) {
  if (pool == NULL || worker_index < 0 || worker_index >= pool->num)
    return -1;
  Task* t = (Task*)malloc(sizeof(Task));
  if (t == NULL)
    return -1;
  t->func = taskfunc;
  t->arg = arg;
  t->next = NULL;

  Worker* w = &pool->workers[worker_index];
  pthread_mutex_lock(&w->mutex);
  w->tail->next = t;  // 接到队尾
  w->tail = t;
  pthread_cond_signal(&w->cond);  // 唤醒本 worker 等待任务
  pthread_mutex_unlock(&w->mutex);
  return 0;
}

void pool_destroy(Threadpool* pool) {
  if (pool == NULL)
    return;
  // 通知所有 worker 停止
  for (int i = 0; i < pool->num; i++) {
    Worker* w = &pool->workers[i];
    pthread_mutex_lock(&w->mutex);
    w->stop = 1;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->mutex);
  }
  // 等待线程退出
  for (int i = 0; i < pool->num; i++)
    pthread_join(pool->workers[i].tid, NULL);
  // 释放资源
  for (int i = 0; i < pool->num; i++) {
    Worker* w = &pool->workers[i];
    pthread_mutex_destroy(&w->mutex);
    pthread_cond_destroy(&w->cond);
    free(w->head);
  }
  free(pool->workers);
  free(pool);
}
