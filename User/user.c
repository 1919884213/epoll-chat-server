#include "user.h"
#include "main.h"

ssize_t newUser(int id, char* pwd){
  char buff[32] = { 0 };
  int fd = open(PATH, O_WRONLY|O_APPEND);
  int len = sprintf(buff, "userid:%duserpwd:%s\n", id, pwd);
  return write(fd, buff, len);
}
