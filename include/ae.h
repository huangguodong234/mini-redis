#ifndef MINI_REDIS_AE_H
#define MINI_REDIS_AE_H

/* ============================================================
 * ae.h —— 事件循环（移植/精简自 Redis 的 ae 层，unstable 分支）
 * ============================================================
 *
 * ★ 设计要点（与 Redis 一致，详细推导见 docs/ae-design.md 待补）：
 *
 *   1. 把「操作系统怎么等事件」彻底隔离出去。ae.c 只调用
 *      aeApiCreate/AddEvent/DelEvent/Poll/... 这几个名字，具体实现由
 *      【编译期】决定（本项目只有 Linux → 固定 include ae_epoll.c）。
 *      上层代码零改动即可换后端 —— 这是「编译期多态」，
 *      代价为零（没有间接调用），因为后端集合在编译时就确定了。
 *
 *   2. events[] 是「fd → 事件处理器」的数组，★ 下标就是 fd。
 *      所以 events[fd] 是 O(1)（地址 = base + fd × sizeof(aeFileEvent)），
 *      不需要哈希、不需要遍历。这依赖「fd 是内核分配的小整数」这一
 *      OS 事实。maxclients 上限决定数组大小。
 *
 *   3. void *clientData 是留給上层的洞 —— ae 层不知道 Client 是什么，
 *      只存一个指针，回调时原样还给上层。和 void *apidata 同一个手法。
 *
 *   4. aeFileEvent.mask 是「已向内核登记的关注掩码」的用户态副本。
 *      必须和内核保持一致（epoll_ctl 是整体设定，不是增量），
 *      且用户态这份是权威。它决定三件事：
 *        · 改关注内容时该发 EPOLL_CTL_ADD 还是 EPOLL_CTL_MOD
 *        · 合并后的新掩码是什么（mask |= 旧掩码）
 *        · 分发时该不该调回调（fe->mask & fired_mask & AE_XXX）
 *
 * ★ 定时器（时间事件）—— 阶段 4 已加入（无序双向链表 + O(n) 扫描）。
 *   数据结构是【无序双向链表 + O(n) 扫描】，和 Redis 一致。
 *   为什么不用最小堆：本项目只有 1~2 个定时器，堆的维护成本反而更高。
 *   详见下方 aeTimeEvent 的说明。
 */

#define AE_OK 0
#define AE_ERR -1

/* 事件掩码 */
#define AE_NONE 0        /* 没有登记任何事件 */
#define AE_READABLE 1    /* 可读 */
#define AE_WRITABLE 2    /* 可写 */

/* aeProcessEvents 的 flags：★ 语义是「允许处理什么」，不是「要处理什么」 */
#define AE_FILE_EVENTS (1 << 0)
#define AE_TIME_EVENTS (1 << 1)
#define AE_ALL_EVENTS (AE_FILE_EVENTS | AE_TIME_EVENTS)
#define AE_DONT_WAIT (1 << 2)
#define AE_CALL_BEFORE_SLEEP (1 << 3)
#define AE_CALL_AFTER_SLEEP (1 << 4)

typedef struct aeEventLoop aeEventLoop;

/* 文件事件回调：clientData 就是注册时挂进去的那个指针（上层传 Client*）
 * mask 是「本轮实际发生了什么」（可能同时含 AE_READABLE|AE_WRITABLE） */
typedef void aeFileProc(aeEventLoop *eventLoop, int fd, void *clientData, int mask);

/* 睡眠前/后的钩子（控制反转：ae 留空位，上层填自己的逻辑） */
typedef void aeBeforeSleepProc(aeEventLoop *eventLoop);

/* ============================================================
 * 定时器（时间事件 / 时间事件）
 * ============================================================
 * ★ 与文件事件的根本不同：
 *     文件事件 = 内核告诉我们「什么时候发生了什么」
 *     时间事件 = 我们自己算出「什么时候该做什么」
 *   所以文件事件靠 epoll_wait 出参驱动，时间事件靠 epoll_wait 的
 *   【timeout 参数】驱动 —— 把超时设成「距最近定时器还有多久」，
 *   睡到那个点自然醒来处理。这就是定时器与事件循环的结合方式。
 *
 * ★ 数据结构为什么是无序双向链表 + O(n) 扫描（同 Redis）：
 *     · 本项目只有 1~2 个定时器（serverCron 是唯一入口，它内部
 *       分发很多子任务，而不是注册很多定时器）
 *     · 2 个元素下，最小堆的维护成本 > 扫一遍的成本
 *     · ★ 关键前提：定时器数量【不随连接数增长】。
 *       如果「每个客户端一个超时定时器」，那 10000 连接 = 10000 定时器，
 *       O(n) 扫描就不可接受了 —— 那种设计必须换结构。
 *       本项目用【轮询】解决客户端空闲超时（见 serverCron），
 *       所以定时器数量恒定，链表足够。
 *
 * ★ 回调返回值是「下次什么时候再执行」（毫秒）：
 *     · 返回 > 0 → 相对当前时间再过这么多毫秒再执行
 *     · 返回 AE_NOMORE (-1) → 只执行这一次，然后把这个定时器删掉
 *   ⚠ 这个约定来自 Redis。它让定时器自己决定「一次性」还是「周期性」，
 *     ae 层不必知道语义差异。
 */
#define AE_NOMORE -1

typedef int aeTimeProc(aeEventLoop *eventLoop, long long id, void *clientData);

/* 每个定时器一个这个结构体（链表节点 + 元信息） */
typedef struct aeTimeEvent {
    long long id;                  /* 定时器编号，全局自增，用于标注和删除 */
    long long when;                /* ★ 绝对到期时刻（毫秒，typeof(unixtime in ms)）。
                                    * 为什么存绝对时间而不是「间隔」：
                                    *   醒来时要判断「到期了没」= now >= when；
                                    *   存间隔的话每次都得记住「上次啥时候跑的」。 */
    aeTimeProc *timeProc;          /* 到期时调这个 */
    void *clientData;              /* 上层数据（ae 只存不解释，同文件事件的 clientData） */
    struct aeTimeEvent *prev;      /* ★ 无序双向链表 —— 双向是为了 O(1) 摘除
                                    *   （和 Client 链表同一个理由：删除时要找前驱） */
    struct aeTimeEvent *next;
} aeTimeEvent;

/* 每个 fd 的事件处理器 —— events[fd] 就是它
 * 实测（gcc -O2, x86-64）sizeof == 32：3 个指针(24) + int(4) + 4 字节 padding。
 * 字段顺序在这里【不影响】总大小（两种排法都是 32），所以按可读性排即可。 */
typedef struct aeFileEvent {
    aeFileProc *rfileProc;   /* 可读时调这个函数（NULL = 不关心读） */
    aeFileProc *wfileProc;   /* 可写时调这个函数（NULL = 不关心写） */
    void *clientData;        /* ★ 上层业务对象，ae 只存不解释、回调时原样还回去。
                              * 本项目挂的是 Client*。
                              * ★ 为什么用 void* 而不是 Client*：ae 是通用库，它
                              * 不知道 Client 是什么，也不该知道 —— 这就是「接口与
                              * 实现分离」。和 apidata 是同一个手法（那个是让 ae 不
                              * 认识 IO 后端，这个是让 ae 不认识业务类型）。
                              * ⚠ 生命周期风险：它是个指针，Client 被 free 后就成了
                              * 悬垂指针。所以 aeDeleteFileEvent 在 mask 归零时会把它
                              * 清成 NULL —— 防止 fd 被 close 后内核复用给新连接时，
                              * 分发循环按旧指针访问已释放的内存（use-after-free）。 */
  
    int mask;                /* 已向内核登记的关注掩码（AE_READABLE|AE_WRITABLE 的或）
                              * ★ 这是【用户态的副本】，必须和内核里那份保持一致，
                              *   而且用户态这份是权威。它决定三件事：
                              *     ① 改关注内容时该发 EPOLL_CTL_ADD 还是 MOD
                              *        （epoll 没有「我登记过什么」的查询接口）
                              *     ② 合并后的新掩码是什么（epoll_ctl 是整体设定
                              *        不是增量，必须 mask |= 旧掩码）
                              *     ③ 分发时该不该调回调
                              *        （fe->mask & fired_mask & AE_XXX）
                              * mask == AE_NONE 表示「这个 fd 没有登记任何事件」，
                              * 也是「这个槽位空着」的标记。 */
} aeFileEvent;

/* epoll_wait 的返回结果，逐项填进这个数组 */
typedef struct aeFiredEvent {
    int fd;
    int mask;
} aeFiredEvent;

struct aeEventLoop {
    /* ---- 容量与 fd 管理 ---- */
    int maxfd;               /* 当前登记的最大 fd（-1 表示一个都没有）。
                              * 用途：aeProcessEvents 判断「有没有 fd 要等」；
                              * 删除事件后需要回退（可能是倒数第二大的）。 */
  
    int setsize;             /* ★ 容量【上限】（配置项：min(maxclients, ulimit)）。
                              * 所有 api 都拿它做越界检查：fd >= setsize → AE_ERR。
                              * 注意：它【不】等于已分配大小（见 nevents）。 */
  
    int nevents;             /* ★ 已【分配】的槽位数（惰性增长，nevents <= setsize）。
                              * 初始 1024，fd 变大时按 2 倍扩张（见 aeCreateFileEvent）。
                              * 为什么要有它：把「配置上限」和「实际分配」解耦 ——
                              * maxclients 配 100 万时不必一上来就分配 32MB。 */
  
    aeFileEvent *events;     /* fd → 处理器 的数组，★ 下标就是 fd。
                              * events[fd] 是 O(1)（地址 = base + fd×sizeof），
                              * 不需要哈希、不需要遍历 —— 依赖「fd 是内核分配的
                              * 小整数」这一 OS 事实。mask == AE_NONE 的槽位是空的。 */
  
    aeFiredEvent *fired;     /* epoll_wait 本轮的输出：哪些 fd 就绪了。
                              * ★ 它是【快照】—— 循环处理期间可能有 fd 被关闭，
                              * 所以分发时要重新检查 fe->mask（见 aeProcessEvents）。
                              * 和 events 一样跟着 nevents 增长（两数组必须同步大小，
                              * 因为 epoll_wait 的 maxevents 用的是 setsize）。 */

    /* ---- 生命周期控制 ---- */
    int stop;                /* aeStop() 置 1 → aeMain 的 while(!stop) 退出。
                              * 为什么需要：aeMain 是死循环、睡在 epoll_wait 里，
                              * 没有自然出口。信号处理函数里调 aeStop() 就能让它退出，
                              * 不必依赖 read() 被 EINTR 打断这个不可靠的机制。
                              * ★ 与业务侧的 Keep_running 是两个独立的东西：
                              *   ae 是通用库，不该去读上层的全局变量（会耦合）。 */

    /* ---- 扩展点：让 ae 层不认识「具体后端」和「上层业务」 ---- */
    void *apidata;           /* ★ 后端私有数据的「挂载点」（类型不确定的指针）。
                              *
                              * 问题：aeApiPoll 要调 epoll_wait(state->epfd)，而 epfd 是
                              * aeApiCreate 创建的 —— 它必须存在活得够久的结构体里。
                              * 存哪？
                              *   ① 放进 aeEventLoop：则 kqueue 的 kq、select 的 fd_set
                              *      也要塞进来 → 公共类型变成「所有后端的杂物间」，
                              *      换后端就得改公共结构体，违背「ae.c 不用改就能换后                                          端」。
                              *   ② 用全局变量：则一个进程只能有一个事件循环（库不该有
                              *      这种隐藏限制），且测试之间会互相污染。
                              *   ③ ★ 留一个 void* 空位，后端自己挂私有结构 —— 就是它。
                              *
                              * 用法（三步，前后端各知道一半）：
                              *   创建：aeApiCreate 里 malloc 一个 aeApiState，
                              *         eventLoop->apidata = state;      ← 挂上去
                              *   使用：每个 aeApi* 函数开头
                              *         aeApiState *state = eventLoop->apidata;  ← 取回
                              *   销毁：aeApiFree 里 free(state)；
                              *
                              * ★ 为什么是一个而不是数组：同时只有一个后端生效
                              *   （要么 epoll 要么 kqueue，不可能并存）→ 一个洞就够。
                              *   对比 Redis 后来加的 void *privdata[3]：那是【上层】有
                              *   多个模块都要挂东西，所以要几个洞取决于「会同时挂几个」。
                              *
                              * ★ 和 aeFileEvent.clientData 是同一个手法：都是「公共结构                                    体
                              *   + 一个 void* 洞」，让稳定的骨架不必认识可替换的东西。
                              *   clientData 让 ae 不认识【上层业务类型】(Client*)，
                              *   apidata   让 ae 不认识【具体 IO 后端】(aeApiState)。                                 */
  
    aeBeforeSleepProc *beforesleep;  /* 睡【之前】的钩子（控制反转）。见下方说明。 */
    aeBeforeSleepProc *aftersleep;   /* 睡【之后】的钩子。见下方说明。 */

    /* ---- 定时器 ---- */
    aeTimeEvent *timeEventHead;      /* ★ 定时器链表头（无序）。NULL = 没有定时器。
                                      * 没有定时器时 epoll_wait 的 timeout 传 -1
                                      * （无限等待）—— 这正是阶段 3 之前的行为。 */
    long long timeEventNextId;       /* 下一个要分配的定时器 id（只增不减） */

    int flags;               /* 全局标志位，目前只有 AE_DONT_WAIT（= 本次不睡）。
                              * 与 aeProcessEvents 的 flags 参数是【两层】：
                              *   参数 = 调用点说「这次别睡」（单次，优先级更高）
                              *   本字段 = 某处说「接下来都别睡」（持续到显式关闭）
                              * 为什么要两个：本字段会在 beforesleep 里被改（比如收到
                              * SIGTERM 后禁睡要尽快退出），而参数的意图来自调用点，
                              * 不能被钩子悄悄推翻 → 判断时参数写在前面。
                              * ⚠ 本项目现阶段用不到（退出用 stop 就够），保留是为了
                              *   和 Redis 的 aeEventLoop 结构一一对上。 */
};

/* ★★ 两个睡眠钩子的说明（本项目的关键扩展点）★★
 *
 * 一次 aeProcessEvents 的顺序：
 *   ① beforesleep()   ← 睡之前
 *   ② 算 timeout
 *   ③ aeApiPoll()     ← ★ 唯一真正睡眠的地方
 *   ④ aftersleep()    ← 睡之后、分发之前
 *   ⑤ 遍历 fired[] 分发回调
 *
 * ● beforesleep：ae 不知道「有输出缓冲要 flush」，它只留一个空位。
 *   ★ 本项目【阶段 3 会用它来把各客户端 outbuf 里的数据 write 出去】。
 *   为什么 flush 放这里（而不是命令执行完立刻写）：
 *     ① 攒批 —— 一条 +OK 从 3 次 write 降到 1 次
 *     ② 该关闭的谁已经确定 → 不会白写
 *     ③ 写完即可撤掉 EPOLLOUT → epoll_wait 才能睡好
 *        （EPOLLOUT 几乎总是就绪，留着它 epoll_wait 会立刻返回 = 忙转）
 *
 * ● aftersleep：处理「睡眠期间世界变了」。程序睡在 epoll_wait 里时是【失控】的，
 *   醒来后需要一个统一的地方检查：睡了多久？系统时钟被 NTP 跳变了吗？
 *   是不是被信号 (EINTR) 打断醒的？Redis 用它检测 clock drift 并修正。
 *
 * 为什么两个钩子不对称：beforesleep 是你【主动收尾】（时间在你控制下）；
 * aftersleep 是你【刚醒】（不知道睡了多久、为什么醒）—— 所以职责完全不同。
 * ⚠ 本项目现阶段可以先传 NULL。 */

/* ---------- 生命周期 ---------- */
aeEventLoop *aeCreateEventLoop(int setsize);
void aeDeleteEventLoop(aeEventLoop *eventLoop);
void aeStop(aeEventLoop *eventLoop);
int aeGetSetSize(aeEventLoop *eventLoop);
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);

/* ---------- 文件事件 ---------- */
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
                      aeFileProc *proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);
void *aeGetFileClientData(aeEventLoop *eventLoop, int fd);

/* ---------- 时间事件（定时器） ----------
 * aeCreateTimeEvent(el, 毫秒, proc, clientData)：
 *   注册一个「再过 ms 毫秒执行」的定时器，返回它的 id（>0），失败返回 AE_ERR。
 *   ★ proc 的返回值决定后续行为：>0 表示「再过这么多毫秒再来」，
 *     AE_NOMORE 表示「只跑这一次，跑完删掉」。
 * aeDeleteTimeEvent(el, id)：按 id 删除。返回 AE_OK / AE_ERR（没找到）。
 * aeGetTimeEventsCount(el)：当前定时器数量（测试/调试用）。
 */
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
                            aeTimeProc *proc, void *clientData);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);
int aeGetTimeEventsCount(aeEventLoop *eventLoop);

/* 当前时间（毫秒）。★ 上层也用它给「最后一次活动」打时间戳
 * （如 Client.lastinteraction），这样全项目只有一个时间来源，
 * 不会出现「两处取时间、精度/基准不一致」的问题。 */
long long aeGetTimeMilliseconds(void);

/* ---------- 主循环 ---------- */
int aeProcessEvents(aeEventLoop *eventLoop, int flags);
void aeMain(aeEventLoop *eventLoop);
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);
void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep);
void aeSetDontWait(aeEventLoop *eventLoop, int noWait);

/* 后端名（调试用，"epoll"） */
char *aeGetApiName(void);

#endif /* MINI_REDIS_AE_H */
