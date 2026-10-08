/* ============================================================
 * networking.c —— 网络驱动层（事件循环的所有回调都在这）
 * ============================================================
 *
 * ★ 对应 Redis 的 networking.c。职责：
 *   · acceptTcpHandler    —— listen fd 可读 → 循环 accept 新连接
 *   · readQueryFromClient —— 客户端可读 → 读一次、喂给解析层
 *   · beforeSleep         —— 本轮收尾（阶段 3 在这里 flush outbuf）
 *   · freeClient          —— 关闭连接（撤关注 → 关 fd → 摘链表 → 释放）
 *
 * ★ 本文件【不含】：
 *   · 解析逻辑（在 server.c 的 processInputBuffer 等）
 *   · 响应编码（在 server.c 的 send_* 系列）
 *   · 事件循环本身（在 ae.c / ae_epoll.c）
 *
 * 这样划分的依据是「变化的原因是否相同」：
 *   网络模型变了（阻塞 → epoll）只改本文件；
 *   协议解析规则变了只改 server.c 的解析层；
 *   响应格式变了只改 send_* 系列。
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "server.h"

/* 全局服务状态 —— ★ 定义在这里（而不是 main.c）：
 * 它是「网络驱动层需要的共享状态」，本文件是它的主要使用者
 * （accept 回调要用 store/zset、freeClient 要用 el）。
 * 定义在 networking.c 后，任何链接本文件的目标都可以用，
 * 包括 stage1 那种自己写 main 的独立产物。 */
Server g_server;

/* 单次 read 的临时缓冲。★ 注意：它是【栈上的局部变量】，
 * 不是每连接一份 —— 因为非阻塞 read 一次只读一小块，
 * 读完立刻 append 进 querybuf（sds），不需要在两次回调间保留。 */
#define READ_CHUNK 4096

/* 等待连接队列长度（传给 listen） */
#define BACKLOG 10

/* 每个连接允许积压的最大请求缓冲，防止恶意客户端只发不解析撑爆内存 */
#define MAX_QUERYBUF (1024 * 1024 * 64)

/* ------------------------------------------------------------
 * 设为非阻塞
 * ------------------------------------------------------------
 * ★ 这是整个改造的前提：不设非阻塞，read() 会卡住事件循环。
 *   accept 出来的新 fd【不会】继承 listen fd 的非阻塞属性，
 *   所以每个新连接都要单独设一次。 */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ------------------------------------------------------------
 * 客户端链表：增 / 删（都是 O(1)）
 * ------------------------------------------------------------ */
static void clients_add(Client *c) {
    c->prev = NULL;
    c->next = g_server.clients;
    if (g_server.clients) g_server.clients->prev = c;
    g_server.clients = c;
    g_server.numclients++;
}

static void clients_remove(Client *c) {
    if (c->prev) c->prev->next = c->next;
    else         g_server.clients = c->next;
    if (c->next) c->next->prev = c->prev;
    c->next = c->prev = NULL;
    g_server.numclients--;
}

/* ============================================================
 * freeClient —— 关闭连接并释放
 * ============================================================
 * ★ 四步顺序不能换：
 *
 *   ① 撤关注（DEL READABLE / DEL WRITABLE）
 *      · 必须在 close 之前：epoll_ctl 要求 fd 有效，close 后会 EBADF
 *      · 且 fd 编号会被内核【立即】复用，先 DEL 才能保证删的是旧的那个
 *      · 两个都要删：漏了写事件，内核会持续报告这个 fd 可写（垃圾事件）
 *
 *   ② close(fd)
 *      · fd 不会被自动释放；不 close 就泄漏编号
 *      · 泄漏到 ulimit -n 上限后，accept 开始失败，服务器再也接不了新连接
 *
 *   ③ 从链表摘除（O(1)，双向链表）
 *
 *   ④ 释放内存
 *
 * ★ aeDeleteFileEvent 在 mask 归零时会把 fe->clientData 清成 NULL ——
 *   这就是「清账」：防止 fd 被复用给新连接后，分发循环按旧指针访问
 *   已释放的 Client（use-after-free）。
 *
 * ⚠ 危险场景：本函数可能在「遍历 fired[] 的过程中」被调用
 *   （比如 read 回调发现协议错误）。此时 fired[] 里可能还有指向
 *   【同一个 fd】的项 —— 靠上面那条「清 clientData」让分发循环
 *   自动跳过（fe->mask & mask & AE_READABLE == 0）。 */
void freeClient(Client *c) {
    if (!c) return;

    /* ① 撤关注：先读后写，两个都要 */
    if (g_server.el) {
        aeDeleteFileEvent(g_server.el, c->fd, AE_READABLE);
        aeDeleteFileEvent(g_server.el, c->fd, AE_WRITABLE);
    }

    /* ② 关 fd */
    close(c->fd);

    /* ③ 从链表摘除 */
    clients_remove(c);

    /* ④ 释放内存 */
    if (c->argv) {
        for (int i = 0; i < c->argc; i++) sdsfree(c->argv[i]);
        free(c->argv);
    }
    sdsfree(c->querybuf);
    free(c);
}

/* ============================================================
 * acceptTcpHandler —— listen fd 可读
 * ============================================================
 * ★ 循环 accept 到 EAGAIN，三个必做项：
 *   ① 每个新 fd 立刻 set_nonblocking（否则以后 read 会卡死整个循环）
 *   ② maxclients 检查（在 EMFILE 之前就拦住）
 *   ③ 注册 AE_READABLE，clientData 挂 Client*
 *
 * 为什么循环：一次 epoll 通知只保证「至少有一个连接可 accept」，
 * 不保证只有一个。循环到 EAGAIN 可以一趟拿完队列里的全部连接，
 * 而且不依赖触发模式（LT/ET 下都正确）。 */
void acceptTcpHandler(aeEventLoop *el, int fd, void *clientData, int mask) {
    (void)clientData; (void)mask;

    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);

        int cfd = accept(fd, (struct sockaddr *)&caddr, &clen);

        if (cfd < 0) {
            /* ★ EAGAIN/EWOULDBLOCK = 队列空了 = 正常出口，不是错误 */
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;          /* 信号打断，重试 */
            /* EMFILE(fd 用光) / ENFILE(系统级) 等：★ 必须 break，
             * continue 会变成死循环（资源不释放，accept 永远失败） */
            perror("accept");
            break;
        }

        /* ① 新 fd 设非阻塞 —— ★ 最容易漏的一步 */
        if (set_nonblocking(cfd) == -1) {
            perror("set_nonblocking");
            close(cfd);
            continue;
        }

        /* ② 连接数上限检查 */
        if (g_server.numclients >= MAX_CLIENTS) {
            fprintf(stderr, "连接数已达上限 %d，拒绝 fd=%d\n", MAX_CLIENTS, cfd);
            close(cfd);
            continue;
        }

        /* 缓冲区 / keepalive / nodelay（沿用原 anet 封装） */
        anetConfigClientSocket(cfd);

        Client *c = create_client(cfd);
        if (!c) {
            close(cfd);
            continue;
        }
        clients_add(c);

        /* ③ 注册可读：clientData 挂 Client* —— ae 层只当它是 void*，
         *    回调时原样还给我们（readQueryFromClient 的第三个参数） */
        if (aeCreateFileEvent(el, cfd, AE_READABLE,
                              readQueryFromClient, c) == AE_ERR) {
            fprintf(stderr, "注册 fd=%d 失败\n", cfd);
            freeClient(c);
            continue;
        }

        printf("[fd=%d] 新连接来自 %s（当前 %d 个）\n",
               cfd, inet_ntoa(caddr.sin_addr), g_server.numclients);
    }
}

/* ============================================================
 * readQueryFromClient —— 客户端 fd 可读
 * ============================================================
 * ★ 与改造前的根本区别：这里【不再有 while 循环】。
 *
 * 改造前（阻塞模型）：
 *     while (Keep_running) { read(...); processInputBuffer(...); }
 *   函数一直不返回，直到该客户端断开 —— 于是只能服务一个客户端。
 *
 * 改造后（事件循环）：
 *     读【一次】→ 追加到 querybuf → 交给解析层 → 立刻返回。
 *   剩下的数据下次 epoll 还会通知（水平触发），或下次读完继续。
 *
 * 这与 Redis 的 readQueryFromClient 语义一致。 */
void readQueryFromClient(aeEventLoop *el, int fd, void *clientData, int mask) {
    (void)el; (void)mask;
    Client *c = (Client *)clientData;
    char buf[READ_CHUNK];

    ssize_t n = read(fd, buf, sizeof(buf));

    if (n > 0) {
        /* 防御：请求缓冲不能无限增长（恶意客户端只发不解析） */
        if (sdslen(c->querybuf) + (size_t)n > MAX_QUERYBUF) {
            fprintf(stderr, "[fd=%d] 请求缓冲超限，断开\n", fd);
            freeClient(c);
            return;
        }

        /* 追加到 sds 尾部。★ 先存临时变量：sdscatlen OOM 时返回 NULL
         * 但不释放旧 sds，直接赋给 c->querybuf 会因 NULL 覆盖而泄漏。 */
        sds nb = sdscatlen(c->querybuf, buf, (size_t)n);
        if (!nb) {
            sdsfree(c->querybuf);
            c->querybuf = NULL;
            fprintf(stderr, "[fd=%d] 缓冲区扩容失败\n", fd);
            freeClient(c);
            return;
        }
        c->querybuf = nb;

        /* 交给解析层（三层解析器，原封不动）。
         * ★ 它内部循环处理粘包，直到半包或缺数据才返回。
         *   返回 -1 表示协议错误 → 断开。 */
        int prc = processInputBuffer(c, g_server.store, g_server.zset);
        if (prc == -1) {
            fprintf(stderr, "[fd=%d] 协议错误，断开连接\n", fd);
            freeClient(c);
        }
        return;
    }

    if (n == 0) {
        /* 对端正常关闭 */
        printf("[fd=%d] 客户端断开\n", fd);
        freeClient(c);
        return;
    }

    /* n < 0 */
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* ★「现在没数据」是正常情况，不是错误。直接返回等下次通知。
         *   若这里 continue 重试 → 100% CPU 空转。 */
        return;
    }
    if (errno == EINTR) return;    /* 被信号打断，下次再说 */

    perror("read");
    freeClient(c);
}

/* ============================================================
 * beforeSleep —— 本轮收尾（睡之前）
 * ============================================================
 * ★ 放这里而不是「命令执行完立刻写」，三个理由：
 *   ① 攒批：一条 +OK 的 3 段 write 合成 1 次系统调用
 *   ② 谁要关闭已确定 → 不会白写
 *   ③ 写完即可撤掉 EPOLLOUT → epoll_wait 才能睡好
 *      （EPOLLOUT 几乎总是就绪，留着它 epoll_wait 会立刻返回 = 忙转）
 *
 * ★ 阶段 3 会在这里实现：遍历 clients，把 outbuf 里的数据 write 出去。
 *   阶段 2 还没引入 outbuf（send_response 仍是阻塞直写），所以这里是占位。 */
void beforeSleep(aeEventLoop *el) {
    (void)el;
    /* 阶段 3: 遍历 g_server.clients，处理各自 outbuf */
}
