#include "pool.h"

// 工作线程的执行函数
// 由 newPool 创建线程时作为线程入口传入，arg 指向线程池对象
static void* handler(void* arg) {
  Threadpool* pool = (Threadpool*)arg;
  while (1) {
    // 加锁保护任务队列与 flag
    pthread_mutex_lock(&pool->mutex);
    // 当队列为空且线程池仍在运行(flag==1)时，阻塞等待被插入任务唤醒
    // 用 while 而非 if，避免条件变量的虚假唤醒(spurious wakeup)
    while (pool->queueFront == pool->queueRear && pool->flag == 1)
      pthread_cond_wait(&pool->cond, &pool->mutex);
    // 队列为空且线程池已关闭(flag==0)：释放锁并退出线程
    if (pool->queueFront == pool->queueRear && pool->flag == 0) {
      pthread_mutex_unlock(&pool->mutex);
      break;
    }
    // 从队首取下一个任务（队列采用带哨兵节点的单链表，queueFront 为哨兵）
    Task* t = pool->queueFront->next;
    pool->queueFront->next = t->next;
    // 若取出的正是最后一个任务，则队尾指针回退到哨兵，保持队列为空判断成立
    if (pool->queueRear == t)  // 取出的是最后一个任务，队列已空
      pool->queueRear = pool->queueFront;
    pthread_mutex_unlock(&pool->mutex);  // 出队完成，可释放锁再执行任务
    // 在锁外执行任务，避免长时间持锁阻塞其他线程
    t->func(t->arg);
    free(t);  // 释放任务节点
  }
  printf("线程池已关闭\n");
  return NULL;
}

// 示例任务函数：打印当前线程号与任务编号，模拟耗时 1 秒的工作
// 注意：此函数会释放 arg，因此传入的 arg 必须是堆上分配且归该任务所有
void taskFunc(void* arg) {
  int num = *(int*)arg;
  printf("线程 %ld 工作中,num = %d\n", pthread_self(), num);
  sleep(1);
  free(arg);
}

// 向线程池的任务队列尾部插入一个新任务并唤醒一个等待中的工作线程
// taskfunc 为要执行的函数，arg 为该函数的参数
static void insertTask(Threadpool* pool, void (*taskfunc)(void*), void* arg) {
  Task* t = (Task*)malloc(sizeof(Task));
  if (t == NULL)
    return;  // 分配失败则放弃本次插入
  t->func = taskfunc;
  t->arg = arg;
  t->next = NULL;
  pthread_mutex_lock(&pool->mutex);  // 加锁保护队列
  pool->queueRear->next = t;  // 接到队尾
  pool->queueRear = t;        // 更新队尾指针
  pthread_cond_signal(&pool->cond);  // 唤醒一个正在等待任务的工作线程
  pthread_mutex_unlock(&pool->mutex);
}


// 创建并初始化一个线程池
// num  为工作线程数量
// func 为工作线程的入口函数(一般传 handler)
// 返回线程池指针，失败时返回 NULL
Threadpool* newPool(int num, void* (*func)(void*)) {
  Threadpool* pool = (Threadpool*)malloc(sizeof(Threadpool));
  if (pool == NULL)
    return NULL;
  // 初始化任务队列：创建哨兵头节点，队首队尾都指向它，表示队列为空
  pool->queueFront = (Task*)malloc(sizeof(Task));
  if (pool->queueFront == NULL) {
    free(pool);
    return NULL;
  }
  pool->queueFront->next = NULL;
  pool->queueRear = pool->queueFront;
  // 初始化线程数量
  pool->num = num;
  // 为线程号数组分配空间
  pool->threadID = (pthread_t*)malloc(sizeof(pthread_t) * num);
  if (pool->threadID == NULL) {
    free(pool->queueFront);
    free(pool);
    return NULL;
  }

  // 必须先初始化互斥锁与条件变量，再创建线程，避免工作线程使用未初始化的同步对象
  pthread_mutex_init(&pool->mutex, NULL);
  pthread_cond_init(&pool->cond, NULL);

  pool->flag = 1;  // 标记线程池处于运行状态

  // 创建 num 个工作线程并设置为分离状态
  for (int i = 0; i < num; i++) {
    pthread_create(&pool->threadID[i], NULL, func, pool);  // 创建线程
    pthread_detach(pool->threadID[i]);  // 线程结束后自动回收资源
  }

  return pool;
}

int main() {
  // 创建包含 5 个工作线程的线程池
  Threadpool* pool = newPool(5, handler);
  if (pool == NULL)
    return 1;
  printf("线程创建成功\n");
  // 向线程池投递 50 个任务
  for (int i = 0; i < 50; i++) {
    int* n = (int*)malloc(sizeof(int));
    *n = i;
    insertTask(pool, taskFunc, n);
  }

  sleep(11);  // 等待任务全部执行完毕(50 个任务 / 5 线程 * 1 秒 ≈ 10 秒)

  // 关闭线程池：置 flag=0 并唤醒所有线程，让它们处理完剩余任务后退出
  pthread_mutex_lock(&pool->mutex);
  pool->flag = 0;
  pthread_cond_broadcast(&pool->cond);
  pthread_mutex_unlock(&pool->mutex);

  sleep(1);  // 等待工作线程退出

  // 释放资源
  free(pool->threadID);
  free(pool->queueFront);
  pthread_mutex_destroy(&pool->mutex);
  pthread_cond_destroy(&pool->cond);
  free(pool);
  return 0;
}