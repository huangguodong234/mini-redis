/* ============================================================
 * parser.c —— 协议解析层（三层解析器）
 * ============================================================
 *
 * ★ 职责：把 c->querybuf 里的【字节流】变成【参数数组】，然后交给命令层。
 *
 *   processInputBuffer     ← 第 1 层入口：循环调用下层，处理粘包，压缩缓冲
 *     processMultibulkBuffer ← 第 2 层：RESP 数组协议状态机（首字节 '*'）
 *     processInlineCommand   ← 第 2 层（分支）：空格分隔的行命令（nc/curl）
 *       protocol.c           ← 第 3 层：单个语法单元（*N\r\n / $L\r\n / 内容）
 *
 * ★ 本文件【只依赖】：
 *     · Client / Command 结构体（include/server.h、include/protocol.h）
 *     · sds（动态缓冲）
 *     · protocol.c 的三个辅助函数
 *
 * ★ 本文件【不依赖】：
 *     · 任何 socket API（accept/read/write/close）—— 一行都没有
 *     · 事件循环（ae.h）
 *     · 全局状态 g_server
 *
 *   这就是把它从 server.c 里拆出来的全部理由：解析规则的【变化原因】
 *   和网络模型、响应格式都不同 —— 改 RESP 解析不该重编网络层，
 *   换 epoll 也不该重编解析层。
 *
 * ★ 关于「状态机」：解析器本身无状态，状态全在 Client 的
 *   querybuf / qb_pos / multibulklen / bulklen 里。所以同一个函数可以在
 *   任意次 read 之间被反复调用，从上次停下的地方继续 —— 这就是半包
 *   （数据没到齐）能正确工作的原因，也是"存档续关"的思想。
 *
 *   为什么这样设计（对比改造前的固定缓冲 + 一次 read）：
 *     粘包：一次 read 收到多条命令，解析完一条就返回 → 后面的命令丢失。
 *     半包：一条命令跨多次 read，被截断的数据解析失败且会被下次 read 覆盖。
 *   用 sds 追加 + 游标续解 + while 循环，两个问题同时消失。
 */

#include <stdlib.h>
#include <string.h>

#include "server.h"      /* Client */
#include "protocol.h"    /* 第 3 层辅助函数 + Command */
#include "commands.h"    /* handle_command */
#include "sds.h"

/*
 * 第一层：processInputBuffer
 * 对应 Redis 的 processInputBuffer
 *
 * 职责：
 *   1. 循环调用 processMultibulkBuffer 解析并执行命令（处理粘包）
 *   2. 如果遇到半包（数据不够），停止循环，等待下次读数据
 *   3. 压缩缓冲区：把已处理的垃圾数据移除
 *
 * 返回值：
 *   1  - 正常（可能处理了若干命令，或者半包等待）
 *  -1  - 协议错误或客户端应断开
 *
 * ★ 注意：本函数【不】碰网络，也【不】负责关闭连接 ——
 *   它只返回 -1 表示「该断开了」，由调用方（networking.c 的
 *   readQueryFromClient）决定怎么断。这是解析层和网络层的边界。
 */
int processInputBuffer(Client *c, Storage *store, ZSet *zset){
    // 循环解析：只要游标后面还有数据，就尝试解析命令（处理粘包）
    // 这里【不】检查 Keep_running —— 本函数的契约是"把缓冲区里所有完整命令一次
    // 处理完"，中途因退出标志跳出会留下"处理了一半"的状态：虽然 sdsrange 会把
    // 未处理数据保留在缓冲区里、随后由 freeClient 统一释放（不是内存错误），
    // 但客户端已经发来的命令会被无谓丢弃，而且让解析层多背了一个"感知关停"的
    // 职责。退出意图由事件循环统一负责（aeStop 让 aeMain 退出），
    // 那只发生在本轮回调全部返回之后，因此不会丢命令。
    while((size_t)c->qb_pos < sdslen(c->querybuf)){
        // 按解析状态分流（不能只看首字节！）：
        //   1) multibulklen > 0 说明一条 RESP 命令正跨多次 read 解析中（上次的半包），
        //      当前游标处的字节是该命令的参数（$L...），必须继续走 multibulk 状态机——
        //      若只看首字节，'$' 会被误判成 inline 行，参数被当成"未知命令"。
        //   2) 否则按首字节：'*' 走 RESP 数组协议，其余走 inline（空格/Tab 分隔的行，
        //      如 "SET k v"，兼容 nc / curl 等经典用法）
        const char *p = c->querybuf + c->qb_pos;
        int rc;
        if (c->multibulklen > 0 || *p == '*') {
            rc = processMultibulkBuffer(c, store, zset);
        } else {
            rc = processInlineCommand(c, store, zset);
        }
        if ( rc == 1){
            // 成功，继续处理下一条
           continue;
        }else if(rc == 0){
            // 半包，跳出等待新数据
            break;
        }else if(rc == -1){
            // 协议错误，发送错误并断开/清空缓冲区
            send_error(c, "ERR protocol error");
            return -1;
        }else if(rc == -2){
            // 命令层要求断开（如 OOM），错误响应已在命令层发出，直接断开
            return -1;
        }
    }

    // 缓冲区压缩：把已解析完的无用数据移除
    // 用 sdsrange 把 [qb_pos, len-1] 的未处理数据整体移到头部（只 memmove + 改 len）
    if (c->qb_pos > 0){
        sdsrange(c->querybuf, (long)c->qb_pos, -1);
        c->qb_pos = 0;
    }
    return 1;
}

/*
 * 第二层：processMultibulkBuffer
 * 对应 Redis 的 processMultibulkBuffer
 *
 * 状态机三个步骤：
 *   1. 解析参数个数 *N\r\n
 *   2. 循环解析每个参数：$L\r\n + 内容\r\n
 *   3. 全部解析完 → 执行命令
 *
 * 返回值：
 *   1  - 成功解析并执行了一条命令
 *   0  - 数据不够（半包），需要更多数据
 *  -1  - 协议错误
 *  -2  - 命令层要求断开（OOM）
 */

int processMultibulkBuffer(Client *c, Storage *store, ZSet *zset) {
    const char *p;
    const char *next = NULL;
    int argc = 0;
    long bulklen = 0;
    int rc;

    /* ========== 状态一：解析参数个数 *N\r\n ========== */
    if (c->multibulklen == 0) {
        c->cmd_start = c->qb_pos;

        p = c->querybuf + c->qb_pos;
        rc = parse_multibulk_header(p, &next, &argc);
        if (rc == 0) return 0;
        if (rc < 0)  return -1;

        c->multibulklen = argc;
        c->qb_pos = next - c->querybuf;

        // 防御性清理 argv（如果之前残留）
        if (c->argv) {
            for (int i = 0; i < c->argc; i++) sdsfree(c->argv[i]);
            free(c->argv);
            c->argv = NULL;
            c->argv_max = 0;
            c->argc = 0;
        }
        c->argv = malloc(argc * sizeof(sds));
        if (!c->argv) {
            c->multibulklen = 0;
            return -1;
        }
        c->argv_max = argc;
        c->argc = 0;
    }

    /* ========== 状态二 + 状态三：循环解析每个参数 ========== */
    while (c->multibulklen > 0) {
        // 半包保护：游标已到缓冲区末尾（头部 *N 已解析、参数数据还没到齐）时，
        // 绝不能去读 '\0' 终止符再喂给 parse_bulk_header（那会被当成协议错误）——
        // 返回 0 等下次 read 补齐数据。TCP 不保证一次 read 收到完整命令。
        if ((size_t)c->qb_pos >= sdslen(c->querybuf)) return 0;
        p = c->querybuf + c->qb_pos;
        // 状态二：解析 $L\r\n
        if (c->bulklen == -1) {
            rc = parse_bulk_header(p, &next, &bulklen);
            if (rc == 0) return 0;
            if (rc < 0)  return -1;

            c->bulklen = bulklen;
            c->qb_pos = next - c->querybuf;
        }

        // 状态三：提取内容（sds，二进制安全）
        p = c->querybuf + c->qb_pos;
        sds content = NULL;
        rc = extract_bulk_content(p, c->bulklen, &content, &next, c->querybuf + sdslen(c->querybuf));
        if (rc == 0) return 0;
        if (rc < 0)  return -1;

        c->argv[c->argc++] = content;
        c->qb_pos = next - c->querybuf;
        c->bulklen = -1;
        c->multibulklen--;
    }

    /* ========== 所有参数提取完毕，执行命令 ========== */
    Command cmd;               // 薄包装：把 argc + argv 提交给命令层（借视图）
    cmd.argc = c->argc;
    cmd.argv = c->argv;
    int hrc = handle_command(c, &cmd, store, zset);

    // 释放 argv（每个元素是 sds）
    for (int i = 0; i < c->argc; i++) sdsfree(c->argv[i]);
    free(c->argv);
    c->argv = NULL;
    c->argc = 0;
    c->argv_max = 0;

    c->multibulklen = 0;
    c->bulklen = -1;
    // B8 修复：命令层要求断开（OOM）时返回 -2，错误响应已由命令层发出
    return (hrc == -1) ? -2 : 1;
}

/*
 * 第二层（inline 分支）：processInlineCommand
 * 解析一行空格/Tab 分隔的 inline 命令（如 "SET k v"，兼容 nc/curl 等
 * 不使用 RESP 协议的客户端）。以 \r\n 或 \n 为行结束。
 *
 * 返回值：
 *   1  - 成功执行了一条命令（或跳过一个空行）
 *   0  - 数据不够（半包），需要更多数据
 *  -1  - 协议错误（token 数超 1024）或 OOM，应断开
 */
int processInlineCommand(Client *c, Storage *store, ZSet *zset) {
    const char *p = c->querybuf + c->qb_pos;
    const char *buf_end = c->querybuf + sdslen(c->querybuf);

    // 找行结束符（\r\n 或裸 \n 都认）
    const char *line_end = memchr(p, '\n', (size_t)(buf_end - p));
    if (!line_end) return 0;                      // 半包

    size_t line_len = (size_t)(line_end - p);
    if (line_len > 0 && p[line_len - 1] == '\r') line_len--;   // 去掉 \r

    // 第一遍：数 token（连续空白当一个分隔；上限 1024，与 proto-max-argc 一致）
    int argc = 0;
    int in_token = 0;
    for (size_t i = 0; i < line_len; i++) {
        int blank = (p[i] == ' ' || p[i] == '\t');
        if (!blank && !in_token) {
            argc++;
            in_token = 1;
            if (argc > 1024) return -1;           // 参数个数超限，协议错误
        } else if (blank) {
            in_token = 0;
        }
    }

    // 空行：直接跳过不报错（Redis 同样忽略空行）
    if (argc == 0) {
        c->qb_pos += (size_t)(line_end - p) + 1;
        return 1;
    }

    sds *argv = malloc(argc * sizeof(sds));
    if (!argv) return -1;                         // OOM，断开

    // 第二遍：逐个构建 token（sds，二进制安全）
    int idx = 0;
    size_t i = 0;
    while (idx < argc && i < line_len) {
        while (i < line_len && (p[i] == ' ' || p[i] == '\t')) i++;
        size_t start = i;
        while (i < line_len && p[i] != ' ' && p[i] != '\t') i++;
        size_t tok_len = i - start;
        argv[idx] = sdsnewlen(p + start, tok_len);
        if (!argv[idx]) {
            for (int j = 0; j < idx; j++) sdsfree(argv[j]);
            free(argv);
            return -1;                            // OOM，断开
        }
        idx++;
    }

    Command cmd;
    cmd.argc = argc;
    cmd.argv = argv;
    int hrc = handle_command(c, &cmd, store, zset);

    // 释放 argv（无论命令成败都要释放，每个元素是 sds）
    for (int j = 0; j < argc; j++) sdsfree(argv[j]);
    free(argv);

    // 游标越过本行（含行结束符）
    c->qb_pos += (size_t)(line_end - p) + 1;
    return (hrc == -1) ? -2 : 1;
}
