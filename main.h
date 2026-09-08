#include "pthread.h"
#include "sys/epoll.h"
#include <stdio.h>
#include "Server.h"
#include "Client.h"
#include "fcntl.h"
#include "arpa/inet.h"
#include "sys/epoll.h"
#include <stdlib.h>
#include <string.h>
#include "user.h"


#define PORT 8080
#define MAX_NUM 1024
#define SERVERADDR "127.0.0.1"