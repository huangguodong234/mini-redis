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
int handle_command(int client_fd, Command *cmd, Storage *store, ZSet *zset);

/* ============================================================
 * 响应发送：为什么"分段直发"、为什么这里不需要输出缓冲
 * ------------------------------------------------------------
 * 命令层不直接碰 write()，只调用 server.c 里封装的 send_* 辅助函数
 * （send_simple_string / send_error / send_integer / send_bulk_string /
 *  send_array_len……）；真正的 write 循环唯一在 send_response() 里。
 *
 * 一、write() 的非阻塞改造与"发送缓冲"问题
 *   要理解这套设计，先看清 TCP 为什么需要缓冲：
 *   - 内核的发送缓冲区（send buffer）是有限大小的。
 *   - TCP 有流量控制（滑动窗口）：当客户端（对端）读得慢 / 它的
 *     接收窗口收成 0 时，我方 send buffer 会被占满（数据排不出去）。
 *   - 一旦 send buffer 满：
 *       * 阻塞模式：write() 卡住等空间 → 阻塞。
 *       * 非阻塞模式：write() 立刻返回 -1，errno==EAGAIN，一个字节
 *         都没发出去。
 *   所以"一次 write() 把整条响应发完"并不是理所当然——对端慢时
 *   你根本发不完，剩下的数据必须有个地方暂存。
 *
 *   真正的 Redis 用的是事件循环 + 非阻塞 socket：它在每个客户端
 *   上维护一个"输出缓冲"（小响应用 c->buf，大响应用 reply 链表），
 *   addReply* 只把拼好的响应写进缓冲，等 ae 事件循环检测到
 *   EPOLLOUT（socket 可写）时才真正 write 出去；一次写不完就留在
 *   缓冲里，下次可写再续。输出缓冲就是"兜住没写完那部分"的暂存区，
 *   这是非阻塞模型下"必须做"的事，而不是可有可无的优化。
 *
 * 二、本 mini-redis 为什么可以"分段直发、不做输出缓冲"
 *   我们是阻塞式单线程模型：readQueryFromClient 在一个循环里同步
 *   read → 解析 → 处理 → write，全程一次调用就能把整条响应写完
 *   （send_response 内部用循环 write 处理部分写入和 EINTR，保证
 *   一次性发完，RESP 协议不截断）。没有"下次可写再续写"的需求，
 *   因此不需要客户端输出缓冲，直接分段写出去即可——既省内存，
 *   也不用事件循环，是当前模型下最简单正确的方案。
 *   等到将来引入 事件循环+非阻塞 时，才需要补上客户端输出缓冲。
 *
 * 三、分段直发 == 抽象掉固定缓冲与临时 sds
 *   send_* 辅助函数把 RESP 拆成前缀/内容/后缀逐段 send_response：
 *     - 固定字面量（":"、"$"、"\r\n"）直接发；
 *     - 长度不定的字符串按 strlen/sdslen 按长度发，绝不 %s（防 '\0' 截断）；
 *     - 数字只用一个数学上有上界的 24B 小数组装 ASCII
 *       （long/size_t 十进制最多 20 位），不存在"长度未知的固定数组"
 *       截断隐患，也不临时 malloc 一块 sds 再释放。
 * ============================================================ */

#endif