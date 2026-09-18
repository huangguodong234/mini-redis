#include "commands.h"

#include <stdio.h>      // snprintf, fprintf
#include <stdlib.h>     // strtod, strtol, malloc, free
#include <strings.h>    // strcasecmp  (注意：不是 <string.h>)
#include <string.h>     // strlen、strcpy
#include <errno.h>      // errno、ERANGE（strtod/strtol 溢出标志）

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

// 核心：根据解析出的命令操作存储引擎，并返回 RESP 响应
// 返回值：1 = 正常响应完成；-1 = 调用方应断开该连接（错误已发出，B8 修复）
// 说明：本层只负责“调命令 + 调 RESP 辅助函数拼接/发送响应”，
//       所有拼接细节（snprintf、按长度发二进制数据）都封装在 server.c 的
//       send_* 辅助函数里，命令层不直接碰 send_response。
int handle_command(int client_fd,int argc,sds *argv,Storage *store,ZSet *zset)  //(文件描述符，参数个数，参数数组，存储引擎，跳表)
{
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
        zset_add(zset,member,score);
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
            int bl = snprintf(buf,sizeof(buf),"%g",score);   // 分数转文本
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
        storage_set_steal(store, argv[1], argv[2]);
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
