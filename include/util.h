#ifndef UTIL_H
#define UTIL_H

/* ============================================================
 * util.h —— 与网络无关的小工具
 * ============================================================
 * ★ 存在的理由（拆分解耦时新增）：同一个常量、同一个 6 行小函数
 *   原本在 main.c / networking.c 各写了一份（当时还有一个已移除的
 *   stage1 产物也各写一份，三份）。
 *
 *   「三份定义」的危险不在于啰嗦，而在于它们【会各自漂移】：
 *   有一天把 BACKLOG 从 10 改成 128，只改了 networking.c 那份，
 *   另外一个产物就悄悄用着 10 —— 而且不报任何错。
 */

/* 服务器 socket 的常量 ---------------------------------------------------
 * BACKLOG：listen() 的等待连接队列长度。★ 故意设得比常见值(511)小：
 *   队列过长只会让「服务器已经忙不过来」的事实对客户端隐藏更久
 *   （连接被排进队列而不是收到 RST），反而更难发现问题。
 *   它不是并发能力的上限 —— 真正上限是 MAX_CLIENTS + 事件循环。 */
#define BACKLOG 10

/* set_nonblocking —— 把 fd 设为非阻塞。
 * ★ 放在头文件 static inline：它是 6 行、无状态、零成本的系统调用封装，
 *   没必要为它建一个 .c（那会带来跨编译单元的调用开销和 Makefile 条目）。
 *
 *   原本 main.c / networking.c 各写一份 —— 两份定义一旦漂移
 *   （比如某处漏了 F_GETFL 的失败检查）就不会被发现。 */
#include <fcntl.h>

static inline int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

#endif
