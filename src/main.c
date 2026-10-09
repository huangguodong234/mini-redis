#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ae.h"
#include "server.h"
#include "util.h"       /* BACKLOG / set_nonblocking（全项目共用一份） */
#include "storage.h"
#include "zset.h"

#define DEFAULT_PORT 6379 // 默认监听端口（可用命令行参数或 PORT 环境变量覆盖）

// 全局标志：是否继续运行（用 sig_atomic_t 保证在信号处理函数中安全读写）
volatile sig_atomic_t Keep_running = 1;

/* ★ 全局服务状态 —— 由 networking.c 定义（它是主要使用者）。
 *   这里只是 extern 引用，填好字段后事件回调就能通过它拿到 store/zset/el。
 *   （放在 networking.c 而不是这里，是为了让不链接 main.c 的独立产物也能用。） */

/* 事件循环指针的副本，专供信号处理函数调 aeStop（异步信号安全：只是置一个 int） */
static aeEventLoop *g_el = NULL;

// SIGINT 处理函数：当用户按下 Ctrl+C 时调用
void handle_sighing(int sig){
    (void) sig;
    Keep_running = 0;
    /* ★ 关键：让 aeMain 的循环退出。
     *   只置 Keep_running 是不够的 —— aeMain 睡在 epoll_wait 里，
     *   没有任何东西会去看那个标志。aeStop 会置 loop->stop，
     *   aeMain 下一轮循环条件判断时就会退出。
     *   （对比改造前：要靠 read() 被 EINTR 打断才能退，而且有客户端
     *     连着时会永远退不出去。） */
    if (g_el) aeStop(g_el);
}

// 注册信号处理
void setup_signal_handler(void){
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sighing;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    // P0-1 修复：忽略 SIGPIPE。
    // 客户端发完命令不读回复就关闭（内核回 RST）、或用 SO_LINGER(1,0) 强制 RST 关闭时，
    // 本进程后续的 write() 会先收到 SIGPIPE，而它的【默认动作是终止进程】——
    // 一个客户端就能把整个服务器杀掉。忽略之后 write() 只返回 -1/EPIPE，
    // 由上层当作普通写错误处理。
    struct sigaction sa_pipe;
    memset(&sa_pipe, 0, sizeof(sa_pipe));
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sa_pipe.sa_flags = 0;
    sigaction(SIGPIPE, &sa_pipe, NULL);
}

/* 设为非阻塞（listen fd 用；客户端 fd 由 networking.c 在 accept 后设置）——
 * ★ 实现已搬到 include/util.h 的 static inline（原来 main.c / networking.c /
 *   main_stage1.c 各写了一份，三份会各自漂移）。 */

int main(int argc, char **argv){
    srand((unsigned)time(NULL));  // 用当前时间播种随机数，使跳表结构不再每次启动相同

    // 解析监听端口：命令行参数 > 环境变量 PORT > 默认 6379
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

    // 初始化存储引擎
    Storage *store = storage_init();
    ZSet *global_zset = zset_create();
    if (!store || !global_zset) {
        fprintf(stderr, "初始化失败：%s\n",
                store ? "有序集合(zset) 创建失败" : "存储引擎(storage) 创建失败");
        zset_free(global_zset);    // 两者对 NULL 都是安全 no-op
        storage_free(store);
        return EXIT_FAILURE;
    }

    // 注册信号处理
    setup_signal_handler();

    /*创建服务器 socket（已绑定并监听）*/
    int server_fd = create_server_socket(port, BACKLOG);

    /* ★ listen fd 也要非阻塞 —— 否则 accept 可能卡住事件循环。
     *   （accept 返回 EAGAIN 表示当前没有新连接，正常退出循环即可） */
    if (set_nonblocking(server_fd) == -1) {
        perror("set_nonblocking(server_fd)");
        close(server_fd);
        zset_free(global_zset);
        storage_free(store);
        return EXIT_FAILURE;
    }

    /* ★ 创建事件循环：setsize 是容量上限，决定 events[] 数组能索引到多大的 fd。
     *   传 MAX_CLIENTS + 16 是留余量 —— 实际需要的 fd 数 = 连接数 + 几个
     *   （listen fd、epoll fd、标准流）。若 accept 得到的 fd 超出 setsize，
     *   注册会失败（aeCreateFileEvent 返回 AE_ERR）。 */
    aeEventLoop *el = aeCreateEventLoop(MAX_CLIENTS + 16);
    if (!el) {
        fprintf(stderr, "aeCreateEventLoop 失败\n");
        close(server_fd);
        zset_free(global_zset);
        storage_free(store);
        return EXIT_FAILURE;
    }

    /* 填全局服务状态（事件回调通过它拿到 store/zset）。
     * ★ 不再有 g_server.el —— 事件循环由回调参数传递，不放进全局。
     *   见 include/server.h 的 Server 注释。 */
    g_server.store = store;
    g_server.zset = global_zset;
    g_server.clients = NULL;
    g_server.numclients = 0;
    g_el = el;

    /* ★ 注册 beforesleep：睡之前的收尾（阶段 3 在这里 flush 各客户端 outbuf） */
    aeSetBeforeSleepProc(el, beforeSleep);

    /* ★ 把 listen fd 注册进事件循环 —— 它可读就表示「有新连接」。
     *   clientData 传 NULL（listen fd 没有对应的 Client）。 */
    if (aeCreateFileEvent(el, server_fd, AE_READABLE,
                          acceptTcpHandler, NULL) == AE_ERR) {
        fprintf(stderr, "注册 listen fd 失败\n");
        aeDeleteEventLoop(el);
        close(server_fd);
        zset_free(global_zset);
        storage_free(store);
        return EXIT_FAILURE;
    }

    printf("事件循环后端 = %s，最大连接数 = %d\n", aeGetApiName(), MAX_CLIENTS);

    /* ★ 主循环：所有事件（新连接、命令到达）都在这里被分发。
     *   对比改造前：accept 一个就陷进 readQueryFromClient 的阻塞读循环，
     *   同一时刻只能服务一个客户端。 */
    aeMain(el);

    // ---- 清理资源 ----
    printf("\n正在关闭服务器,释放内存...\n");

    /* 关闭所有还连着的客户端（正常退出时它们可能都还活着）。
     * ★ 复用 freeClient 的释放路径，而不是在这里把它的四步再抄一遍 ——
     *   抄一遍就等于「释放逻辑有两份」，将来给 Client 加字段时必定漏一处。
     *   传 NULL 表示不撤关注：事件循环马上就整个销毁了，逐个撤销纯属浪费。
     *   ★ 先存 next 再关：freeClient 会把自己从链表摘掉。 */
    Client *c = g_server.clients;
    while (c) {
        Client *next = c->next;
        freeClient(NULL, c);
        c = next;
    }
    g_server.clients = NULL;
    g_server.numclients = 0;

    aeDeleteEventLoop(el);     // 释放事件循环 + 后端状态（含 epoll 实例）
    close(server_fd);
    zset_free(global_zset);
    storage_free(store);
    printf("所有资源已释放，再见！\n");
    return 0;
}
