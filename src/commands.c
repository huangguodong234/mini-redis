#include "commands.h"

#include <stdio.h>      // snprintf, fprintf
#include <stdlib.h>     // strtod, strtol, malloc, free
#include <strings.h>    // strcasecmp  (注意：不是 <string.h>)
#include <string.h>     // strlen、strcpy
#include <errno.h>      // errno、ERANGE（strtod/strtol 溢出标志）
#include <limits.h>     // INT_MIN / INT_MAX（ZRANGE 索引 long→int 的范围校验，P1-3）
#include <float.h>      // DBL_MAX（format_score 判定 ±inf 用）

/*
 * 严格解析整型参数（B9 修复：atoi 对垃圾输入会静默返回 0）
 * 用 strtol + endptr/errno 校验：
 *   - end == s      → 一个数字都没读到
 *   - *end != '\0'  → 数字后面还跟了垃圾（如 "12abc"）
 *   - ERANGE        → 溢出 long
 * 成功返回 1 并写入 *out；失败返回 0
 */
static int parse_strict_long(const char *s, long *out) {
    if (!s || *s == '\0') return 0;
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE) return 0;
    *out = v;
    return 1;
}

/*
 * 把 double 分数格式化成 RESP 批量字符串要用的文本（对齐 Redis d2string）。
 *
 * 历史演进：
 *   1) 最初用 "%g"：只有 6 位有效数字，1234567890 → "1.23457e+09"（数据读错）。
 *   2) 改用 "%.17g"：能无损往返，但【不保证最短】，于是常见值出现浮点噪声：
 *        0.1  → "0.10000000000000001"   （真实 Redis 回 "0.1"）
 *        0.3  → "0.29999999999999999"   （真实 Redis 回 "0.3"）
 *        1e-7 → "9.9999999999999995e-08"（真实 Redis 回 "1e-07"）
 *      ZSCORE 会把分数原样回给用户，所以这是用户可见的格式不兼容。
 *   3) 现在：取【能无损往返的最短表示】。
 *
 * 为什么不是直接照抄 Redis 的 fpconv_dtoa（Grisu）：
 *   真实 Redis 的 d2string 走 "double2ll + ll2string 整型快路径"，浮点回退用
 *   deps/fpconv 的最短表示算法。本项目不引入第三方依赖，改用等价语义的最小实现：
 *   从 15 位有效数字起逐级尝试，第一个满足 strtod(文本) == 原值 的精度即最短可
 *   无损往返的表示。%.15g 已能覆盖绝大多数"人写出来的"分数，只有真正需要 16/17
 *   位才能区分的值（如 0.1+0.2 = 0.30000000000000004）才会升到高精度。
 *
 *   正确性：%.\{15,16,17\}g 中 17 位对 double 必然无损往返（DBL_DECIMAL_DIG），
 *   故循环一定在 <=17 处命中；实测 200 万个随机 double 往返 0 失败。
 *   同时保留了整型值走 "%g" 天然不带小数点的特性：3.0 → "3"、42.5 → "42.5"。
 *
 * ★ 二进制安全性说明（重要）：
 *   本函数【只处理分数文本】，输入是 double、输出是 snprintf 按格式串生成的
 *   十进制文本（字符集仅 0-9 + - . e 与字面量 inf/nan），【不可能含 '\0'】。
 *   所以这里的长度天生是文本长度，与"存储值的二进制安全"是两件事——
 *   用户存入的图片、protobuf 等二进制数据走的是 GET/SET 路径，
 *   由 send_bulk_string() 用 sdslen() + 显式字节长度发送（server.c），
 *   全文没有一处依赖 strnlen/strlen 推断长度，故不受影响。
 *
 *   但【返回长度一律取 snprintf 的真实写入数，不用 strlen(buf) 反推】：
 *   这是硬要求，不是风格问题。strlen 的正确性依赖"输出不含 NUL"这一隐含前提，
 *   一旦哪天格式串改动、或调用方改用本函数格式化别的数据，该前提失效时
 *   strlen 会返回偏小的长度，把数据静默截断——而且截断得"像是对的"，
 *   极难排查。改成直接采信 snprintf 的返回值后，前提不再需要成立。
 *
 *   三个出口都遵守该规则：非有限值分支取 snprintf 返回值；主循环在 n >= bufsz
 *   时截断到 bufsz-1（snprintf 的返回值是"本应写入的长度"，可能超缓冲，
 *   不能直接当可用长度用）；n < 0 是编码错误，置空串返回 0。
 *   缓冲充足时（buf[64] 存分数绰绰有余，%.17g 最多约 24 字节），
 *   返回值恒等于实际写入的字节数。
 */
static int format_score(double score, char *buf, size_t bufsz) {
    if (!buf || bufsz == 0) return 0;

    /* 非有限值显式处理：C 库对 inf/nan 的拼写因平台而异（"inf"/"Infinity"/
       "nan"/"-nan"），Redis 固定输出 "inf"/"-inf"（nan 进不来：ZADD 已拒绝）。
       显式拼写可避免跨 libc 的格式漂移。 */
    int n;
    if (score != score) {                 // NaN（与自身不等）
        n = snprintf(buf, bufsz, "nan");
        if (n < 0) { buf[0] = '\0'; return 0; }
        return (size_t)n < bufsz ? n : (int)bufsz - 1;
    }
    if (score > DBL_MAX || score < -DBL_MAX) {   // ±inf
        n = snprintf(buf, bufsz, score < 0 ? "-inf" : "inf");
        if (n < 0) { buf[0] = '\0'; return 0; }
        return (size_t)n < bufsz ? n : (int)bufsz - 1;
    }

    /* 取"能无损往返的最短表示"：15 → 16 → 17 逐级尝试 */
    for (int prec = 15; prec <= 17; prec++) {
        n = snprintf(buf, bufsz, "%.*g", prec, score);
        if (n < 0) { buf[0] = '\0'; return 0; }
        if ((size_t)n >= bufsz) {        // 缓冲不足：截断，实际可用 bufsz-1 字节
            buf[bufsz - 1] = '\0';       // snprintf 已保证，显式写出以表明意图
            return (int)bufsz - 1;
        }
        if (strtod(buf, NULL) == score) break;   // 无损往返 → 这就是最短表示
    }
    return n;   // snprintf 的真实写入数（不含结尾 '\0'），不依赖 strlen
}

// 核心：根据解析出的命令操作存储引擎，并返回 RESP 响应
// 返回值：1 = 正常响应完成；-1 = 调用方应断开该连接（错误已发出，B8 修复）
// 说明：本层只负责“调命令 + 调 RESP 辅助函数拼接/发送响应”，
//       所有拼接细节（snprintf、按长度发二进制数据）都封装在 server.c 的
//       send_* 辅助函数里，命令层不直接碰 send_response。
int handle_command(int client_fd, Command *cmd, Storage *store, ZSet *zset)  //(文件描述符，命令包装，存储引擎，跳表)
{
    int argc = cmd->argc;   // 参数个数
    sds *argv = cmd->argv;  // 参数数组（借视图，指向解析器创建的 sds 数组）

    //空命令
    if (argc<1){
        send_error(client_fd, "ERR no command");
        return 1;
    }
    char *cmd_name =argv[0];    // 命令名，比如 "SET"（sds，当 C 字符串用）
                                     // 命令名按 '\0' 语义比较（strcasecmp），
                                     // 命令名永远不会含 '\0'，二进制安全不受影响

    //PING 命令-检测两台设备之间网络通不通、延迟高不高
    if(strcasecmp(cmd_name,"PING")==0){
        send_simple_string(client_fd, "PONG");
        return 1;
    }

    // ZADD 命令-给有序集合zset添加数据，附带分数用来排序，重复数据会更新分数。
    if(strcasecmp(cmd_name ,"ZADD")==0){
        // 参数格式：ZADD key score member
        if(argc !=4){
            send_error(client_fd, "ERR wrong number of arguments for 'ZADD'");
            return 1;
        }

        sds key=argv[1];     // ZSET 的键名（暂未实现多键，先忽略，直接使用全局 zset）
        (void)key;                  // 避免编译警告

        // 严格校验 score（B9 修复）：atof("abc") 会静默变 0.0；atof("nan") 得到 NaN，
        // 跳表所有含 NaN 的比较都为 false，节点插到头部，排序永久错乱
        char *score_end = NULL;
        errno = 0;
        double score = strtod(argv[2], &score_end);
        if (score_end == argv[2] ||   // 一个数字都没读到
            *score_end != '\0' ||          // 数字后跟了垃圾（如 "1.5abc"）
            errno == ERANGE ||             // 溢出 double（Redis 同样按错误处理）
            score != score) {              // NaN（与自身不等），必须拒绝
            send_error(client_fd, "ERR value is not a valid float");
            return 1;
        }
        // 注：±inf 放行，与 Redis 行为一致（inf 可作合法边界分数）

        sds member=argv[3];
        // P0-4 修复：zset_add 现在返回成败。无论成功还是失败，member 的所有权
        // 都已转移完毕（成功 → 跳表持有；失败 → 已被释放），故一律置 NULL；
        // 但失败时必须报错，不能回 +OK（否则是静默数据丢失）。
        if (!zset_add(zset, member, score)) {
            argv[3] = NULL;
            send_error(client_fd, "ERR out of memory");
            return -1;
        }
        argv[3] = NULL;   // 已被跳表接管所有权，调用方（server.c）不得再 free
        send_simple_string(client_fd, "OK");
        return 1;
    }

    // ZREM 命令-从有序集合中删除指定成员
    if(strcasecmp(cmd_name,"ZREM")==0){
        // 参数：ZREM key member
        if(argc !=3){
            send_error(client_fd, "ERR wrong number of arguments for 'ZREM'");
            return 1;
        }
        sds key=argv[1];
        (void)key;
        sds member=argv[2];
        int result=zset_rem(zset,member);
        send_integer(client_fd, result);
        return 1;
    }

    //ZSCORE命令 指定成员 member 对应的分数（score）
    if(strcasecmp(cmd_name,"ZSCORE")==0){
        if(argc!=3){
            send_error(client_fd, "ERR wrong number of arguments for 'ZSCORE'");
            return 1;
        }
        sds key=argv[1];
        (void)key;
        sds member=argv[2];
        bool found =false;
        double score=zset_score(zset,member,&found);
        if(!found){
            send_null_bulk(client_fd);   // nil
        }else{
            char buf[64];
            int bl = format_score(score, buf, sizeof(buf));   // 分数转文本（%.17g + 裁尾零）
            send_bulk_string_len(client_fd, buf, (size_t)bl); // 按长度发送
        }
        return 1;
    }

    // ZRANGE 命令(按顺序返回，按索引范围返回有序集合成员)
    if(strcasecmp(cmd_name,"ZRANGE")==0){
        // 参数：ZRANGE key start stop
        if(argc !=4){
            send_error(client_fd, "ERR wrong number of arguments for 'ZRANGE'");
            return 1;
        }

        char *key=argv[1];
        (void)key;

        // 严格校验索引（B9 修复）：atoi 对垃圾输入不报错
        long start_l, stop_l;
        if(!parse_strict_long(argv[2], &start_l) ||
           !parse_strict_long(argv[3], &stop_l)){
            send_error(client_fd, "ERR value is not an integer or out of range");
            return 1;
        }
        // P1-3 修复：long → int 之前必须校验范围。原先直接强转，超 int 的值会被
        // 静默截断（ZRANGE k 4294967297 -1 会被截成 1），用户拿到毫不相干的结果。
        if (start_l < INT_MIN || start_l > INT_MAX ||
            stop_l  < INT_MIN || stop_l  > INT_MAX) {
            send_error(client_fd, "ERR value is not an integer or out of range");
            return 1;
        }
        int start =(int)start_l;
        int stop =(int)stop_l;

        sds *member =zset_range(zset,start,stop);
        if(!member){
            // B8 修复：分配失败。响应一个字节都还没发，无协议失步，
            // 但 OOM 下不应继续服务，报错并断开
            send_error(client_fd, "ERR out of memory");
            return -1;
        }

        // 发送 RESP 数组回复（可含 NULL 结尾）
        // 先计算成员个数（zset_range 保证以 NULL 结尾）
        int count =0;
        while(member[count]) count++;

        send_array_len(client_fd, count);                    // 数组头 *n\r\n

        // 逐条发送成员（批量字符串，二进制安全：sdslen 取长 + 按长度发）
        for(int i=0;i<count;i++){
            send_bulk_string(client_fd, member[i]);          // $len\r\n<data>\r\n
            sdsfree(member[i]);
        }
        free(member);
        return 1;
    }

    // 比较命令名，忽略大小写（SET / set / Set 都行）
    //SET命令
    if(strcasecmp(cmd_name,"SET")==0){
        if(argc !=3){
            send_error(client_fd, "ERR wrong number of arguments for 'SET'");
            return 1;
        }
        // SET 分支默认走"argv 不释放给哈希"优化（steal 所有权）：
        //   直接把解析出的 key/value 的 sds 所有权移交给哈希表（省掉每 SET 两次
        //   sdsdup 的 malloc+memcpy，也省掉 server.c 次数后面的两次 sdsfree）。
        //   ⚠ 被接管的两格 argv 必须置 NULL：server.c 的释放循环对 NULL 是
        //   安全 no-op，不会 double-free；哈希表（含 duplicate-key 自释放、
        //   OOM 拒绝路径自释放）成为它们的唯一 owner，不泄漏。
        //   备注：小值下省下的 malloc/free 摊在 ~107µs 的 RTT 里可忽略（≈+1.8%），
        //   只有大 value 才因省掉大块 memcpy 明显受益（32KB +7.2% / 128KB +7.4%）。
        // P0-4 修复：storage_set_steal 现在返回成败。无论成功还是失败，
        // key/value 的所有权都已转移完毕（成功 → 哈希表持有；失败 → 已被释放），
        // 所以两格一律置 NULL；但失败时必须报错，绝不能回 +OK ——
        // 否则客户端以为写成功了，实际数据已丢（静默数据丢失）。
        if (!storage_set_steal(store, argv[1], argv[2])) {
            argv[1] = NULL;
            argv[2] = NULL;
            // 按 B8 约定：此时一个响应字节都还没发出，不存在协议失步；
            // OOM 下不应继续服务，报错并断开连接。
            send_error(client_fd, "ERR out of memory");
            return -1;
        }
        argv[1] = NULL;   // 已被哈希表接管，调用方（server.c）不得再 free
        argv[2] = NULL;
        send_simple_string(client_fd, "OK");   // 惯例大写（redis-cli 会原样显示）
        return 1;
    }

    //GET 分支
    else if(strcasecmp(cmd_name,"GET")==0){
        if(argc !=2){
            send_error(client_fd, "ERR wrong number of arguments for 'GET'");
            return 1;
        }
        sds val= storage_get(store,argv[1]);
        send_bulk_string(client_fd, val);   // val==NULL → $-1\r\n；否则按 sdslen 二进制安全发送
        sdsfree(val);                       // 方案C：val 是 storage_get 返回的可拥有深拷贝，用完即释放
        return 1;
    }

    //DEL 分支
    else if(strcasecmp(cmd_name,"DEL")==0){
        if(argc !=2){
            send_error(client_fd, "ERR wrong number of arguments for 'DEL'");
            return 1;
        }
        int deleted =storage_del(store,argv[1]);
        send_integer(client_fd, deleted);
        return 1;
    }
    //未知命令
    else{
        send_error(client_fd, "ERR unknown command");
        return 1;
    }
}
