#ifndef COMMANDS_H
#define COMMANDS_H

#include "protocol.h"   // 解析要用到的辅助函数声明
#include "storage.h"    // Storage 类型
#include "zset.h"       // ZSet 类型
#include "server.h"     // send_response 声明（因为 commands.c 要用）

// 返回值：1 = 正常响应完成；-1 = 应断开连接（如 OOM：错误已发出，继续响应不安全，B8 修复）
//
// 参数说明（cmd）：
//   cmd 是解析器打包的 Command（见 protocol.h），其中 cmd.argv 指向
//   server.c（processMultibulkBuffer / processInlineCommand）创建的 sds 数组，
//   所有权归调用方；命令层只读使用。唯一例外是 SET 分支的
//   "steal 所有权"优化：把 key/value 两格的 sds 直接移交给哈希表，并把这
//   两格 cmd->argv[i] 置 NULL（server.c 的 sdsfree(NULL) 是安全 no-op），因此调用方
//   循环释放不 double-free、哈希表不泄漏。其余命令的 argv 一律归调用方释放。
// ★ 事件循环改造：第一个参数从 int client_fd 改为 Client *c。
//   因为响应不再直接 write，而是追加进 c->outbuf（非阻塞模型必须缓冲）。
//   fd 仍在 c 里（c->fd）—— 需要时取即可。
int handle_command(Client *c, Command *cmd, Storage *store, ZSet *zset);

/* ============================================================
 * 响应发送：命令层只管"拼"，不管"发"
 * ------------------------------------------------------------
 * 命令层（commands.c）不直接碰 write()，只调用 server.c 封装的 send_* 辅助函数
 * （send_simple_string / send_error / send_integer / send_bulk_string /
 *  send_array_len……）。它们只做一件事：把拼好的字节【追加进 c->outbuf】。
 *
 * 一、为什么必须要有输出缓冲（outbuf）
 *   TCP 的发送缓冲由内核持有且大小有限，并受对端接收窗口（流量控制）影响。
 *   一旦内核 send buffer 满：
 *     · 阻塞模式：write() 卡住等空间 → 整个进程停摆。
 *     · 非阻塞模式：write() 立刻返回 -1 / EAGAIN。
 *   所以"一次 write 把整条响应发完"并不成立 —— 对端慢时根本发不完，
 *   剩下的数据【必须有个地方暂存】，否则就是"发送半包"：
 *   客户端收到半条 RESP 回复，它的解析器会一直等后半截。
 *
 *   本项目已完成 epoll + 非阻塞改造，所以 outbuf 是【必需】的：
 *     send_*  →  追加进 c->outbuf（纯内存操作，永不阻塞）
 *     真正的 write  →  由 networking.c 的 writeToClient 在
 *                      beforeSleep / 可写事件回调里执行；
 *                      一次写不完就留在缓冲里，下次可写再续。
 *   ★ 这也是 Redis 的做法（它用小响应 c->buf + 大响应 reply 链表）。
 *
 * 二、分段追加不再有代价
 *   send_* 把 RESP 拆成前缀/内容/后缀逐段 send_response 追加：
 *     - 固定字面量（":"、"$"、"\r\n"）直接追加；
 *     - 长度不定的字符串按 strlen/sdslen 按长度追加，绝不 %s（防 '\0' 截断）；
 *     - 数字只用一个数学上有界的 24B 小数组装 ASCII
 *       （long/size_t 十进制最多 20 位），不存在"长度未知的固定数组"
 *       截断隐患，也不临时 malloc 一块 sds 再释放。
 *   ★ 改造前这些分段是【多次系统调用】（一条 +OK = 3 次 write）；
 *     现在只是对同一块 sds 的多次 sdscatlen，最后由 writeToClient
 *     一次性发出 —— 攒批反而更快。
 * ============================================================ */

#endif