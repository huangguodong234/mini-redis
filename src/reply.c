/* ============================================================
 * reply.c —— 响应编码层（RESP 输出）
 * ============================================================
 *
 * ★ 职责：把「要回复的内容」编码成 RESP 字节流，【追加进 c->outbuf】。
 *   本层是 outbuf 的生产者；消费者是 networking.c 的 writeToClient。
 *   一入一出，配成一对 —— 所以这两层合起来才是"发送字节流"这件事。
 *
 * ★ 本文件【不依赖】：
 *     · 任何 socket API —— 一行 write() 都没有（这是关键的设计约束）
 *     · 事件循环（ae.h）
 *
 *   「本层永不阻塞」是可以静态保证的：它只做内存拷贝 + sds 扩容。
 *   唯一可能的失败是 OOM，不是 EAGAIN。这个性质让命令层可以放心地
 *   在任意深度的调用栈里发响应，不必关心对端读得快不快。
 *
 * ★ 为什么必须要有 outbuf（对比改造前的阻塞直发）：
 *   改造前 send_response 直接 while 循环 write()，直到整条响应发完才返回。
 *   「数据还没发出去」这个状态由【内核发送缓冲区】暗中承担，代价是对端
 *   读得慢时本进程会卡在 write 里 —— 整个服务器停摆。
 *
 *   改成非阻塞后，write 可能只发出去一部分（n < len）或完全发不出去
 *   （EAGAIN）。剩余部分不存起来就是「发送半包」：客户端会一直等后半条
 *   RESP 回复。所以这个状态必须由我们自己显式持有 —— 载体就是 outbuf。
 *
 * ★ 为什么「攒进缓冲」在改造后反而更快：
 *   改造前一条 +OK 要 3 次 write（"+"/"OK"/"\r\n"）≈ 3 次系统调用；
 *   现在 3 次 sdscatlen（纯内存操作）攒成 1 次 write。
 *   下面每个 send_* 都还是"分段追加"——但分段本身不再有代价了。
 */

#include <stdio.h>      /* snprintf */
#include <string.h>     /* strlen */
#include <stdlib.h>

#include "server.h"
#include "sds.h"

// 追加 len 字节到客户端的输出缓冲。
// ★ 参数是 Client*（不是 fd）—— 因为要访问 c->outbuf。fd 在 c->fd 里。
// ★ 本函数【不】执行 write，所以永远不阻塞、也不会失败（除 OOM）。
void send_response(Client *c, const void *data, size_t len){
    if (!c || !data || len == 0) return;

    /* ★ 输出缓冲上限检查（续命检查，fail-fast）：
     *   对端不读回复时，write() 会一直 EAGAIN，outbuf 只增不减。
     *   没有上限的话，一条连接就能把进程撑到 OOM（实测 3 条连接
     *   让 RSS 从 1MB 涨到 5.3GB 并继续涨）。
     *   ★ 这里只做「标记」，不在这里断开连接 —— 因为 send_response
     *     是在命令执行途中被调用的，此刻 freeClient 会让命令层手里
     *     的 Client* 立刻变成悬垂指针。
     *     正确做法：打标记，由事件循环在【回调返回之后】统一踢掉
     *     （见 beforeSleep 里的处理），那才是安全点。 */
    if (sdslen(c->outbuf) - c->outbuf_pos + len > MAX_OUTBUF) {
        if (!c->output_over_limit) {
            c->output_over_limit = 1;
            fprintf(stderr,
                    "[fd=%d] 输出缓冲超限（未发 %zu + 新增 %zu > %d），将在本轮末断开"
                    "（对端不读回复）\n",
                    c->fd, sdslen(c->outbuf) - c->outbuf_pos, len, MAX_OUTBUF);
            /* 尽量让客户端知道原因；即使发不出去也不影响断开流程 */
            send_response(c, "-ERR output buffer limit reached, closing\r\n",
                          sizeof("-ERR output buffer limit reached, closing\r\n") - 1);
        }
        return;   /* ★ 不再追加，防止内存继续膨胀 */
    }

    /* sds 是二进制安全的（带长度），所以数据里的 '\0' 不会截断 */
    sds nb = sdscatlen(c->outbuf, data, len);
    if (!nb) {
        /* OOM：outbuf 保持原样（sdscatlen 失败不释放旧 sds）。
         * 这里只能放弃本次追加 —— 不 freeClient，因为调用方（命令层）
         * 可能还在用它，而且丢掉一条回复比断开连接更温和。
         * ★ 注意：调用方无法感知这个失败（签名是 void），这是当前的局限；
         *   Redis 会用 addReplyError 之类显式处理 OOM。 */
        fprintf(stderr, "[fd=%d] 输出缓冲扩容失败，丢弃 %zu 字节\n", c->fd, len);
        return;
    }
    c->outbuf = nb;
    /* ★ 标记"有新数据"，让 beforeSleep 知道该尝试写一次。
     *   没有这个标记时，beforeSleep 只能每轮都试 → 对端不读时
     *   EAGAIN 紧循环把 CPU 打满（实测 86%），并拖慢其它连接。 */
    c->reply_pending = 1;
}

// ==================== RESP 编码 / 发送辅助函数 ====================
// 每个函数把一种 RESP 数据类型「拼接」封装成一步，命令层只调这些 API。
//
// 统一思路：把 RESP 拆成前缀/内容/后缀几段，逐段 send_response 追加进 outbuf：
//   - 固定字面量（":"、"$"、"\r\n" 等）直接追加；
//   - 长度不定的字符串（s / msg / val 数据）按 strlen/sdslen 按长度追加，绝不 %s；
//   - 数字只用一个“数学上有界”的小临时数组装 ASCII（long/size_t 十进制
//     最多 20 位 + 符号，24 字节足够），不存在“长度未知的固定数组”截断隐患。

// 简单字符串  +<s>\r\n：分三段  "+"  s  "\r\n"
void send_simple_string(Client *c, const char *s){
    if(!s) return;
    send_response(c, "+", 1);
    send_response(c, s, strlen(s));   // s 长度任意，按长度发
    send_response(c, "\r\n", 2);
}

// 错误  -<msg>\r\n：分三段  "-"  msg  "\r\n"
void send_error(Client *c, const char *msg){
    if(!msg) return;
    send_response(c, "-", 1);
    send_response(c, msg, strlen(msg));   // msg 长度任意，按长度发
    send_response(c, "\r\n", 2);
}

// 整数  :<n>\r\n：n 是 long，十进制位数数学上有界（≤20 位），小数组足够
void send_integer(Client *c, long n){
    char num[24];
    int len = snprintf(num, sizeof(num), ":%ld\r\n", n);
    send_response(c, num, (size_t)len);
}

// 批量字符串（按明确长度）：$<len>\r\n + <data> + \r\n，二进制安全
// len 是 size_t，十进制位数数学上有界（≤20 位），片段依次追加
void send_bulk_string_len(Client *c, const void *data, size_t len){
    char hdr[24];
    int hl = snprintf(hdr, sizeof(hdr), "$%zu\r\n", len);
    send_response(c, hdr, (size_t)hl);   // 长度行（纯文本，无 \0）
    send_response(c, data, len);         // 数据（按长度，二进制安全）
    send_response(c, "\r\n", 2);         // 尾部
}

// 批量字符串（sds 版）：val == NULL → $-1\r\n，否则按 sdslen 发完整数据
void send_bulk_string(Client *c, sds val){
    if(!val){
        send_null_bulk(c);
        return;
    }
    char hdr[24];
    size_t len = sdslen(val);
    int hl = snprintf(hdr, sizeof(hdr), "$%zu\r\n", len);
    send_response(c, hdr, (size_t)hl);   // 长度行（纯文本，无 \0）
    send_response(c, val, len);          // 数据（按长度，二进制安全）
    send_response(c, "\r\n", 2);         // 尾部
}

// 空批量字符串  $-1\r\n  （注意：$-1\r\n 只有 5 字节，发送长度传 5，
//                         若误传 strlen("$-1\r\n")=6 会把结尾 '\0' 也发出去，
//                         在回复流里多播一个 NUL 字节，破坏二进制精确客户端）
void send_null_bulk(Client *c){
    send_response(c, "$-1\r\n", 5);
}

// 数组头  *<n>\r\n：n 是 long，十进制位数数学上有界（≤20 位），小数组足够
void send_array_len(Client *c, long n){
    char num[24];
    int len = snprintf(num, sizeof(num), "*%ld\r\n", n);
    send_response(c, num, (size_t)len);
}
