#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/*
 * 自定义二进制协议（占位）。
 * 阶段0+1 仅用纯文本 PING/ECHO 验证链路；
 * 文件服务器阶段将启用下列命令与定长包头。
 */

/* 命令类型 */
typedef enum {
  CMD_PING = 0,
  CMD_LIST,
  CMD_UPLOAD,
  CMD_DOWNLOAD,
  CMD_DELETE
} CmdType;

/* 定长包头：命令 + 负载长度（网络字节序） */
typedef struct {
  uint32_t cmd;
  uint32_t length;
} PacketHeader;

#endif
