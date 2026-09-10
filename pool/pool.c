#include "pool.h"

static void* handler(void* arg) {
  Threadpool* pool = (Threadpool*)arg;
  while (1) {
    pthread_mutex_lock(&pool->mutex);
    while (pool->queueFront == pool->queueRear && pool->flag == 1)
      pthread_cond_wait(&pool->cond, &pool->mutex);
    if (pool->queueFront == pool->queueRear && pool->flag == 0) {
      pthread_mutex_unlock(&pool->mutex);
      break;
    }
    // 从队列中获取一个任务
    Task* t = pool->queueFront->next;
    pool->queueFront->next = t->next;
    if (pool->queueRear == t)  // 取出的是最后一个任务，队列已空
      pool->queueRear = pool->queueFront;
    pthread_mutex_unlock(&pool->mutex);
    // 执行任务
    t->func(t->arg);
    free(t);
  }
  printf("线程池已关闭\n");
  return NULL;
}

void taskFunc(void* arg) {
  int num = *(int*)arg;
  printf("线程 %ld 工作中,num = %d\n", pthread_self(), num);
  sleep(1);
  free(arg);
}

static void insertTask(Threadpool* pool, void (*taskfunc)(void*), void* arg) {
  Task* t = (Task*)malloc(sizeof(Task));
  if (t == NULL)
    return;
  t->func = taskfunc;
  t->arg = arg;
  t->next = NULL;
  pthread_mutex_lock(&pool->mutex);
  pool->queueRear->next = t;
  pool->queueRear = t;
  pthread_cond_signal(&pool->cond);
  pthread_mutex_unlock(&pool->mutex);
}


Threadpool* newPool(int num, void* (*func)(void*)) {
  Threadpool* pool = (Threadpool*)malloc(sizeof(Threadpool));
  if (pool == NULL)
    return NULL;
  // 初始化任务队列
  pool->queueFront = (Task*)malloc(sizeof(Task));
  if (pool->queueFront == NULL) {
    free(pool);
    return NULL;
  }
  pool->queueFront->next = NULL;
  pool->queueRear = pool->queueFront;
  // 初始化线程数量
  pool->num = num;
  // 初始化线程号
  pool->threadID = (pthread_t*)malloc(sizeof(pthread_t) * num);
  if (pool->threadID == NULL) {
    free(pool->queueFront);
    free(pool);
    return NULL;
  }

  pthread_mutex_init(&pool->mutex, NULL);
  pthread_cond_init(&pool->cond, NULL);

  pool->flag = 1;

  for (int i = 0; i < num; i++) {
    pthread_create(&pool->threadID[i], NULL, func, pool);  // 创建线程
    pthread_detach(pool->threadID[i]);  // 线程结束后自动回收资源
  }

  return pool;
}

int main() {
  Threadpool* pool = newPool(5, handler);
  if (pool == NULL)
    return 1;
  printf("线程创建成功\n");
  for (int i = 0; i < 50; i++) {
    int* n = (int*)malloc(sizeof(int));
    *n = i;
    insertTask(pool, taskFunc, n);
  }

  sleep(11);

  pthread_mutex_lock(&pool->mutex);
  pool->flag = 0;
  pthread_cond_broadcast(&pool->cond);
  pthread_mutex_unlock(&pool->mutex);

  sleep(1);

  free(pool->threadID);
  free(pool->queueFront);
  pthread_mutex_destroy(&pool->mutex);
  pthread_cond_destroy(&pool->cond);
  free(pool);
  return 0;
}
