/* ============================================================
 * main_stage1.c —— 改造阶段 1：ae 事件循环骨架的空跑验证
 * ============================================================
 *
 * ★ 本阶段【只做文件事件】的骨架验证，不接命令解析：
 *
 *   · 用 ae 接管 accept：listen fd 注册 AE_READABLE
 *   · accept 后把新 fd 也注册进事件循环（★ 设为非阻塞）
 *   · 收到数据就 read 一次并丢弃，读到 0 / 出错就关闭连接
 *
 * 目的：把「ae 层是否正确」和「解析器是否正确」隔离开。
 * 如果这里有问题，问题一定在 ae 层；反之说明 ae 层可用。
 *
 * ★ 与阶段 0（src/main.c）的对比验证：
 *   阶段 0：第二个客户端连不上（服务器卡在第一个客户端的 read 上）
 *   本文件：多个客户端可以同时连上、同时被服务
 *
 * 编译：make stage1 && ./mini-redis-stage1
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "ae.h"
#include "server.h"     /* create_server_socket / Keep_running / anet 包装 */

#define DEFAULT_PORT 6379
#define BACKLOG 10
/* MAX_CLIENTS 现在统一定义在 include/server.h（server.h 已被包含），
 * 这里不再重复定义 —— 否则 -Wall 会报 "redefined"。 */

/* 阶段 1 的「客户端」只需要这些 —— 真正的 Client 结构体留到阶段 2 */
typedef struct Stage1Conn {
    int fd;
    struct Stage1Conn *next;
    struct Stage1Conn *prev;
} Stage1Conn;

static aeEventLoop *g_loop = NULL;
static Stage1Conn *g_conns = NULL;      /* 双向链表头 */
static int g_numconns = 0;

volatile sig_atomic_t Keep_running = 1;

/* ---------- 连接表（双向链表，free 时 O(1) 摘除） ---------- */
static void conn_add(Stage1Conn *c) {
    c->prev = NULL;
    c->next = g_conns;
    if (g_conns) g_conns->prev = c;
    g_conns = c;
    g_numconns++;
}

static void conn_remove(Stage1Conn *c) {
    if (c->prev) c->prev->next = c->next;
    else         g_conns = c->next;
    if (c->next) c->next->prev = c->prev;
    g_numconns--;
}

/* ---------- 设为非阻塞 ---------- */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * 关闭连接 —— ★ 严格按「撤销 → 关闭 → 释放 → 清账」四步。
 * 顺序不能换：DEL 必须在 close 之前（close 后 fd 无效会 EBADF；
 * 且 fd 会被内核立即复用，此时 DEL 可能误删新连接）。
 */
static void free_conn(Stage1Conn *c) {
    if (!c) return;

    /* ① 撤关注：先读后写（两个都要删，漏了写事件会让 epoll 持续报告） */
    aeDeleteFileEvent(g_loop, c->fd, AE_READABLE);
    aeDeleteFileEvent(g_loop, c->fd, AE_WRITABLE);
    /* 注：aeDeleteFileEvent 在 mask 归零时已清 clientData/rfileProc，
     *     这里再把 clientData 显式置 NULL 也无害（幂等）。 */

    /* ② 关 fd —— 释放编号，防止泄漏到 ulimit 上限后接不了新连接 */
    close(c->fd);

    /* ③ 从容器摘除（O(1)，双向链表） */
    conn_remove(c);

    /* ④ 释放对象 */
    free(c);
}

/* ---------- 客户端可读回调 ---------- */
static void stage1ReadHandler(aeEventLoop *el, int fd, void *clientData, int mask) {
    (void)el; (void)mask;
    Stage1Conn *c = (Stage1Conn *)clientData;
    char buf[4096];

    ssize_t n = read(fd, buf, sizeof(buf));

    if (n > 0) {
        /* 阶段 1 不解析命令，只确认「数据到了、没有卡住别的事件」 */
        printf("[fd=%d] 收到 %zd 字节（阶段 1 丢弃）\n", fd, n);
        return;
    }
    if (n == 0) {
        printf("[fd=%d] 客户端正常断开\n", fd);
        free_conn(c);
        return;
    }
    /* n < 0 */
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* ★ 非阻塞下「现在没数据」是正常情况，不是错误，直接返回等下次通知 */
        return;
    }
    if (errno == EINTR) return;    /* 被信号打断，下次再说 */

    perror("read");
    free_conn(c);
}

/* ---------- listen fd 可读回调：★ 循环 accept 到 EAGAIN ---------- */
static void stage1AcceptHandler(aeEventLoop *el, int fd, void *clientData, int mask) {
    (void)clientData; (void)mask;

    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);

        int cfd = accept(fd, (struct sockaddr *)&caddr, &clen);

        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;   /* ★ 队列空了，正常出口 */
            if (errno == EINTR) continue;                          /* 信号打断，重试 */
            /* EMFILE/ENFILE 等：★ 必须 break，continue 会死循环 */
            perror("accept");
            break;
        }

        /* ★ 新 fd 【不会】继承 listen fd 的非阻塞属性 —— 必须显式设置。
         *   漏了这一步：客户端连上但不发数据时，read 会阻塞住整个事件循环。 */
        if (set_nonblocking(cfd) == -1) {
            perror("set_nonblocking");
            close(cfd);
            continue;
        }

        /* maxclients 检查（在 EMFILE 之前就拦住） */
        if (g_numconns >= MAX_CLIENTS) {
            fprintf(stderr, "连接数已达上限 %d，拒绝 fd=%d\n", MAX_CLIENTS, cfd);
            close(cfd);
            continue;
        }

        anetConfigClientSocket(cfd);   /* 缓冲区/keepalive/nodelay */

        Stage1Conn *c = malloc(sizeof(*c));
        if (!c) { close(cfd); continue; }
        c->fd = cfd;
        conn_add(c);

        /* 注册可读，clientData 挂我们自己 —— ae 层只当它是 void* */
        if (aeCreateFileEvent(el, cfd, AE_READABLE, stage1ReadHandler, c) == AE_ERR) {
            fprintf(stderr, "注册 fd=%d 失败\n", cfd);
            close(cfd);
            conn_remove(c);
            free(c);
            continue;
        }

        printf("[fd=%d] 新连接来自 %s（当前 %d 个）\n",
               cfd, inet_ntoa(caddr.sin_addr), g_numconns);
    }
}

/* ---------- beforeSleep：阶段 1 只做计数（阶段 3 在这里 flush outbuf） ---------- */
static void stage1BeforeSleep(aeEventLoop *el) {
    (void)el;
    /* 阶段 3 会变成：遍历 g_conns，把 outbuf 里的数据 write 出去 */
}

/* ---------- 优雅退出：让 aeMain 的循环结束 ---------- */
static void handle_sigint(int sig) {
    (void)sig;
    Keep_running = 0;
    if (g_loop) aeStop(g_loop);    /* ★ 让 aeMain 退出，不用等客户端断开 */
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    const char *env_port = getenv("PORT");
    if (env_port && *env_port) {
        int ep = atoi(env_port);
        if (ep > 0 && ep <= 65535) port = ep;
    }
    if (argc >= 2) {
        int ap = atoi(argv[1]);
        if (ap > 0 && ap <= 65535) port = ap;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    /* ★ SIGPIPE 必须忽略：向已收到 RST 的连接 write 会先收到 SIGPIPE，
     *   而它的默认动作是【终止进程】—— 一个客户端就能杀掉服务器。 */
    struct sigaction sa_pipe;
    memset(&sa_pipe, 0, sizeof(sa_pipe));
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sigaction(SIGPIPE, &sa_pipe, NULL);

    int server_fd = create_server_socket(port, BACKLOG);

    /* listen fd 设为非阻塞，否则 accept 可能卡住事件循环 */
    if (set_nonblocking(server_fd) == -1) {
        perror("set_nonblocking(server_fd)");
        close(server_fd);
        return EXIT_FAILURE;
    }

    g_loop = aeCreateEventLoop(MAX_CLIENTS);
    if (!g_loop) {
        fprintf(stderr, "aeCreateEventLoop 失败\n");
        close(server_fd);
        return EXIT_FAILURE;
    }

    aeSetBeforeSleepProc(g_loop, stage1BeforeSleep);

    if (aeCreateFileEvent(g_loop, server_fd, AE_READABLE,
                          stage1AcceptHandler, NULL) == AE_ERR) {
        fprintf(stderr, "注册 listen fd 失败\n");
        aeDeleteEventLoop(g_loop);
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("mini-redis(阶段1) 启动，监听 %d，事件循环后端 = %s\n",
           port, aeGetApiName());
    printf("★ 阶段 1 只验证事件循环：收到数据会丢弃，不会执行命令\n");

    aeMain(g_loop);

    /* ---- 清理 ---- */
    printf("\n正在关闭服务器...\n");
    Stage1Conn *c = g_conns;
    while (c) {
        Stage1Conn *next = c->next;
        close(c->fd);
        free(c);
        c = next;
    }
    aeDeleteEventLoop(g_loop);
    close(server_fd);
    printf("所有资源已释放，再见！\n");
    return 0;
}
