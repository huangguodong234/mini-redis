/* ============================================================
 * ae.c —— 事件循环骨架
 * ============================================================
 *
 * 移植自 Redis 的 ae.c（unstable 分支），按本项目的注释风格重写，
 * 并做了两处精简：
 *   1. 去掉时间事件（定时器）—— 留到改造阶段 4。本阶段只有文件事件。
 *   2. 去掉 AE_BARRIER —— 本项目不需要「先落盘再回响应」的语义。
 *
 * ★ 保留的是架构本身的形状：分发表驱动 + before/after sleep 钩子 +
 *   void* 隔离后端。这样以后和 Redis 源码能一一对上。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>

#include "ae.h"

/* ★ 编译期多态：本项目固定 Linux/epoll。
 *   Redis 在这里是 #ifdef HAVE_EVPORT / HAVE_EPOLL / HAVE_KQUEUE / select
 *   的四层嵌套 —— 那些宏由构建系统探测后定义，是为了跨平台。
 *   我们只跑 Linux，所以直接包含，不引入平台探测的复杂度。 */
#include "ae_epoll.c"

/* 初始只分配这么多个槽位，之后按 fd 增长惰性扩张。
 * ★ 抄的是 Redis unstable 的做法：把「配置上限 setsize」和
 *   「实际已分配 nevents」解耦 —— 配置 maxclients=100万 时不用一上来
 *   就分配 100万 × 32 B = 32 MB。 */
#define AE_INITIAL_ALLOC 1024

/* 【用途】创建事件循环：分配 events/fired 数组 + 后端状态。setsize 是容量上限（如 maxclients）。 */
aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    int i;

    if (setsize <= 0) return NULL;

    if ((eventLoop = malloc(sizeof(*eventLoop))) == NULL) return NULL;

    eventLoop->nevents = (setsize < AE_INITIAL_ALLOC) ? setsize : AE_INITIAL_ALLOC;
    eventLoop->events = malloc(sizeof(aeFileEvent) * (size_t)eventLoop->nevents);
    eventLoop->fired = malloc(sizeof(aeFiredEvent) * (size_t)eventLoop->nevents);
    if (eventLoop->events == NULL || eventLoop->fired == NULL) {
        free(eventLoop->events);
        free(eventLoop->fired);
        free(eventLoop);
        return NULL;
    }

    eventLoop->setsize = setsize;
    eventLoop->maxfd = -1;
    eventLoop->stop = 0;
    eventLoop->flags = 0;
    eventLoop->beforesleep = NULL;
    eventLoop->aftersleep = NULL;
    eventLoop->apidata = NULL;

    if (aeApiCreate(eventLoop) == -1) {
        free(eventLoop->events);
        free(eventLoop->fired);
        free(eventLoop);
        return NULL;
    }

    /* mask == AE_NONE 表示「这个槽位空着」 */
    for (i = 0; i < eventLoop->nevents; i++)
        eventLoop->events[i].mask = AE_NONE;

    return eventLoop;
}

/* 【用途】销毁事件循环：释放后端状态和两个数组。 */
void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    if (!eventLoop) return;
    aeApiFree(eventLoop);
    free(eventLoop->events);
    free(eventLoop->fired);
    free(eventLoop);
}

/* 【用途】让 aeMain 的循环退出（信号处理函数里调，等价于「叫停」）。 */
void aeStop(aeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}

/* 【用途】读取容量上限。 */
int aeGetSetSize(aeEventLoop *eventLoop) {
    return eventLoop->setsize;
}

/* 【用途】调整容量上限（如 maxclients 改了）。
 * ★ 只负责「上限」和「收缩」；扩张交给 aeCreateFileEvent 惰性做。
 *   两处都能改 nevents，所以要同时看这两个函数才能确认正确性。 */
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize) {
    if (setsize == eventLoop->setsize) return AE_OK;
    if (eventLoop->maxfd >= setsize) return AE_ERR;   /* 已有更大 fd，不能缩小 */
    if (aeApiResize(eventLoop, setsize) == -1) return AE_ERR;

    eventLoop->setsize = setsize;

    /* 只有缩小时才需要真的 realloc（放大等 aeCreateFileEvent 惰性做） */
    if (setsize < eventLoop->nevents) {
        aeFileEvent *ne = realloc(eventLoop->events, sizeof(aeFileEvent) * (size_t)setsize);
        aeFiredEvent *nf = realloc(eventLoop->fired, sizeof(aeFiredEvent) * (size_t)setsize);
        if (ne) eventLoop->events = ne;
        if (nf) eventLoop->fired = nf;
        eventLoop->nevents = setsize;
    }
    return AE_OK;
}

/* 【用途】开关全局「不睡」标志（本次 aeProcessEvents 立即返回）。 */
void aeSetDontWait(aeEventLoop *eventLoop, int noWait) {
    if (noWait) eventLoop->flags |= AE_DONT_WAIT;
    else        eventLoop->flags &= ~AE_DONT_WAIT;
}

/* 【用途】注册事件：让事件循环在这个 fd 上关注 mask，就绪时调 proc。
 *         clientData 原样保存、回调时还给上层（本项目是 Client*）。
 *         内部会按需惰性扩容 events/fired 数组。 */
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
                      aeFileProc *proc, void *clientData)
{
    /* ★ 越界防护（不是「防溢出」）：fd 是内核给的编号，可能超过容量。
     *   少了这一行，events[fd] 会写到数组外面 —— 内存踩踏。 */
    if (fd < 0 || fd >= eventLoop->setsize) {
        errno = ERANGE;
        return AE_ERR;
    }

    /* ★ 惰性增长：fd 超出已分配槽位时，按 2 倍（或刚好够 fd+1）扩张。
     *   realloc 可能搬移整块内存 —— 这就是为什么 epoll_event.data 里
     *   必须存 fd（整数）而不是 &events[fd]（指针会失效）。 */
    if (fd >= eventLoop->nevents) {
        int newnevents = eventLoop->nevents;
        newnevents = (newnevents * 2 > fd + 1) ? newnevents * 2 : fd + 1;
        if (newnevents > eventLoop->setsize) newnevents = eventLoop->setsize;

        aeFileEvent *ne = realloc(eventLoop->events, sizeof(aeFileEvent) * (size_t)newnevents);
        aeFiredEvent *nf = realloc(eventLoop->fired, sizeof(aeFiredEvent) * (size_t)newnevents);
        /* realloc 失败时不改 nevents，保持旧数组可用（下次再试） */
        if (ne) eventLoop->events = ne;
        if (nf) eventLoop->fired = nf;
        if (!ne || !nf) return AE_ERR;

        /* 新槽位初始化为 AE_NONE */
        for (int i = eventLoop->nevents; i < newnevents; i++)
            eventLoop->events[i].mask = AE_NONE;

        eventLoop->nevents = newnevents;
    }

    aeFileEvent *fe = &eventLoop->events[fd];

    /* 先告知内核，成功了再更新用户态那份（保证两者不会不一致） */
    if (aeApiAddEvent(eventLoop, fd, mask) == -1)
        return AE_ERR;

    fe->mask |= mask;
    if (mask & AE_READABLE) fe->rfileProc = proc;
    if (mask & AE_WRITABLE) fe->wfileProc = proc;
    fe->clientData = clientData;

    if (fd > eventLoop->maxfd)
        eventLoop->maxfd = fd;
    return AE_OK;
}

/* 【用途】取消关注：删掉这个 fd 上的 mask 事件（★ 必须在 close 之前调）。
 *         若删到 mask 归零，会一并清掉 clientData/回调 —— 防止 fd 被复用后
 *         分发循环按旧指针访问已释放的 Client（use-after-free）。 */
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask) {
    if (fd < 0 || fd >= eventLoop->setsize) return;
    aeFileEvent *fe = &eventLoop->events[fd];
    if (fe->mask == AE_NONE) return;

    aeApiDelEvent(eventLoop, fd, mask);
    fe->mask = fe->mask & (~mask);

    /* 把 clientData 也清掉 —— 防止 fd 被 close 后复用给新连接时，
     * 分发循环按旧指针访问已释放的 Client（use-after-free）。
     * ★ 这就是「关闭连接四步」里最后一步「清账」的内容。 */
    if (fe->mask == AE_NONE) {
        fe->clientData = NULL;
        fe->rfileProc = NULL;
        fe->wfileProc = NULL;
    }

    /* maxfd 可能要回退（它是「最大的活跃 fd」） */
    if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
        int j;
        for (j = eventLoop->maxfd - 1; j >= 0; j--)
            if (eventLoop->events[j].mask != AE_NONE) break;
        eventLoop->maxfd = j;
    }
}

/* 【用途】查询某个 fd 当前登记的关注掩码（AE_NONE 表示没登记）。 */
int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
    if (fd < 0 || fd >= eventLoop->setsize) return 0;
    return eventLoop->events[fd].mask;
}

/* 【用途】查询某个 fd 注册时挂的 clientData（本项目是 Client*）。 */
void *aeGetFileClientData(aeEventLoop *eventLoop, int fd) {
    if (fd < 0 || fd >= eventLoop->setsize) return NULL;
    aeFileEvent *fe = &eventLoop->events[fd];
    if (fe->mask == AE_NONE) return NULL;
    return fe->clientData;
}

/* ============================================================
 * 核心：一轮事件处理
 * ============================================================
 *
 * 顺序（每一步的位置都有理由）：
 *   ① beforesleep   —— 睡【之前】收尾（本项目用它 flush 各客户端 outbuf）
 *   ② 算 timeout    —— 决定最多睡多久
 *   ③ aeApiPoll     —— ★ 唯一真正睡眠的地方（epoll_wait）
 *   ④ aftersleep    —— 睡【之后】，处理「睡眠期间世界变了」
 *   ⑤ 遍历 fired[]  —— 分发回调
 *
 * ★ 为什么 flush 放 beforesleep 而不是「命令执行完立刻写」：
 *   ① 攒批：一条 +OK 从 3 次 write 降到 1 次
 *   ② 谁要关闭已确定 → 不会白写
 *   ③ 写完即可撤掉 EPOLLOUT → epoll_wait 才能睡好（EPOLLOUT 几乎总是就绪，
 *      留着它 epoll_wait 会立刻返回 → 忙转）
 */
int aeProcessEvents(aeEventLoop *eventLoop, int flags) {
    int processed = 0, numevents;

    /* 什么都没允许处理 → 直接返回 */
    if (!(flags & AE_FILE_EVENTS) && !(flags & AE_TIME_EVENTS)) return 0;

    if (eventLoop->maxfd != -1 ||
        ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT)))
    {
        struct timeval tv, *tvp = NULL;   /* NULL = 无限等待 */

        if (eventLoop->beforesleep != NULL && (flags & AE_CALL_BEFORE_SLEEP))
            eventLoop->beforesleep(eventLoop);

        /* ★ 必须在 beforesleep 之后判断 —— 它内部可能改了 eventLoop->flags。
         *   参数 flags 优先级最高（调用点的意图不能被钩子推翻）。 */
        if ((flags & AE_DONT_WAIT) || (eventLoop->flags & AE_DONT_WAIT)) {
            tv.tv_sec = tv.tv_usec = 0;
            tvp = &tv;
        }
        /* 本项目暂无定时器：没有 DONT_WAIT 时就无限等待
         * （阶段 4 加定时器后，这里改成「睡到最近一个定时器到期」） */

        numevents = aeApiPoll(eventLoop, tvp);
        if (numevents < 0) return processed;   /* epoll 出错，交给上层决定 */

        if (eventLoop->aftersleep != NULL && (flags & AE_CALL_AFTER_SLEEP))
            eventLoop->aftersleep(eventLoop);

        /* 没要求处理文件事件 → 睡了但不消费（用于「只想推进时间」的场景） */
        if (!(flags & AE_FILE_EVENTS)) numevents = 0;

        for (int j = 0; j < numevents; j++) {
            int fd = eventLoop->fired[j].fd;
            aeFileEvent *fe = &eventLoop->events[fd];
            int mask = eventLoop->fired[j].mask;
            int fired = 0;

            /* ★ 双重与：fe->mask 是「我登记关心什么」，mask 是「内核说发生了什么」。
             *   为什么必须重新检查 fe->mask：fired[] 是 epoll_wait 那一刻的【快照】，
             *   而本轮回调可能已经把某个 fd 关掉了（其 mask 被清成 AE_NONE）——
             *   轮到它时这个判断会自动跳过，避免对已关闭的 fd 调回调。 */
            if (fe->mask & mask & AE_READABLE) {
                fe->rfileProc(eventLoop, fd, fe->clientData, mask);
                fired++;
                /* 回调可能改了 events（甚至 realloc），重新取指针 */
                fe = &eventLoop->events[fd];
            }

            if (fe->mask & mask & AE_WRITABLE) {
                /* 同一函数同时注册了读和写时只调一次，避免重复工作 */
                if (!fired || fe->wfileProc != fe->rfileProc) {
                    fe->wfileProc(eventLoop, fd, fe->clientData, mask);
                    fired++;
                }
            }

            processed++;
        }
    }

    return processed;
}

/* 【用途】主循环：反复 aeProcessEvents 直到 stop 被置位。 */
void aeMain(aeEventLoop *eventLoop) {
    eventLoop->stop = 0;
    while (!eventLoop->stop) {
        aeProcessEvents(eventLoop,
                        AE_ALL_EVENTS | AE_CALL_BEFORE_SLEEP | AE_CALL_AFTER_SLEEP);
    }
}

/* 【用途】注册「睡之前」的钩子（本项目用它 flush 各客户端 outbuf）。 */
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}

/* 【用途】注册「睡之后」的钩子（处理睡眠期间世界变了，可传 NULL）。 */
void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep) {
    eventLoop->aftersleep = aftersleep;
}

/* 【用途】返回后端名（"epoll"），供日志用。 */
char *aeGetApiName(void) {
    return aeApiName();
}
