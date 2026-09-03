#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "anet.h"

/**
 * 避免Linux内核自动调优时，在高并发的情况下，单连接无限膨胀导致内存灾难（缓冲区大小看项目的场景定）
 * anetSetSendBuffer: 设置 SO_SNDBUF 内核发送缓冲区
 * 返回0成功，-1失败；失败仅打印，不终止程序（Redis行为）
 */
int anetSetSendBuffer(int fd, int size) {
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == -1) {
        perror("anetSetSendBuffer SO_SNDBUF");
        return -1;
    }
    return 0;
}

/**
 * 避免Linux内核自动调优时，在高并发的情况下，单连接无限膨胀导致内存灾难（缓冲区大小看项目的场景定）
 * anetSetRecvBuffer: 设置 SO_RCVBUF 内核接收缓冲区
 */
int anetSetRecvBuffer(int fd, int size) {
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == -1) {
        perror("anetSetRecvBuffer SO_RCVBUF");
        return -1;
    }
    return 0;
}

/**
 * anetSetKeepAlive: TCP保活整套配置
 * idle：空闲多少秒开始探测
 * interval：探测包间隔秒数
 * cnt：最大探测失败次数
 */
int anetSetKeepAlive(int fd, int idle, int interval, int cnt) {
    int keepalive_switch = 1;
    // 打开总开关，跨平台
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive_switch , sizeof(keepalive_switch)) == -1) {
        perror("anetSetKeepAlive SO_KEEPALIVE");
        return -1;
    }

#ifdef __linux__
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
    return 0;
}

/**
 * anetSetTcpNoDelay：TCP_NODELAY，关闭Nagle算法，Redis必开，降低小包延迟
 */
int anetSetTcpNoDelay(int fd, int on) {
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) == -1) {
        perror("anetSetTcpNoDelay TCP_NODELAY");
        return -1;
    }
    return 0;
}

/**
 * anetSetReuseAddr：SO_REUSEADDR，用于listen fd，解决重启TIME_WAIT端口占用
 * 设置地址可重用（避免重启时端口被占用，端口在关闭后会处于TIME_WAIT状态。端口默认要等待1-2分钟才能再次使用）
 * 即使处于TIME_WAIT状态下，可以bing成功
 */
int anetSetReuseAddr(int fd) {
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("anetSetReuseAddr SO_REUSEADDR");
        return -1;
    }
    return 0;
}

/**
 * 封装：对新accept出来的client fd做全套socket配置
 * 模仿Redis：新客户端连接后一次性配置缓冲区、keepalive、nodelay，关闭Nagle算法
 * idle=60, interval=10, cnt=3
 */
int anetConfigClientSocket(int fd) {
    anetSetSendBuffer(fd, ANET_SNDBUF);
    anetSetRecvBuffer(fd, ANET_RCVBUF);
    anetSetKeepAlive(fd, 60, 10, 3);
    anetSetTcpNoDelay(fd, 1);
    return 0;
}

