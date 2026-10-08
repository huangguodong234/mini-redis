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

/* ============================================================
 * Client —— 每个连接的状态
 * ============================================================
 * ★ 改造后新增两个链表指针：Client 串成【双向链表】，用于
 *   「遍历所有客户端」（如 beforSleep 里 flush 输出、超时检查）。
 *
 * 为什么是双向：free_client 要在任意位置 O(1) 摘除自己。
 *   单向链表摘除当前节点必须先找到前驱 → O(n)；而 free_client 会在
 *   很多地方被调（协议错误、写错误、超时、Ctrl+C），每次都要 O(1)。
 *   代价是每个 Client 多 16 字节，相对 Client 本身可忽略。
 *
 * 为什么还要链表（而不是只靠 events[fd].clientData）：
 *   events[] 是「fd → Client」的查找表（O(1)），但它【不能遍历】——
 *   稀疏数组要扫全部槽位且有空洞。链表负责「枚举全部客户端」。
 *   ★ 两个容器存同一批对象，各服务一类操作（同 Redis 的 clients 链表）。 */
typedef struct Client {
    int fd;
    sds querybuf;             // 请求缓冲区（SDS 动态扩容，不再有固定大小限制）
    size_t qb_pos;            // 解析游标：已解析数据的进度（相对 querybuf 起点）
                              // 缓冲区有效长度用 sdslen(querybuf) 取，O(1)

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
} Client;

/* ============================================================
 * 全局服务状态
 * ============================================================
 * ★ 为什么需要它：事件回调的签名是固定的
 *     void cb(aeEventLoop *el, int fd, void *clientData, int mask)
 *   只能拿到 clientData（= Client*）。但执行命令还需要 Storage 和 ZSet。
 *   两种办法：
 *     ① 回调的 clientData 挂一个「含 Client + 全局指针」的结构
 *     ② ★ 用全局变量（本项目选这个，Redis 也是这么做的）
 *   因为 storage/zset 本来就是【进程唯一】的，不是每个连接一份。 */
typedef struct {
    Storage *store;
    ZSet *zset;
    aeEventLoop *el;
    Client *clients;      // 客户端双向链表头
    int numclients;       // 当前连接数（用于 maxclients 检查）
} Server;

extern Server g_server;

/* ============================================================
 * 网络驱动层（networking.c）
 * ============================================================ */

/* listen fd 可读回调：循环 accept 到 EAGAIN，为每个新连接建 Client 并注册可读。
 * 用法：aeCreateFileEvent(el, server_fd, AE_READABLE, acceptTcpHandler, NULL) */
void acceptTcpHandler(aeEventLoop *el, int fd, void *clientData, int mask);

/* 客户端 fd 可读回调：★ 只读一次（非阻塞），追加到 querybuf 后交给解析层。
 * 对应 Redis 的 readQueryFromClient。 */
void readQueryFromClient(aeEventLoop *el, int fd, void *clientData, int mask);

/* beforesleep 钩子：本轮事件处理完、即将睡眠前的收尾。
 * ★ 阶段 3 会在这里 flush 各客户端的 outbuf；阶段 2 只做占位。 */
void beforeSleep(aeEventLoop *el);

/* 关闭连接并释放：★ 严格按「撤关注 → 关 fd → 摘链表 → 释放」四步。
 * 顺序不能换 —— 详见 networking.c 里的说明。 */
void freeClient(Client *c);

/* ============================================================
 * 客户端生命周期 / socket 创建（server.c）
 * ============================================================ */
Client *create_client(int fd);
void free_client(Client *c);              // 旧名，内部转发到 freeClient
int create_server_socket(int port, int backlog);

/* ============================================================
 * 响应发送（server.c）
 * ============================================================ */
// 发送响应给客户端：按明确字节长度发送的底层函数（二进制安全，数据可含 '\0'）
// 唯一执行 write() 循环（处理部分写入 / EINTR）的地方；上层辅助函数全部经由它发出。
// ★ 阶段 2 仍是阻塞式直接 write；阶段 3 会改成「追加进 outbuf + 由事件循环发送」。
void send_response(int client_fd, const void *data, size_t len);

// ==================== RESP 编码 / 发送辅助函数 ====================
// 命令层（commands.c）只允许调用下面这些高层 API 来拼接并发送响应，
// 禁止直接碰 snprintf / send_response 的原始拼接细节——所有“拼接+发送”
// 逻辑都封装在 server.c 里，全部经由 send_response 发出。
// 每个函数只负责一“种” RESP 数据类型，职责单一、可复用。
void send_simple_string(int client_fd, const char *s);
void send_error(int client_fd, const char *msg);
void send_integer(int client_fd, long n);
void send_bulk_string_len(int client_fd, const void *data, size_t len);
void send_bulk_string(int client_fd, sds val);
void send_null_bulk(int client_fd);
void send_array_len(int client_fd, long n);

/* ============================================================
 * 解析层（server.c）—— 三层解析器，不碰网络
 * ============================================================ */

/*
*第二层：调用processMultibulkBuffer，缩容缓存区
*对应Redis processInputBuffer
*/
int processInputBuffer(Client *c, Storage *store, ZSet *zset);

/*
*第三层：解析器（RESP 数组协议，首字节为 '*'）
*对应Redis processMultibulkBuffer
*/
int processMultibulkBuffer(Client *c, Storage *store, ZSet *zset);

/*
*第三层（inline 分支）：解析空格分隔的行命令（首字节非 '*'），
*兼容 nc/curl 等不用 RESP 协议的客户端
*/
int processInlineCommand(Client *c, Storage *store, ZSet *zset);

#endif
