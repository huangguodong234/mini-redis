/* ============================================================
 * ae_epoll.c —— ae 的 Linux epoll 后端
 * ============================================================
 *
 * ★ 这个文件是 ae 层里唯一直接碰 epoll_* 系统调用的地方。
 *   ae.c 只调用下面这几个 aeApi* 函数，完全不知道 epoll 的存在。
 *
 * ★ 为什么用 #include 而不是函数指针表：
 *   后端集合（epoll/kqueue/select）在【编译时】就确定了，不需要运行期选择。
 *   #include 换实现 → 零间接调用开销。这和你 dict 用 dictType 函数指针表
 *   （运行期换策略）是两种多态，选择依据是「实现集合何时确定」。
 */

#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ae.h"

/* 后端私有数据 —— 通过 eventLoop->apidata 挂上去。
 * aeEventLoop 是公共类型（必须对所有后端通用），不能含 epfd，
 * 所以用 void* 留一个洞。 */
typedef struct aeApiState {
    int epfd;                      /* epoll 实例的 fd */
    struct epoll_event *events;     /* epoll_wait 的输出缓冲 */
} aeApiState;

/* 【用途】创建后端状态：建 epoll 实例 + 分配 epoll_wait 的输出缓冲，
 *         挂到 eventLoop->apidata 上。aeCreateEventLoop 时调一次。 */
static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = malloc(sizeof(aeApiState));
    if (!state) return -1;

    /* ★ 这份缓冲区按 setsize 分配（不是 nevents）。
     *   理由：epoll_wait 的 maxevents 必须 <= 数组容量，否则内核写越界；
     *   而「本轮就绪数」最坏可以接近全部登记数，所以必须按上限分配。
     *   这是「用内存换正确性」—— 和 ae.c 里 events[] 的惰性增长不同策略。 */
    state->events = malloc(sizeof(struct epoll_event) * (size_t)eventLoop->setsize);
    if (!state->events) {
        free(state);
        return -1;
    }
    /* 1024 只是给内核的提示值，现代内核忽略它 */
    state->epfd = epoll_create(1024);
    if (state->epfd == -1) {
        free(state->events);
        free(state);
        return -1;
    }
    eventLoop->apidata = state;
    return 0;
}

/* 【用途】按新的 setsize 扩容 epoll_wait 的输出缓冲。aeResizeSetSize 时调。 */
static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event *ne = realloc(state->events,
                                    sizeof(struct epoll_event) * (size_t)setsize);
    if (!ne) return -1;              /* 失败时保留旧指针，不泄漏 */
    state->events = ne;
    return 0;
}

/* 【用途】销毁后端：关 epoll 实例 + 释放输出缓冲 + 释放私有结构。
 *         aeDeleteEventLoop 时调。 */
static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;
    if (!state) return;
    close(state->epfd);
    free(state->events);
    free(state);
}

/* 把 ae 的 mask 翻译成 epoll 的 events 位 */
static uint32_t aeMaskToEpoll(int mask) {
    uint32_t ee = 0;
    if (mask & AE_READABLE) ee |= EPOLLIN;
    if (mask & AE_WRITABLE) ee |= EPOLLOUT;
    return ee;
}

/* 【用途】登记/修改关注事件：告诉内核「这个 fd 我要关注什么」。
 *         aeCreateFileEvent 时调。
 *         难点：epoll 没有「我登记过什么」的查询接口，只能靠自己记的
 *         events[fd].mask 判断该用 ADD 还是 MOD。 */
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee;
    memset(&ee, 0, sizeof(ee));

    /* ★ epoll 不提供「我登记过哪些 fd」的查询接口，所以只能看自己那份
     *   影子掩码（events[fd].mask）来判断：
     *     mask == AE_NONE → 从没登记过（或已全删）→ ADD
     *     否则                                    → MOD（重复 ADD 会 EEXIST） */
    int op = (eventLoop->events[fd].mask == AE_NONE) ? EPOLL_CTL_ADD
                                                     : EPOLL_CTL_MOD;

    /* ★ 关键：epoll_ctl 是【整体设定】不是增量。
     *   要把「原来关心的」和「这次要加的」合并后一起下发，
     *   否则只写 EPOLLOUT 会把 EPOLLIN 丢掉 → 该 fd 静默收不到命令。 */
    mask |= eventLoop->events[fd].mask;

    ee.events = aeMaskToEpoll(mask);
    ee.data.fd = fd;   /* ★ 存 fd（不是指针）—— 见下方说明 */
    if (epoll_ctl(state->epfd, op, fd, &ee) == -1) return -1;
    return 0;
}

/* 【用途】取消关注事件（部分或全部）。aeDeleteFileEvent 时调。
 *         还有剩余关注 → MOD 下发剩下的；全删了 → DEL 移除该 fd。 */
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee;
    memset(&ee, 0, sizeof(ee));

    int mask = eventLoop->events[fd].mask & (~delmask);

    ee.events = aeMaskToEpoll(mask);
    ee.data.fd = fd;
    if (mask != AE_NONE) {
        epoll_ctl(state->epfd, EPOLL_CTL_MOD, fd, &ee);
    } else {
        /* 全删了 → DEL。注意：内核 < 2.6.9 要求这里传非空指针，
         * 所以即使不用 ee 的内容也照传。 */
        epoll_ctl(state->epfd, EPOLL_CTL_DEL, fd, &ee);
    }
}

/* 【用途】等待事件 + 翻译结果。★ 整个程序唯一真正睡眠的地方。
 *         · 调 epoll_wait 睡到有 fd 就绪（或超时）
 *         · 把内核的 EPOLLIN/EPOLLOUT 翻译成 ae 的 AE_READABLE/AE_WRITABLE，
 *           填进 fired[]（这就是「内核格式 → ae 格式」的转换点）
 *         返回值 = 就绪 fd 个数（0 = 超时或 EINTR，-1 = 真出错）。 */
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int numevents = 0;

    /* 超时换算：毫秒。★ +999 是向上取整 —— 1.001ms 要变成 2ms 而不是 1ms，
     * 否则会提前醒来空转一轮。tvp == NULL 表示无限等待（-1）。 */
    int timeout = tvp ? (int)(tvp->tv_sec * 1000 + (tvp->tv_usec + 999) / 1000) : -1;

    int retval = epoll_wait(state->epfd, state->events, eventLoop->setsize, timeout);
    if (retval > 0) {
        numevents = retval;
        for (int j = 0; j < numevents; j++) {
            int mask = 0;
            struct epoll_event *e = state->events + j;

            if (e->events & EPOLLIN) mask |= AE_READABLE;
            if (e->events & EPOLLOUT) mask |= AE_WRITABLE;
            /* ★ EPOLLERR / EPOLLHUP 翻译成「可读且可写」。
             *   内核会自动报告这两个（无需登记），但 ae 的位掩码里没有
             *   「错误」这一位。翻译成可读的后果：上层回调去 read()，
             *   拿到 -1 或 0 → 正常走「客户端断开」的清理路径。
             *   用一次多余的系统调用，换掉在事件层引入「错误位」的复杂度。 */
            if (e->events & EPOLLERR) mask |= AE_READABLE | AE_WRITABLE;
            if (e->events & EPOLLHUP) mask |= AE_READABLE | AE_WRITABLE;

            eventLoop->fired[j].fd = e->data.fd;   /* ★ 取回注册时的 fd */
            eventLoop->fired[j].mask = mask;
        }
    } else if (retval == -1 && errno != EINTR) {
        /* EINTR 是信号打断，属于正常情况，故意不报错（上层会重新进循环） */
        fprintf(stderr, "aeApiPoll: epoll_wait 失败: %s\n", strerror(errno));
        return -1;
    }
    return numevents;
}

/* 【用途】返回后端名字，供日志/调试用（aeGetApiName 转发到这里）。 */
static char *aeApiName(void) {
    return "epoll";
}
