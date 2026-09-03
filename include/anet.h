#ifndef __ANET_H
#define __ANET_H

#include <sys/socket.h>

// 设置为 4MB，作为内核自动调优的“天花板”（上限）
#define ANET_SNDBUF  (1024 * 1024 * 4)
#define ANET_RCVBUF  (1024 * 1024 * 4)

int anetSetSendBuffer(int fd, int size);
int anetSetRecvBuffer(int fd, int size);
int anetSetKeepAlive(int fd, int idle, int interval, int cnt);
int anetSetTcpNoDelay(int fd, int on);
int anetSetReuseAddr(int fd);

int anetConfigClientSocket(int fd);

#endif
