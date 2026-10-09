/* ============================================================
 * networking.c —— 网络驱动层（事件循环的所有回调都在这）
 * ============================================================
 *
 * ★ 对应 Redis 的 networking.c。职责：
 *   · acceptTcpHandler    —— listen fd 可读 → 循环 accept 新连接
 *   · readQueryFromClient —— 客户端可读 → 读一次、喂给解析层
 *   · beforeSleep         —— 本轮收尾（在这里 flush outbuf）
 *   · freeClient          —— 关闭连接（撤关注 → 关 fd → 摘链表 → 释放）
 *   · writeToClientRaw    —— ★ 全项目唯一执行 write() 的地方
 *
 * ★ 本文件【不含】（2026 拆分解耦后）：
 *   · 解析逻辑   → src/parser.c（三层解析器）
 *   · 响应编码   → src/reply.c（send_response + send_*）
 *   · 事件循环   → src/ae.c / src/ae_epoll.c
 *   · socket 创建 / Client 对象生命周期 → src/server.c
 *
 * 这样划分的依据是「变化的原因是否相同」：
 *   网络模型变了（阻塞 → epoll → io_uring）只改本文件；
 *   协议解析规则变了只改 parser.c；
 *   响应格式变了只改 reply.c。
 *
 * ★ 层间边界（本文件作为「中间的协调者」）：
 *
 *      [epoll 事件]
 *           ↓
 *    readQueryFromClient            ← 网络层：读一次，拿到字节
 *           ↓ processInputBuffer(c, g_server.store, g_server.zset)
 *    parser.c 三层解析器            ← 解析层：字节 → c->argv
 *           ↓ handle_command(...)
 *    commands.c                     ← 命令层：执行，调 send_*
 *           ↓ send_response(...)
 *    reply.c 追加进 c->outbuf        ← 编码层：永不阻塞
 *           ↓
 *    beforeSleep / writeToClientRaw ← 回到网络层：唯一真正 write 的地方
 *
 *   ★ 解析层的返回值 -1 表示「该断开了」，但怎么断由本文件决定 ——
 *     解析层不认识 close()，也不认识 aeDeleteFileEvent。
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "server.h"
#include "util.h"       /* BACKLOG / set_nonblocking（全项目共用一份） */

/* 全局服务状态 —— ★ 定义在这里（而不是 main.c）：
 * 它是「网络驱动层需要的共享状态」，本文件是它的主要使用者
 * （accept 回调要用 store/zset、freeClient 要用 el）。
 * 定义在 networking.c 后，任何链接本文件的目标都可以用，
 * 包括 stage1 那种自己写 main 的独立产物。 */
Server g_server;

/* 前向声明：可写事件回调（定义在下方，签名要匹配 aeFileProc）。
 * writeToClientRaw 注册的就是它。 */
static void writeToClientHandler(aeEventLoop *el, int fd, void *clientData, int mask);

/* 单次 read 的临时缓冲。★ 注意：它是【栈上的局部变量】，
 * 不是每连接一份 —— 因为非阻塞 read 一次只读一小块，
 * 读完立刻 append 进 querybuf（sds），不需要在两次回调间保留。 */
#define READ_CHUNK 4096

/* 每个连接允许积压的最大请求缓冲，防止恶意客户端只发不解析撑爆内存 */
#define MAX_QUERYBUF (1024 * 1024 * 64)

/* ------------------------------------------------------------
 * 客户端链表：增 / 删（都是 O(1)）
 * ------------------------------------------------------------
 * ★ 这两个函数本可以改成「把 g_server 作为参数传入」的更干净形式，
 *   但它们只在 accept/free 各调一次，且 g_server.clients 就是"客户端集合"
 *   本身 —— 传参只会让调用点更长。真正需要避免的是【能拿到却不用的
 *   隐式依赖】（如 el），不是所有全局访问。 */
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
 *   自动跳过（fe->mask & mask & AE_READABLE == 0）。
 *
 * ★ 为什么加 el 参数（2026 拆分解耦）：撤关注需要事件循环，而
 *   所有调用点都【手头就有】它 —— 回调的签名里第一个参数就是 el。
 *   之前读 g_server.el 是隐式依赖：既多了一层"必须记得先初始化
 *   g_server.el"的隐含契约，也让这个函数无法在测试里作用到另一个
 *   事件循环上。传 NULL 表示「不撤关注」（仅供无事件循环的场景）。 */
void freeClient(aeEventLoop *el, Client *c) {
    if (!c) return;

    /* ★ 第 ⓪ 步：把 outbuf 里还没发出去的数据尽力写完。
     *
     *   为什么必须做：send_response 是【入队】而不是直接 write。
     *   如果关闭前不 flush，像「协议错误 → send_error → 立刻断开」
     *   这样的路径会把整条错误回复丢掉 —— 客户端什么也收不到。
     *   （改造前 send_response 是直接 write，所以没这个问题。）
     *
     *   这里是【尽力而为】的同步发送：循环 write 直到发完或真的写不动。
     *   为什么可以在这里阻塞：这是连接的最后时刻，我们马上要关它，
     *   短暂阻塞不影响别的连接，而且通常一次就写完。
     *   ⚠ 但对端完全不读时不能无限等 —— 所以 EAGAIN 就放弃剩余数据
     *     （宁可丢掉尾部，也不能把整个服务器拖住）。 */
    if (sdslen(c->outbuf) > 0) {
        size_t sent = 0;
        size_t total = sdslen(c->outbuf);
        while (sent < total) {
            ssize_t n = write(c->fd, c->outbuf + sent, total - sent);
            if (n > 0) { sent += (size_t)n; continue; }
            if (n < 0 && errno == EINTR) continue;
            break;   /* EAGAIN 或写错误：发不完了，放弃剩余部分 */
        }
    }

    /* ① 撤关注：先读后写，两个都要 */
    if (el) {
        aeDeleteFileEvent(el, c->fd, AE_READABLE);
        aeDeleteFileEvent(el, c->fd, AE_WRITABLE);
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
    sdsfree(c->outbuf);
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
            /* EMFILE/ENFILE 等：★ 必须 break（详见下方 EMFILE 说明） */
            perror("accept");
            break;
        }

        /* ① 新 fd 设非阻塞 —— ★ 最容易漏的一步 */
        if (set_nonblocking(cfd) == -1) {
            perror("set_nonblocking");
            close(cfd);
            continue;
        }

        /* ② 连接数上限检查。
         *    ★ 这里是「accept 之后」检查 —— 三种放置位置的取舍：
         *      · accept 之前：必须自己维护"待接受的积压数"，且在 LT 模式下
         *        无法区分"队列里真有一个"还是"伪唤醒"，容易多算/漏算。
         *      · ★ accept 之后（本实现）：代价是 accept 一个再 close 掉，
         *        多一次系统调用。换来的是判断总是基于真实计数，绝不误判。
         *      · 拒绝方式：直接 close 会让客户端收到 RST（或空回复），
         *        更好的做法是先回一条 "-ERR max number of clients reached"
         *        再关（Redis 就这么做）—— 留待阶段 4。
         *    Redis 在 acceptTcpHandler 里也是 accept 之后再查 maxclients。 */
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
            freeClient(el, c);
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
    (void)mask;
    Client *c = (Client *)clientData;
    char buf[READ_CHUNK];

    ssize_t n = read(fd, buf, sizeof(buf));

    if (n > 0) {
        /* 防御：请求缓冲不能无限增长（恶意客户端只发不解析） */
        if (sdslen(c->querybuf) + (size_t)n > MAX_QUERYBUF) {
            fprintf(stderr, "[fd=%d] 请求缓冲超限，断开\n", fd);
            freeClient(el, c);
            return;
        }

        /* 追加到 sds 尾部。★ 先存临时变量：sdscatlen OOM 时返回 NULL
         * 但不释放旧 sds，直接赋给 c->querybuf 会因 NULL 覆盖而泄漏。 */
        sds nb = sdscatlen(c->querybuf, buf, (size_t)n);
        if (!nb) {
            sdsfree(c->querybuf);
            c->querybuf = NULL;
            fprintf(stderr, "[fd=%d] 缓冲区扩容失败\n", fd);
            freeClient(el, c);
            return;
        }
        c->querybuf = nb;

        /* 交给解析层（三层解析器，在 src/parser.c）。
         * ★ 它内部循环处理粘包，直到半包或缺数据才返回。
         *   返回 -1 表示协议错误 → 断开。
         * ★ store/zset 只能走 g_server：解析层要执行命令，需要它们，
         *   但回调签名里没有 —— 这才是全局变量存在的正当理由。 */
        int prc = processInputBuffer(c, g_server.store, g_server.zset);
        if (prc == -1) {
            fprintf(stderr, "[fd=%d] 协议错误，断开连接\n", fd);
            freeClient(el, c);
        }
        return;
    }

    if (n == 0) {
        /* 对端正常关闭 */
        printf("[fd=%d] 客户端断开\n", fd);
        freeClient(el, c);
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
    freeClient(el, c);
}

/* ============================================================
 * writeToClient —— ★ 唯一执行 write() 的地方
 * ============================================================
 * 把 c->outbuf 里的数据尽力写出去。三条规则缺一不可：
 *
 *   ① 部分成功（0 < n < len）→ 消费掉已写出的部分，【继续循环】
 *      因为刚刚成功了，说明内核缓冲区还有空间，再试一次很可能仍有收益。
 *
 *   ② EAGAIN → ★ 立刻返回，绝不重试
 *      内核缓冲区现在是满的，重试 1000 次结果完全一样（它不会因为你
 *      重试而腾出空间，只会因为对端读了才腾出）。必须：
 *        · 注册 AE_WRITABLE，让内核在「有空间」时通知我们
 *        · 立刻把控制权交回事件循环（去服务别的 fd）
 *
 *   ③ outbuf 空了 → ★ 必须取消 AE_WRITABLE
 *      否则 epoll_wait 会【疯狂】返回「可写」（socket 大多数时候都可写）
 *      → 回调被反复调用 → 什么都不做 → 100% CPU 空转。
 *      这是 epoll 编程最经典的性能陷阱，而且不报任何错。
 *
 * 返回值：1 = 发完（已取消可写注册）；0 = 没发完（已注册可写）；-1 = 出错。
 *
 * ★ 签名必须匹配 aeFileProc —— 因为它自己会被注册为 wfileProc：
 *     writeToClient(loop, fd, clientData, mask)
 *   这是「自己注册自己」的递归结构：写不完 → 注册可写 → 内核通知 →
 *   再调本函数 → 继续写。 */
/* 内部实现：真正干活，返回 1/0/-1（调用方需要区分「发完」和「没发完」）。
 * ★ el 由调用方传入（回调的 el 参数 / beforeSleep 的参数）——
 *   本函数不再读 g_server.el，这样它在任何事件循环上都可用。 */
static int writeToClientRaw(aeEventLoop *el, Client *c) {
    if (!c || !el) return -1;

    while (sdslen(c->outbuf) > 0) {
        size_t remaining = sdslen(c->outbuf);
        ssize_t n = write(c->fd, c->outbuf, remaining);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* ② 内核满了 → 注册可写，等通知。★ 不重试、不忙等 */
                if (aeCreateFileEvent(el, c->fd, AE_WRITABLE,
                                      writeToClientHandler, c) == AE_ERR) {
                    return -1;      /* 注册不上（fd 越界/OOM），让调用方关闭 */
                }
                return 0;
            }
            if (errno == EINTR) continue;   /* 被信号打断，重试这次 write */
            /* EPIPE / ECONNRESET 等：对端已关闭 → 让调用方 freeClient */
            return -1;
        }

        if (n == 0) {
            /* 防御性写法：按 POSIX，count > 0 时成功返回不会是 0。
             * 万一真遇到，continue 会变成空转死循环，所以按「发不出去」处理。 */
            return -1;
        }

        /* ① 部分或全部写出 → 消费掉已发出的部分 */
        sdsrange(c->outbuf, n, -1);
    }

    /* ③ outbuf 空了 → ★ 必须取消可写通知，否则 epoll_wait 会忙转 */
    aeDeleteFileEvent(el, c->fd, AE_WRITABLE);
    return 1;
}

/* 可写事件回调包装：aeFileProc 返回 void，而内部实现要返回状态码，
 * 所以包一层。★ 这就是「回调签名的适配层」。 */
static void writeToClientHandler(aeEventLoop *el, int fd, void *clientData, int mask) {
    (void)fd; (void)mask;
    Client *c = (Client *)clientData;
    if (!c) return;
    if (writeToClientRaw(el, c) == -1) {
        /* 写错误 → 关闭。注意：此刻我们是在事件分发循环【内部】，
         * freeClient 会把 mask 清成 AE_NONE，分发循环会自动跳过它。 */
        freeClient(el, c);
    }
}

/* ============================================================
 * beforeSleep —— 本轮收尾（睡之前）
 * ============================================================
 * ★ 放这里而不是「命令执行完立刻写」，三个理由：
 *   ① 攒批：一条 +OK 从 3 次 write 降到 1 次（改造前是 3 次系统调用）
 *   ② 谁要关闭已确定 → 不会白写
 *   ③ 写完即可撤掉 EPOLLOUT → epoll_wait 才能睡好
 *      （EPOLLOUT 几乎总是就绪，留着它 epoll_wait 会立刻返回 = 忙转）
 *
 * ★ 遍历时的陷阱：writeToClientRaw 可能因为写错误而 freeClient(c)，
 *   那样 c 这个节点就被摘掉了 —— 后续 c->next 就是 use-after-free。
 *   所以【先存 next 再处理】（下面 next 变量就是这么做的）。
 *
 *   ⚠ 更稳的做法是「先收集要关闭的，遍历完再统一关」（惰性删除思路）。
 *     当前的「先存 next」在 writeToClientRaw 只释放【自己】的前提下是安全的：
 *     它不会去释放别的 Client。 */
void beforeSleep(aeEventLoop *el) {
    if (!el) return;

    Client *c = g_server.clients;
    while (c) {
        Client *next = c->next;      /* ★ 先存，因为 c 可能被释放 */

        if (sdslen(c->outbuf) > 0) {
            if (writeToClientRaw(el, c) == -1) {
                freeClient(el, c);    /* 写错误 → 关闭（c 已摘出链表） */
            }
        }
        c = next;
    }
}
