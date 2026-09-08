#include "Client.h"
#include "main.h"

int client_init(void){
  
  int clientfd = socket(AF_INET,SOCK_STREAM,0);
  struct sockaddr_in serverAddr = {
    .sin_family = AF_INET,
    .sin_port = htons(PORT),
  };
  inet_pton(AF_INET,SERVERADDR,&serverAddr.sin_addr);
  connect(clientfd,(struct sockaddr*)&serverAddr,sizeof(serverAddr));

  close(clientfd);
}