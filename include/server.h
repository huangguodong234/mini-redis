#ifndef SERVER_H
#define SERVER_H

#include "ae.h"
#include "anet.h"
#include "storage.h"
#include "zset.h"
#include "sds.h"
#include <stddef.h>
#include <signal.h>

/* 全局运行标志：由 main.c 定义，SIGINT 处理函数把它置 0。
 * ★ 事件循环改造后它仍然有用：aeMain 的退出靠 aeStop()，但业务代码
 *   （比如命令层想主动退出）仍可读它。 */
extern volatile sig_atomic_t Keep_running;

/* 最大连接数（同时决定 ae 事件数组的容量上限 setsize）。
 * ★ 为什么不设成 ulimit -n 的极限：还要留出给日志/临时文件等的 fd。
 *   Redis 默认 10000，本项目取同样量级。 */
#define MAX_CLIENTS 10000

/* 单个客户端允许积压的【输出】缓冲上限（对应 Redis 的
 * client-output-buffer-limit normal 的硬限制）。
 *
 * ★ 为什么必须有它 —— 这是混合负载测试挖出来的真实漏洞（不是理论风险）：
 *   一个连上来、拼命发命令但【从不读回复】的客户端（下游卡死、
 *   被防火墙挡住、故意搞破坏），会让服务器把回复不断堆进 c->outbuf。
 *   而 write() 永远返回 EAGAIN（对端收窗口是满的），所以 outbuf
 *   只增不减。实测 3 条这样的连接，RSS 从 1MB 涨到 5.3GB 且还在涨，
 *   直接把整台机器拖进 OOM —— 一条连接就能打死整个进程。
 *
 * ★ 为什么在【入队】处拦，而不是在写失败处拦：
 *   写失败只能看到一次次 EAGAIN，无法判断"已经堆了多少"；
 *   而在 send_response 里，size 是精确的，超限就能立刻判定并断开。
 *
 * ★ 阈值取 64MB：正常客户端读得比写得快，outbuf 通常只有几百字节~
 *   几 KB（一条 1MB 的 GET 回复也就 1MB）。真到 64MB，说明对端
 *   已经停止读取了 —— 此时断开是唯一正确的处理（Redis 也是这么做的）。 */
#define MAX_OUTBUF (64 * 1024 * 1024)

/* ============================================================
 * Client —— 每个连接的状态
 * ============================================================
 * ★ 改造后新增两个链表指针：Client 串成【双向链表】，用于
 *   「遍历所有客户端」（如 beforeSleep 里 flush 输出、超时检查）。
 *
 * 为什么是双向：freeClient 要在任意位置 O(1) 摘除自己。
 *   单向链表摘除当前节点必须先找到前驱 → O(n)；而 freeClient 会在
 *   很多地方被调（协议错误、写错误、超时、Ctrl+C），每次都要 O(1)。
 *   代价是每个 Client 多 16 字节，相对 Client 本身可忽略。
 *
 * 为什么还要链表（而不是只靠 events[fd].clientData）：
 *   events[] 是「fd → Client」的查找表（O(1)），但它【不能遍历】——
 *   稀疏数组要扫全部槽位且有空洞。链表负责「枚举全部客户端」。
 *   ★ 两个容器存同一批对象，各服务一类操作（同 Redis 的 clients 链表）。 */
typedef struct Client {
    int fd;
    /* ★ 输出缓冲已超限（由 reply.c 置位，由 beforeSleep 统一断开）。
     *   为什么用标记而不是当场断开：send_response 在命令执行途中被调用，
     *   此刻 freeClient 会让调用栈上手里的 Client* 立刻悬垂。
     *   「打标记 + 延迟处理」是事件循环里处理"回调中途发现要关闭"的通用手法。 */
    int output_over_limit;
    sds querybuf;             // 请求缓冲区（SDS 动态扩容，不再有固定大小限制）
    size_t qb_pos;            // 解析游标：已解析数据的进度（相对 querybuf 起点）
                              // 缓冲区有效长度用 sdslen(querybuf) 取，O(1)

    /* ★ 输出缓冲区（事件循环改造新增）——
     *   非阻塞 write 一旦写不完（EAGAIN），剩余数据必须有个地方存着，
     *   否则就是「发送半包」：客户端收到半条 RESP 回复，解析器会一直等。
     *
     *   ★ 对比改造前：阻塞式 send_response 用 while 循环 write，直到发完
     *   才返回 —— 数据「还没发出去」这个状态由内核缓冲区暗中承担。
     *   改成非阻塞后，主动权回到事件循环，这个状态就必须由我们自己显式持有，
     *   载体就是它。 */
    sds outbuf;
    /* ★ outbuf 里【已发送】的字节数（写游标）。
     *   为什么不每写一次就 sdsrange 把剩下的挪到开头：那样每次都要
     *   memmove 整个剩余缓冲，对端读得慢时（outbuf 几十 MB、每次只
     *   写出几 KB）变成 O(n²) —— 实测单次 memmove 可达 6.4ms。
     *   游标一直推进，只在【全部发完】时回收一次空间。 */
    size_t outbuf_pos;
    /* ★ 本轮是否有新数据待发送（send_response 置位，beforeSleep 消费）。
     *   用来避免"对端不读时每轮都去 write 一次 EAGAIN"造成的 CPU 空转。 */
    int reply_pending;

    // 状态机存档变量（对应 Redis processMultibulkBuffer 的状态）
    int cmd_start;
    int multibulklen;   // 剩余待解析的参数个数，0 表示还没解析命令头
    long bulklen;       // 当前参数的剩余长度，-1 表示尚未解析该参数的长度

    // 参数暂存数组（解析过程中动态填充）
    sds *argv;        // 参数数组（每个元素是 sds，所有权归 Client，自由时 sdsfree）
    int argc;           // 当前已解析的参数个数
    int argv_max;       // argv 数组的容量

    /* ---- 链表指针（把所有客户端串起来） ---- */
    struct Client *next;
    struct Client *prev;

    /* ★ 最后一次产生「可观测活动」的时刻（毫秒，来自 aeGetTimeMilliseconds）。
     *   用于空闲超时检测（serverCron 里轮询）。
     *
     *   ★ 为什么用【轮询】而不是「每个客户端一个定时器」：
     *     后者在 10000 连接时会产生 10000 个定时器，ae 的 O(n) 扫描
     *     就变成 O(10000)/轮 —— 而定时器本该只有 1~2 个。
     *     轮询把成本从「定时器数量」转回了「客户端数量」，
     *     而遍历客户端本来就是必须的（beforeSleep 每轮都在做）。
     *
     *   ★ 语义：只在【读到客户端数据】时更新。
     *     所以「连上但不发任何命令」的连接会被踢 —— 这正是想要的
     *     （防住只占 fd 不发数据的连接）。 */
    long long lastinteraction;
} Client;

/* ============================================================
 * 全局服务状态
 * ============================================================
 * ★ 定义在 networking.c（它是主要使用者：accept 要用 store/zset，
 *   freeClient 要用 el）；这里只是 extern 引用。
 *
 * ★ 为什么还需要全局：事件回调的签名是固定的
 *     void cb(aeEventLoop *el, int fd, void *clientData, int mask)
 *   clientData 只够挂 Client*。但执行命令还需要 Storage 和 ZSet。
 *   两种办法：
 *     ① 回调的 clientData 挂一个「含 Client + 全局指针」的结构
 *     ② ★ 用全局变量（本项目选这个，Redis 也是这么做的）
 *   因为 storage/zset 本来就是【进程唯一】的，不是每个连接一份。
 *
 * ⚠ 使用约定（2026 拆分解耦后新增）—— 能不用 g_server 就不用：
 *   · el：【已从本结构体删除】。所有需要事件循环的地方都改为从回调参数
 *         拿（回调签名的第一个参数就是 aeEventLoop *el）。原来那个
 *         g_server.el 字段在改完之后变成【只写不读】的死状态 ——
 *         死状态比重复代码更危险：它看起来像"有地方在用"，
 *         实际谁都没读，下次有人想用它时会发现它可能是 NULL/过期的。
 *   · store / zset：回调签名里拿不到 → 这两项允许走 g_server
 *         （真进程唯一，符合上面的理由 ②）。
 *   ★ 判断标准：能从参数拿到的一律走参数；拿不到的才走全局。 */
typedef struct {
    Storage *store;
    ZSet *zset;
    Client *clients;      // 客户端双向链表头
    int numclients;       // 当前连接数（用于 maxclients 检查）

    /* ---- 定时任务配置（阶段 4 新增） ---- */
    int hz;               /* serverCron 每秒跑多少次（Redis 默认 10，即每 100ms） */
    int maxidletime;      /* 客户端空闲多少秒后被踢掉（0 = 不检查超时） */
    long long cronloops;  /* serverCron 已经跑过多少轮（调试/统计用） */
} Server;

extern Server g_server;

/* ============================================================
 * 定时任务（networking.c）
 * ============================================================
 * ★ serverCron —— 所有周期性工作的【唯一入口】。
 *
 *   ★ 为什么是「一个定时器 + 内部分发」而不是「多个定时器」：
 *     Redis 的做法就是一个 serverCron，内部依次检查各种条件
 *     （AOF、主从、clientsCron、内存整理……），每个子任务有自己的
 *     频率计数器（每 N 轮跑一次）。
 *     这样定时器链表永远是 O(1) 个节点，扫描成本恒定。
 *
 *   本项目它做两件事：
 *     ① 检查客户端空闲超时（分批，见实现注释）
 *     ② 维护 cronloops 计数
 */
int serverCron(aeEventLoop *el, long long id, void *clientData);

/* ============================================================
 * 网络驱动层（src/networking.c）
 * ============================================================ */

/* listen fd 可读回调：循环 accept 到 EAGAIN，为每个新连接建 Client 并注册可读。
 * 用法：aeCreateFileEvent(el, server_fd, AE_READABLE, acceptTcpHandler, NULL) */
void acceptTcpHandler(aeEventLoop *el, int fd, void *clientData, int mask);
void acceptTcpHandler(aeEventLoop *el, int fd, void *clientData, int mask);

/* 启动时预留 1 个应急 fd（打开 /dev/null 占住），供 fd 耗尽时回 -ERR 使用。
 * ★ 为什么必须预留：accept() 因 EMFILE 失败时内核【不返回 fd】，
 *   没有 fd 就连一条错误消息都发不出去，客户端只能干等到超时。
 *   预留一个名额后，真到 EMFILE 就关掉它腾出位置，accept 一个、
 *   回 -ERR、再关、然后重新占位（Redis 同款手法）。
 * 用法：在创建监听 socket 之后、进入事件循环之前调一次。 */
void reserveCrisisFd(void);

/* 客户端 fd 可读回调：★ 只读一次（非阻塞），追加到 querybuf 后交给解析层。
 * 对应 Redis 的 readQueryFromClient。 */
void readQueryFromClient(aeEventLoop *el, int fd, void *clientData, int mask);

/* beforesleep 钩子：本轮事件处理完、即将睡眠前的收尾。
 * ★ 在这里 flush 各客户端的 outbuf。 */
void beforeSleep(aeEventLoop *el);

/* 关闭连接并释放：★ 严格按「撤关注 → 关 fd → 摘链表 → 释放」四步。
 * 顺序不能换 —— 详见 networking.c 里的说明。
 * ★ el 用于撤关注；调用点（事件回调）手头都有它。传 NULL = 不撤关注。 */
void freeClient(aeEventLoop *el, Client *c);

/* ============================================================
 * 连接生命周期 / socket 创建（src/server.c）
 * ============================================================ */
Client *create_client(int fd);
void free_client(Client *c);              // 旧名，内部转发到 freeClient
int create_server_socket(int port, int backlog);

/* ============================================================
 * 响应编码层（src/reply.c）
 * ============================================================
 * ★ 本层只把数据【追加进 c->outbuf】，永不执行 write、永不阻塞。
 *   真正发出去的 write 在 networking.c 的 writeToClient —— 一入一出配对。
 *
 *   为什么必须先入队：非阻塞 write 可能只发出去一部分（或返回 EAGAIN），
 *   剩余数据必须存起来等下次「可写」通知 —— 否则就是发送半包。
 *
 *   发送时机：由 beforeSleep（和可写事件回调）调 writeToClient 统一 flush。
 *   放 beforeSleep 的理由：① 攒批（一条 +OK 从 3 次 write 降到 1 次）
 *   ② 该关闭的谁已确定，不会白写 ③ 写完即可撤掉 EPOLLOUT，epoll_wait 才能睡好。 */

// 追加数据到客户端的输出缓冲（不执行 write）。
// ★ 参数是 Client*（不是 fd）—— 因为要访问 c->outbuf；fd 在 c->fd 里。
void send_response(Client *c, const void *data, size_t len);

// ==================== RESP 编码 / 发送辅助函数 ====================
// 命令层（commands.c）只允许调用下面这些高层 API 来拼接响应，
// 禁止直接碰 snprintf / write 的原始细节。
// 每个函数只负责一“种” RESP 数据类型，职责单一、可复用。
void send_simple_string(Client *c, const char *s);
void send_error(Client *c, const char *msg);
void send_integer(Client *c, long n);
void send_bulk_string_len(Client *c, const void *data, size_t len);
void send_bulk_string(Client *c, sds val);
void send_null_bulk(Client *c);
void send_array_len(Client *c, long n);

/* ============================================================
 * 解析层（src/parser.c）—— 三层解析器
 * ============================================================
 * ★ 本层只认字节流：不碰 socket、不碰事件循环、不碰响应编码。
 *   它读 c->querybuf，写 c->argv / c->argc，然后调 handle_command。
 *   唯一的"输出"是返回值：-1 表示该断开（由 networking.c 决定怎么断）。 */

/*
*第一层：入口。循环调用下层处理粘包，最后压缩缓冲区
*对应 Redis 的 processInputBuffer
*返回 1=正常（可能已处理若干命令，或半包等待）；-1=协议错误/应断开
*/
int processInputBuffer(Client *c, Storage *store, ZSet *zset);

/*
*第二层：解析器（RESP 数组协议，首字节为 '*'）
*对应 Redis 的 processMultibulkBuffer
*返回 1=成功一条；0=半包；-1=协议错误；-2=命令层要求断开(OOM)
*/
int processMultibulkBuffer(Client *c, Storage *store, ZSet *zset);

/*
*第二层（inline 分支）：解析空格分隔的行命令（首字节非 '*'），
*兼容 nc/curl 等不用 RESP 协议的客户端
*返回同上
*/
int processInlineCommand(Client *c, Storage *store, ZSet *zset);

#endif
