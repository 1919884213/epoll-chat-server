#ifndef POOL_H
#define POOL_H

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>


typedef struct Task {
  void (*func)(void* arg);
  void* arg;
  struct Task* next;
}Task;

typedef struct ThreadPool {
  //任务队列
  Task *queueFront;
  Task *queueRear;
  //线程数量
  int num;
  //线程号
  pthread_t *threadID;
  //互斥锁与条件变量
  pthread_mutex_t mutex;
  pthread_cond_t  cond;
  //状态
  int flag;
} Threadpool;

Threadpool* newPool(int num, void* (*func)(void*));

#endif