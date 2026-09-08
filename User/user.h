#include <stdio.h>
#include <stdint.h>
#include "unistd.h"

#define PATH "/home/lrd/桌面/C/Src/User/user.txt"

typedef struct user {
  char name[10];
  int id;
  char pwd[32];
  uint32_t time;
}user_t;

ssize_t newUser(int id, char* pwd);
