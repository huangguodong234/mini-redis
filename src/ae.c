/* ============================================================
 * ae.c —— 事件循环骨架
 * ============================================================
 *
 * 移植自 Redis 的 ae.c（unstable 分支），按本项目的注释风格重写，
 * 并做了两处精简：
 *   1. 去掉 AE_BARRIER —— 本项目不需要「先落盘再回响应」的语义。
 *   2. 定时器做最小化实现（无序双向链表 + O(n) 扫描，同 Redis）——
 *      见 include/ae.h 里 aeTimeEvent 的说明。
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
    eventLoop->timeEventHead = NULL;   /* ★ 没有定时器 → epoll_wait 无限等 */
    eventLoop->timeEventNextId = 0;

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

/* 【用途】销毁事件循环：释放后端状态、两个数组、以及全部定时器节点。 */
void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    if (!eventLoop) return;
    aeApiFree(eventLoop);

    /* ★ 必须释放定时器链表 —— 否则 aeCreateTimeEvent 分配的节点全泄漏。
     *   （实测点：ASan 的 LeakSanitizer 会直接报出来。） */
    aeTimeEvent *te = eventLoop->timeEventHead;
    while (te) {
        aeTimeEvent *next = te->next;
        free(te);
        te = next;
    }

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
 * 时间事件（定时器）
 * ============================================================
 * ★ 与文件事件的分工：
 *   文件事件 —— 内核告诉我们「fd 就绪了」，我们被动响应
 *   时间事件 —— 我们自己算「什么时候该做什么」，主动安排
 *
 * ★ 两者如何结合（这是理解定时器的关键）：
 *   epoll_wait 的 timeout 参数由【最近的定时器】算出来。
 *   于是「睡到定时器到期」这一个动作同时实现了两件事：
 *     · 等文件事件（有事件就提前醒）
 *     · 等定时器（没事件就睡到点）
 *   ★ 不需要单独的线程或 alarm 信号 —— 定时器和 IO 多路复用共用一个等待。
 */

/* 【用途】当前时间（毫秒，单调来源）。定时器的所有时刻都用它。 */
long long aeGetTimeMilliseconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* 【用途】找出「最快到期的定时器」。返回 NULL 表示一个定时器都没有。
 * ★ O(n) 扫描 —— 本项目定时器只有 1~2 个，扫描成本可忽略（同 Redis）。
 *   如果定时器数量会随连接数增长，这里必须换成最小堆。 */
static aeTimeEvent *aeSearchNearestTimer(aeEventLoop *eventLoop) {
    aeTimeEvent *te = eventLoop->timeEventHead;
    aeTimeEvent *nearest = NULL;

    while (te) {
        if (!nearest || te->when < nearest->when) nearest = te;
        te = te->next;
    }
    return nearest;
}

/* 【用途】注册一个定时器：再过 milliseconds 毫秒首次执行。
 *   返回定时器 id（>0），失败返回 AE_ERR。
 * ★ 节点插在链表头 —— 头插法 O(1)。因为链表无序，插哪都一样。 */
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
                            aeTimeProc *proc, void *clientData) {
    if (!eventLoop || !proc || milliseconds < 0) return AE_ERR;

    aeTimeEvent *te = malloc(sizeof(*te));
    if (!te) return AE_ERR;

    eventLoop->timeEventNextId++;
    te->id = eventLoop->timeEventNextId;
    te->when = aeGetTimeMilliseconds() + milliseconds;
    te->timeProc = proc;
    te->clientData = clientData;

    te->prev = NULL;
    te->next = eventLoop->timeEventHead;
    if (te->next) te->next->prev = te;
    eventLoop->timeEventHead = te;

    return te->id;
}

/* 【用途】按 id 删除定时器。返回 AE_OK，没找到返回 AE_ERR。
 * ★ 双向链表让摘除是 O(1)（找到节点之后）—— 但找节点本身仍是 O(n)，
 *   因为链表无序、没有按 id 的索引。定时器数量少，无所谓。
 * ⚠ 绝不能在 timeProc 回调里删除【自己】—— 回调返回后 processTimeEvents
 *   还会访问这个节点（改 when / 或 free）。要结束自己就返回 AE_NOMORE。 */
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id) {
    aeTimeEvent *te = eventLoop->timeEventHead;

    while (te) {
        if (te->id == id) {
            if (te->prev) te->prev->next = te->next;
            else          eventLoop->timeEventHead = te->next;
            if (te->next) te->next->prev = te->prev;
            free(te);
            return AE_OK;
        }
        te = te->next;
    }
    return AE_ERR;
}

/* 【用途】当前定时器数量（测试/调试用）。 */
int aeGetTimeEventsCount(aeEventLoop *eventLoop) {
    int n = 0;
    for (aeTimeEvent *te = eventLoop->timeEventHead; te; te = te->next) n++;
    return n;
}

/* 【用途】执行所有【已到期】的定时器，返回执行次数。
 * ★ 三个必须注意的点：
 *   ① 先存 next 再处理：回调里可能删除别的定时器（free 掉节点），
 *      那样 te->next 就是 use-after-free。
 *   ② 只处理 when <= now 的：没到的直接跳过（链表无序，必须扫全部）。
 *   ③ 回调返回值决定后续：
 *        > 0      → 改 when 为新的一次到期时刻，留在链表里（周期性定时器）
 *        AE_NOMORE → free 掉节点，从链表摘除（一次性定时器）
 *
 * ⚠ 为什么「先存 next」就够：回调里可以删【别人】，但不能删【自己】
 *   （那样 te 就悬空了，下面的 te->when / free(te) 会崩）。
 *   Redis 也是这个约定。
 */
static int processTimeEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    aeTimeEvent *te;
    long long now = aeGetTimeMilliseconds();

    te = eventLoop->timeEventHead;
    while (te) {
        aeTimeEvent *next = te->next;      /* ★ ① 先存：回调可能删掉别的节点 */

        if (te->when <= now) {             /* ★ ② 到期的才执行 */
            int retval = te->timeProc(eventLoop, te->id, te->clientData);
            processed++;

            if (retval != AE_NOMORE) {
                /* 周期性：改成下一次到期时刻（相对现在） */
                te->when = now + retval;
            } else {
                /* 一次性：摘除并释放。
                 * ★ 这里不能用 aeDeleteTimeEvent(id) —— 它是 O(n) 重新查找，
                 *   而我们手上就有节点，直接 O(1) 摘掉。 */
                if (te->prev) te->prev->next = te->next;
                else          eventLoop->timeEventHead = te->next;
                if (te->next) te->next->prev = te->prev;
                free(te);
            }
        }
        te = next;
    }
    return processed;
}

/* ============================================================
 * 核心：一轮事件处理
 * ============================================================
 *
 * 顺序（每一步的位置都有理由）：
 *   ① beforesleep   —— 睡【之前】收尾（本项目用它 flush 各客户端 outbuf）
 *   ② 算 timeout    —— 决定最多睡多久（★ 由最近的定时器决定）
 *   ③ aeApiPoll     —— ★ 唯一真正睡眠的地方（epoll_wait）
 *   ④ aftersleep    —— 睡【之后】，处理「睡眠期间世界变了」
 *   ⑤ 遍历 fired[]  —— 分发文件事件回调
 *   ⑥ 处理定时器    —— ★ 文件事件全部处理完之后
 *
 * ★ 为什么定时器放最后（不能放最前）：
 *   ① 文件事件是「外部真实发生的」，定时器是「我们自己安排的」——
 *      先服务外部请求，避免定时器把 IO 饿死。
 *   ② 定时器回调里可能关连接/改事件（比如踢掉空闲客户端），
 *      此时文件事件已经处理完，不会和 fired[] 里的旧快照冲突。
 *   ③ 如果反过来，一个 100ms 的定时器会把本轮的 IO 推迟到它之后处理 ——
 *      在高负载下这会累积成明显的延迟。
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
      // maxfd == -1 → 一个 fd 都没注册 → 就算 epoll_wait 也没意义
      //但如果【有定时器要处理，且不是 DONT_WAIT】→ 仍然要进块内（因为要睡到定时器到期）
      
    {
        struct timeval tv, *tvp = NULL;   /* NULL = 无限等待 */

        if (eventLoop->beforesleep != NULL && (flags & AE_CALL_BEFORE_SLEEP))
            eventLoop->beforesleep(eventLoop);

        /* ★ 必须在 beforesleep 之后判断 —— 它内部可能改了 eventLoop->flags。
         *   参数 flags 优先级最高（调用点的意图不能被钩子推翻）。 */
        if ((flags & AE_DONT_WAIT) || (eventLoop->flags & AE_DONT_WAIT)) {
            tv.tv_sec = tv.tv_usec = 0;
            tvp = &tv;
        } else if (flags & AE_TIME_EVENTS) {
            /* ★ 算超时 = 距最近定时器还有多久。
             *   这一步是「定时器」和「IO 多路复用」的结合点：
             *   把 timeout 设成到最近定时器的距离，epoll_wait 就会
             *   在有事件时立刻醒、没事件时睡到定时器该跑的时刻。
             *   ★ 不需要额外线程、不需要 alarm 信号。 */
            aeTimeEvent *shortest = aeSearchNearestTimer(eventLoop);
            if (shortest) {
                long long now = aeGetTimeMilliseconds();
                long long diff = shortest->when - now;
                if (diff <= 0) diff = 0;   /* 已到期 → 不睡，立刻返回去处理它 */
                tv.tv_sec = diff / 1000;
                tv.tv_usec = (diff % 1000) * 1000;
                tvp = &tv;
            }
            /* shortest == NULL（没有定时器）→ tvp 保持 NULL = 无限等待。
             * ★ 这正是阶段 3 之前的行为，向后兼容。 */
        }

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

    /* ★ ⑥ 文件事件处理完之后才处理定时器。
     *   理由见函数头注释：先服务外部真实请求，避免定时器把 IO 饿死。 */
    if (flags & AE_TIME_EVENTS) {
        processed += processTimeEvents(eventLoop);
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
