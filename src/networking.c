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
#include <fcntl.h>     /* open()：预留应急 fd */
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "server.h"
#include "util.h"       /* BACKLOG / set_nonblocking（全项目共用一份） */

/* 全局服务状态 —— ★ 定义在这里（而不是 main.c）：
 * 它是「网络驱动层需要的共享状态」，本文件是它的主要使用者
 * （accept 回调要用 store/zset）。
 * 定义在 networking.c 后，任何链接本文件的目标都能读到它 ——
 * 而不必依赖「必须先链接 main.c」这个隐含顺序。 */
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
    if (c->outbuf_pos < sdslen(c->outbuf)) {
        /* ★ 从 outbuf_pos 开始发：前面 [0, outbuf_pos) 是【已经写出过】的，
         *   重发会造成客户端收到重复字节（RESP 流错位）。 */
        size_t sent = c->outbuf_pos;
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

/* ------------------------------------------------------------
 * 应急 fd —— fd 耗尽时还能回一条错误消息的唯一办法
 * ------------------------------------------------------------
 * ★ 问题：accept() 因 EMFILE 失败时，内核【没有返回 fd】。
 *   没有 fd 就没法 write 任何东西 —— 客户端会被晾着，直到自己超时。
 *   运维在服务端也只看到一行 "Too many open files"。
 *
 * ★ 解法（Redis 同款）：启动时预留 1 个 fd 一直占着不释放。
 *   于是真实可用 fd 数比 RLIMIT 少 1；真到 EMFILE 时：
 *     ① close(应急 fd)      → 立刻腾出 1 个名额
 *     ② 再 accept 一次       → 这次能成功，拿到那个名额
 *     ③ 回 -ERR 后 close     → 客户端知道原因了
 *     ④ 重新 open 占住名额   → 为下一次 EMFILE 做准备
 *
 * ★ 为什么用 /dev/null 去占：
 *   它是永远可用的 fd 来源；用 dup() 一个必须合法存在的 fd 也行，
 *   但 /dev/null 的语义最没有歧义（不占磁盘、不占设备）。
 */
static int g_crisis_fd = -1;

/* 前向声明：应急 fd 的处理要用到它，而它的定义在后面 */
static size_t rejectWithError(int fd, const char *msg);

/* 启动时调用一次：预留应急 fd。
 * ★ 必须在创建事件循环【之前】调：要保证这个 fd 号码落在
 *   ae 的 events[] 容量范围内？—— 不需要，它从不注册进事件循环，
 *   只是一个被占用的名额。 */
void reserveCrisisFd(void) {
    g_crisis_fd = open("/dev/null", O_RDONLY);
    if (g_crisis_fd == -1) {
        /* 占不到位不影响正确性，只是 EMFILE 时无法告知客户端 */
        fprintf(stderr, "[init] 无法预留应急 fd（%s），EMFILE 时将无法回 -ERR\n",
                strerror(errno));
        return;
    }
    printf("[init] 已预留应急 fd=%d（fd 耗尽时用它回 -ERR）\n", g_crisis_fd);
}

/* fd 耗尽时：腾名额 → accept 一个 → 回错误 → 关 → 重新占位 */
static void acceptAndRejectWhenExhausted(int listen_fd, int saved_errno) {
    if (g_crisis_fd == -1) {
        /* 没有应急 fd：只能放弃，客户端会自己超时 */
        return;
    }

    /* ① 腾出 1 个名额 */
    close(g_crisis_fd);
    g_crisis_fd = -1;

    /* ② 现在这次 accept 应当能成功（拿到刚腾出的那个 fd 号） */
    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);
    int cfd = accept(listen_fd, (struct sockaddr *)&caddr, &clen);
    if (cfd >= 0) {
        /* ③ 告知原因后关闭
         *    ★ 消息里带上具体错误码：ENFILE 是「整台机器满了」，
         *      EMFILE 是「本进程配额满了」，运维处置方式完全不同。 */
        rejectWithError(cfd, saved_errno == ENFILE
                             ? "out of file descriptors (system-wide, ENFILE)"
                             : "out of file descriptors (EMFILE)");
        close(cfd);
    }

    /* ④ 重新占住名额，为下一次 EMFILE 做准备
     *    ★ 即使上面 accept 失败也要重新占 —— 否则下一次 EMFILE
     *      就彻底没有应急手段了。 */
    g_crisis_fd = open("/dev/null", O_RDONLY);
}

/* ------------------------------------------------------------
 * rejectWithError —— 关闭一个 fd 之前，先尽力把错误消息发出去
 * ------------------------------------------------------------
 * ★ 为什么必须有它（而不是直接 close）：
 *   直接 close() 时，若接收缓冲里还有客户端已发、我们没读的数据，
 *   内核会发 RST 而不是 FIN —— 客户端看到的是 "Connection reset by
 *   peer"，拿不到任何可读原因。运维于是分不清：
 *     「服务器满了」还是「网络抖动」还是「我自己协议写错了」。
 *   Redis 在 maxclients 超限时同样先发
 *   "-ERR max number of clients reached" 再关，本项目对齐这个行为。
 *
 * ★ 调用约束：
 *   · 只用于【尚未注册进事件循环】的 fd（accept 出来但还没 create_client）。
 *     已经注册的 fd 必须走 send_response + freeClient，不能走这里。
 *   · 先 shutdown(SHUT_WR) 再 close：让对端先收到 FIN，
 *     能确定「这条消息是完整的」，避免 close 因未读数据触发 RST 把它冲掉。
 *   · 发送是【尽力而为】：EAGAIN/EPIPE 立刻放弃，绝不在这里等待 ——
 *     本函数在 accept 热路径上，一旦阻塞就会卡住整个事件循环。
 *
 * 返回实际写出的字节数（仅供日志，调用方不依赖）。
 */
static size_t rejectWithError(int fd, const char *msg) {
    if (fd < 0 || !msg) return 0;

    char buf[256];
    const char *prefix = "-ERR ";
    size_t plen = 5, mlen = strlen(msg);
    size_t total = plen + mlen + 2;

    if (total >= sizeof(buf)) {
        /* 消息异常长：截断，绝不允许越界（这是内核路径上的栈缓冲） */
        mlen = sizeof(buf) - plen - 3;
        total = plen + mlen + 2;
    }
    memcpy(buf, prefix, plen);
    memcpy(buf + plen, msg, mlen);
    memcpy(buf + plen + mlen, "\r\n", 2);

    size_t off = 0;
    while (off < total) {
        ssize_t n = write(fd, buf + off, total - off);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        break;                  /* EAGAIN / EPIPE：放弃，不重试 */
    }
    shutdown(fd, SHUT_WR);
    return off;
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

            /* ---- fd 耗尽（EMFILE = 本进程到上限，ENFILE = 系统到上限）----
             *
             * ★ 这是「沉默故障」的经典现场，必须专门处理，光 break 不够：
             *   accept 失败时内核【没有给你 fd】，所以连「回一条错误再关」
             *   都做不到 —— 客户端只会被晾在那里，直到它自己超时。
             *   运维这边只能看到 perror 打一行 "Too many open files"。
             *
             * ★ 解法（Redis 同款）：预留一个「应急 fd」。
             *   正常情况下它一直占着不用，于是真实 fd 用量比 RLIMIT 少 1；
             *   真到 EMFILE 时，把它 close 掉就立刻腾出 1 个 fd 名额，
             *   用这个名额去 accept、回一条 -ERR、再关 ——
             *   客户端至少知道「服务器资源耗尽」，而不是干等。
             *   处理完立刻重新占住那个名额，为下一次做准备。
             *
             * ★ 为什么用 static 而不是放进 Server 结构：
             *   它是纯粹的「fd 配额管理」，和业务状态无关；
             *   而且只能由 accept 路径读写，放全局反而更容易看清它只有一份。 */
            if (errno == EMFILE || errno == ENFILE) {
                fprintf(stderr,
                        "[listen] fd 耗尽（%s），用应急 fd 回 -ERR 后继续\n",
                        errno == EMFILE ? "EMFILE" : "ENFILE");
                acceptAndRejectWhenExhausted(fd, errno);
                break;   /* 本轮不再 accept：腾出的名额已用掉 */
            }

            /* 其他 accept 错误（ECONNABORTED / EPROTO 等）：本次连接坏了，
             * ★ 用 break 而不是 return —— accept 循环的约定是「一次
             *   通知尽量拿完队列」。continue 会在这里空转（同一个错误重复出现）。 */
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
         *      · 拒绝方式：★ 先回一条 "-ERR max number of clients reached"
         *        再关（对齐 Redis 行为）。直接 close 会让客户端只收到
         *        RST —— 运维看到的日志是 "Connection reset by peer"，
         *        完全区分不出「服务器满了」「网络故障」「自己写错了」。
         *    Redis 在 acceptTcpHandler 里也是 accept 之后再查 maxclients。 */
        if (g_server.numclients >= MAX_CLIENTS) {
            fprintf(stderr,
                    "[fd=%d] 连接数已达上限 %d，回 -ERR 后关闭\n",
                    cfd, MAX_CLIENTS);
            rejectWithError(cfd, "max number of clients reached");
            close(cfd);
            continue;
        }

        /* 缓冲区 / keepalive / nodelay（沿用原 anet 封装） */
        anetConfigClientSocket(cfd);

        Client *c = create_client(cfd);
        if (!c) {
            /* ★ 分配 Client 失败 = 内存耗尽。以前直接 close，客户端只看到
             *   连接被重置；现在先告诉它原因，运维也能从客户端侧看到。 */
            fprintf(stderr, "[fd=%d] 创建 Client 失败（OOM），回 -ERR 后关闭\n", cfd);
            rejectWithError(cfd, "out of memory");
            close(cfd);
            continue;
        }
        clients_add(c);

        /* ③ 注册可读：clientData 挂 Client* —— ae 层只当它是 void*，
         *    回调时原样还给我们（readQueryFromClient 的第三个参数） */
        if (aeCreateFileEvent(el, cfd, AE_READABLE,
                              readQueryFromClient, c) == AE_ERR) {
            /* ★ 注册失败意味着这个连接永远不会被调度到 —— 不告诉客户端
             *   就是「连上了但永远不回话」，对客户端而言是最难排查的形态。
             *   这里先把消息写进 outbuf，再由 freeClient 的收尾 flush 发出。 */
            fprintf(stderr, "[fd=%d] 注册事件失败（fd 超出事件数组容量？），回 -ERR 后关闭\n",
                    cfd);
            send_error(c, "ERR server cannot register event for this connection");
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
        /* ★ 更新空闲计时：客户端确实发东西来了。
         *   放在最前面 —— 只要读到了数据，无论后面解析成败都算这次活动。
         *   （否则「一直发非法命令」的连接会被误判为空闲，
         *     而它其实很活跃，只是内容不对 —— 那该由协议错误路径处理。） */
        c->lastinteraction = aeGetTimeMilliseconds();

        /* 防御：请求缓冲不能无限增长（恶意客户端只发不解析）。
         *
         * ★ 断开前要先告诉客户端「为什么」。
         *   以前这里只打一行服务端日志就 close，客户端什么也收不到 ——
         *   它只知道「连接莫名其妙断了」，不知道是自己发了一条声明长度
         *   巨大却永远发不完的命令。排查成本差一个数量级。
         *
         * ★ 为什么走 send_error + freeClient，而不是 rejectWithError：
         *   这个 fd 已经注册进事件循环了，错误消息必须先入 outbuf，
         *   再由 freeClient 的收尾 flush 同步写出（那条路径已经存在）。
         *   用 rejectWithError 直接 write 会绕过 outbuf，和已有的
         *   「可能半截回复已入队」状态打架。 */
        if (sdslen(c->querybuf) + (size_t)n > MAX_QUERYBUF) {
            fprintf(stderr,
                    "[fd=%d] 请求缓冲超限（%zu + %zd > %d），回 -ERR 后断开\n",
                    fd, sdslen(c->querybuf), n, MAX_QUERYBUF);
            send_error(c, "ERR query buffer limit reached (client-query-buffer-limit)");
            freeClient(el, c);
            return;
        }

        /* 追加到 sds 尾部。★ 先存临时变量：sdscatlen OOM 时返回 NULL
         * 但不释放旧 sds，直接赋给 c->querybuf 会因 NULL 覆盖而泄漏。 */
        sds nb = sdscatlen(c->querybuf, buf, (size_t)n);
        if (!nb) {
            fprintf(stderr, "[fd=%d] 请求缓冲扩容失败（OOM），回 -ERR 后关闭\n", fd);
            /* ★ 顺序很重要：先把错误写进 outbuf，再释放 querybuf。
             *   send_error 只碰 outbuf，不碰 querybuf，所以这里安全；
             *   而 freeClient 的收尾 flush 会把它同步发出去。 */
            send_error(c, "ERR out of memory (query buffer grow failed)");
            sdsfree(c->querybuf);
            c->querybuf = NULL;
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
            /* ★ 注意这里【不再】补发 send_error：解析层在返回 -1 之前
             *   已经发过 "-ERR protocol error"（见 parser.c）。再发一条
             *   会让客户端收到两条错误，是协议失步的开始。
             *   这里只负责记录日志并断开。 */
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

    /* ★★★ 这里用 outbuf_pos 做"已发送游标"，而不是每写完一块就把剩下的
     *     全部 memmove 到头部。这是本次混合负载测试挖出来的真实性能缺陷：
     *
     *     旧写法：write(n) 成功后执行 sdsrange(c->outbuf, n, -1)
     *             → 把 len-n 字节整体 memmove 到缓冲区开头。
     *     问题：对端读得慢时 outbuf 会堆到几十 MB，而每次只写出几 KB
     *           （socket 缓冲就那么大），于是每写一次都要搬动剩下的
     *           几十 MB —— 总工作量是 O(n²/块大小)。
     *     实测（本机，/tmp/memmove_bench）：64MB 缓冲、每次写 4KB 时，
     *     单次 memmove 要 6.4ms，2000 次写累计 12.7 秒；
     *     而同样的数据量如果只搬一次，总耗时 < 50ms。
     *     这就是"只发不收的客户端能把别人 P99 打到 790ms"的真正原因。
     *
     *     修法（Redis 的 qb_pos 手法）：只推进游标，写完整块时才回收一次空间。
     *     这样每次 write 之后不再搬内存，代价摊到"缓冲区真正空了"的那一刻。 */
    while (c->outbuf_pos < sdslen(c->outbuf)) {
        size_t remaining = sdslen(c->outbuf) - c->outbuf_pos;
        ssize_t n = write(c->fd, c->outbuf + c->outbuf_pos, remaining);

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

        /* ① 部分或全部写出 → ★ 只推进游标，不搬内存 */
        c->outbuf_pos += (size_t)n;
    }

    /* ③ outbuf 全部发完 → 一次性地把空间回收掉（只在这里 memmove/realloc，
     *    也就是"攒够了再清"，而不是每次 write 都清） */
    if (c->outbuf_pos > 0) {
        sdsrange(c->outbuf, (long)c->outbuf_pos, -1);
        c->outbuf_pos = 0;
    }

    /* ④ 空了 → ★ 必须取消可写通知，否则 epoll_wait 会忙转 */
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

        /* ★ 输出缓冲超限的客户端在这里统一踢掉。
         *   为什么放在 beforeSleep 而不是在 send_response 里直接断：
         *   send_response 可能深藏在「解析 → 执行命令 → 编码回复」的
         *   调用栈里，那一刻断开会把调用栈上所有还在用的 Client*
         *   变成悬垂指针。beforeSleep 是「本轮所有回调都已返回」的安全点，
         *   也正好是 flush 的唯一收口 —— 两者天然同一个位置。 */
        if (c->output_over_limit) {
            fprintf(stderr, "[fd=%d] 输出缓冲超限，断开连接（对端长时间不读回复）\n",
                    c->fd);
            freeClient(el, c);
            c = next;
            continue;
        }

        /* ★★ 只对"本轮有新数据待发"的客户端执行 write，否则会空转。
         *
         *   为什么这条判断是必须的（本次混合负载测试挖出的真实问题）：
         *   outbuf 里堆着几十 MB、对端又不读时，write() 每次都返回 EAGAIN。
         *   如果 beforeSleep 每轮都去试一次，而事件循环又因为
         *   EPOLLOUT 一直被唤醒 → 形成"醒来→写→EAGAIN→注册 EPOLLOUT→
         *   立刻又醒"的紧循环。实测服务端 CPU 打到 86%，
         *   而正常业务的 PING 在这期间被拖到 844ms。
         *   ★ 这正是 Redis 里 client->reply_bytes / 写入策略要解决的问题：
         *     必须用"有新数据"这个事件来驱动发送，而不是"每次都试一下"。
         *
         *   outbuf_pos < sdslen(outbuf) 表示"还有没发完的数据"，
         *   但光有这个条件还不够 —— 数据没发完时，发送时机应该是
         *   【内核可写通知（EPOLLOUT）】，而不是每一轮 beforeSleep。
         *   所以这里只在"本轮产生了新数据"时才尝试：
         *   由 send_response 置位 c->reply_pending。 */
        if (c->reply_pending) {
            c->reply_pending = 0;
            if (writeToClientRaw(el, c) == -1) {
                freeClient(el, c);    /* 写错误 → 关闭（c 已摘出链表） */
                c = next;
                continue;
            }
        }
        c = next;
    }
}

/* ============================================================
 * serverCron —— 周期性任务的唯一入口
 * ============================================================
 * 由 ae 层每 1000/hz 毫秒回调一次（本项目 hz=10 → 每 100ms）。
 *
 * ★ 返回值是「下次什么时候再来」（毫秒），不是「间隔」——
 *   ae 会把它换算成新的绝对到期时刻。
 *   ★ 每次返回【动态计算】而不是固定值：这样即使某一轮跑得慢了，
 *     也不会把延迟累积下去（每次都是「从现在起 100ms 后」）。
 *
 * ★ 本函数当前做一件事：清理空闲超时的客户端。
 *   以后可以在这里加：渐进式 rehash 的推进、内存统计、日志轮转等。
 *   ★ 关键是【所有周期工作共用一个定时器】—— 定时器链表恒定只有
 *     1~2 个节点，ae 的 O(n) 扫描才成立。
 */
int serverCron(aeEventLoop *el, long long id, void *clientData) {
    (void)id; (void)clientData;
    g_server.cronloops++;

    /* ---------- 客户端空闲超时检查 ---------- */
    if (g_server.maxidletime > 0 && g_server.numclients > 0) {
        long long now = aeGetTimeMilliseconds();
        long long limit_ms = (long long)g_server.maxidletime * 1000;

        /* ★ 分批（每轮只检查一部分），而不是一次性扫完所有客户端。
         *
         *   为什么：10000 个客户端时，一轮扫完要 10000 次比较 ——
         *   虽然单次很便宜，但它会让【这一轮】的延迟变大，
         *   而 serverCron 每 100ms 就要跑一次。
         *
         *   iterations = numclients / hz 是 Redis 的算法：
         *     hz=10（每 100ms 一轮）→ 每轮检查 1/10 的客户端
         *     → 理论上每个客户端 1 秒内至少被检查一次
         *   ★ 这就把「一次大停顿」摊成了「每次小停顿」——
         *     和渐进式 rehash 是同一个思想（把 O(n) 摊成 O(1)/轮）。
         *
         *   ★ 分批的做法：只检查「空闲时长落在本片区间」的客户端，
         *     分片号每轮轮转。于是每个客户端仍能在大约 1 秒内被检查一次，
         *     而轮转状态只是一个整数（cronloops % slices）—— 不持有任何
         *     对象地址。
         *
         *   ⚠️ 这里【绝不能用】「保存上次扫描到的 Client* 当游标」。
         *     那个节点随时可能在两次 serverCron 之间被 freeClient 释放
         *     （对端断开、协议错误…），游标就成了悬垂指针 ——
         *     下一轮解引用即 heap-use-after-free。
         *     ★ 这不是假设：本项目最初就是这么写的，被 ASan 抓到
         *       serverCron 读取已释放的 Client（见 git 历史里的阶段4修复）。
         *     教训：分批游标要存【可重算的标量】（下标/时间），
         *           不要存【对象地址】—— 地址的生命周期不受你控制。 */
        int slices = (g_server.hz > 0) ? g_server.hz : 1;
        int slice  = (int)(g_server.cronloops % slices);
        long long slice_ms = limit_ms / slices;
        if (slice_ms < 1) slice_ms = 1;

        /* ★ 遍历只用 c / next 两个局部指针 —— c 一定是本轮链表里的活节点，
         *   所以 c->next 永远安全（前面每次 freeClient 前都先存了 next）。 */
        Client *c = g_server.clients;
        while (c) {
            Client *next = c->next;          /* ★ 先存：c 可能被踢掉 */
            long long idle = now - c->lastinteraction;

            /* 本片区间 [slice*slice_ms, (slice+1)*slice_ms)。
             * ★ 最后一片【不设上界】—— 否则空闲远超 limit_ms 的客户端
             *   会落在所有分片之外，永远不被检查、永远踢不掉。 */
            int in_slice = (slice == slices - 1)
                         ? (idle >= slice_ms * slice)
                         : (idle >= slice_ms * slice && idle < slice_ms * (slice + 1));

            if (in_slice && idle > limit_ms) {
                printf("[fd=%d] 空闲 %lld 秒，超过上限 %d 秒，断开\n",
                       c->fd, idle / 1000, g_server.maxidletime);
                freeClient(el, c);
            }
            c = next;
        }
    }

    /* 下次还是 1000/hz 毫秒之后（动态计算，避免误差累积） */
    return 1000 / (g_server.hz > 0 ? g_server.hz : 10);
}
