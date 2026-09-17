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
int handle_command(int client_fd,Command *cmd,Storage *store,ZSet *zset)  //(文件描述符，解释器，存储器-里面有命令执行代码)
{
    //空命令
    if (cmd->argc<1){
        send_response(client_fd,"-ERR no command\r\n");
        return 1;
    }
    char *cmd_name =cmd->argv[0];    // 命令名，比如 "SET"（sds，当 C 字符串用）

    //PING 命令-检测两台设备之间网络通不通、延迟高不高
    if(strcasecmp(cmd_name,"PING")==0){
        send_response(client_fd,"+PONG\r\n");
        return 1;
    }

    // ZADD 命令-给有序集合zset添加数据，附带分数用来排序，重复数据会更新分数。
    if(strcasecmp(cmd_name ,"ZADD")==0){
        // 参数格式：ZADD key score member
        if(cmd->argc !=4){
            send_response(client_fd, "-ERR wrong number of arguments for 'ZADD'\r\n");
            return 1;
        }

        sds key=cmd->argv[1];     // ZSET 的键名（暂未实现多键，先忽略，直接使用全局 zset）
        (void)key;                  // 避免编译警告

        // 严格校验 score（B9 修复）：atof("abc") 会静默变 0.0；atof("nan") 得到 NaN，
        // 跳表所有含 NaN 的比较都为 false，节点插到头部，排序永久错乱
        char *score_end = NULL;
        errno = 0;
        double score = strtod(cmd->argv[2], &score_end);
        if (score_end == cmd->argv[2] ||   // 一个数字都没读到
            *score_end != '\0' ||          // 数字后跟了垃圾（如 "1.5abc"）
            errno == ERANGE ||             // 溢出 double（Redis 同样按错误处理）
            score != score) {              // NaN（与自身不等），必须拒绝
            send_response(client_fd, "-ERR value is not a valid float\r\n");
            return 1;
        }
        // 注：±inf 放行，与 Redis 行为一致（inf 可作合法边界分数）

        sds member=cmd->argv[3];
        zset_add(zset,member,score);
        send_response(client_fd, "+OK\r\n");
        return 1;
    }

    // ZREM 命令-从有序集合中删除指定成员
    if(strcasecmp(cmd_name,"ZREM")==0){
        // 参数：ZREM key member
        if(cmd->argc !=3){
            send_response(client_fd, "-ERR wrong number of arguments for 'ZREM'\r\n");
            return 1;
        }
        sds key=cmd->argv[1];
        (void)key;
        sds member=cmd->argv[2];
        int result=zset_rem(zset,member);
        char response[16];
        snprintf(response,sizeof(response),":%d\r\n",result);
        send_response(client_fd,response);
        return 1;
    }

    //ZSCORE命令 指定成员 member 对应的分数（score）
    if(strcasecmp(cmd_name,"ZSCORE")==0){
        if(cmd->argc!=3){
            send_response(client_fd, "-ERR wrong number of arguments for 'ZSCORE'\r\n");
            return 1;
        }
        sds key=cmd->argv[1];
        (void)key;
        sds member=cmd->argv[2];
        bool found =false;
        double score=zset_score(zset,member,&found);
        if(!found){
            send_response(client_fd, "$-1\r\n");   // nil
        }else{
            char buf[64];
            snprintf(buf,sizeof(buf),"%g",score);
            char response[128];
            snprintf(response,sizeof(response),"$%zu\r\n%s\r\n",strlen(buf),buf);
            send_response(client_fd,response);
        }
        return 1;
    }

    // ZRANGE 命令(按顺序返回，按索引范围返回有序集合成员)
    if(strcasecmp(cmd_name,"ZRANGE")==0){
        // 参数：ZRANGE key start stop
        if(cmd->argc !=4){
            send_response(client_fd, "-ERR wrong number of arguments for 'ZRANGE'\r\n");
            return 1;
        }

        char *key=cmd->argv[1];
        (void)key;

        // 严格校验索引（B9 修复）：atoi 对垃圾输入不报错
        long start_l, stop_l;
        if(!parse_strict_long(cmd->argv[2], &start_l) ||
           !parse_strict_long(cmd->argv[3], &stop_l)){
            send_response(client_fd, "-ERR value is not an integer or out of range\r\n");
            return 1;
        }
        int start =(int)start_l;
        int stop =(int)stop_l;

        sds *member =zset_range(zset,start,stop);
        if(!member){
            // B8 修复：分配失败。响应一个字节都还没发，无协议失步，
            // 但 OOM 下不应继续服务，报错并断开
            send_response(client_fd, "-ERR out of memory\r\n");
            return -1;
        }

        // 构造 RESP 数组回复
        // 先计算成员个数（zset_range 保证以 NULL 结尾）
        int count =0;
        while(member[count]) count++;

        // 发送数组头
        char header[32];
        snprintf(header,sizeof(header),"*%d\r\n",count);
        send_response(client_fd,header);

        // 发送每个成员（批量字符串，二进制安全：sdslen 取长 + 按长度发，避免 %s 截断）
        for(int i=0;i<count;i++){
            size_t len = sdslen(member[i]);
            char hdr[32];
            int hl = snprintf(hdr, sizeof(hdr), "$%zu\r\n", len);
            send_response_len(client_fd, hdr, (size_t)hl);   // header（纯文本长度行）
            send_response_len(client_fd, member[i], len);    // 数据（二进制安全）
            send_response_len(client_fd, "\r\n", 2);         // trailer
            sdsfree(member[i]);
        }
        free(member);
        return 1;
    }

    // 比较命令名，忽略大小写（SET / set / Set 都行）
    //SET命令
    if(strcasecmp(cmd_name,"SET")==0){
        if(cmd->argc !=3){
            send_response(client_fd,"-ERR wrong number of arguments for 'SET'\r\n");
            return 1;
        }
        storage_set(store,cmd->argv[1],cmd->argv[2]);
        send_response(client_fd,"+OK\r\n");   // 惯例大写（redis-cli 会原样显示）
        return 1;
    }

    //GET 分支
    else if(strcasecmp(cmd_name,"GET")==0){
        if(cmd->argc !=2){
            send_response(client_fd,"-ERR wrong number of arguments for 'GET'\r\n");
            return 1;
        }
        sds val= storage_get(store,cmd->argv[1]);

        if(val){
            // 用 sdslen 取长度 + send_response_len 按长度发送 → 二进制安全（值可含 '\0'）
            // RESP 批量字符串 = $<len>\r\n<len字节数据>\r\n，三段发送，避免 %s 在 '\0' 截断
            size_t len = sdslen(val);
            char hdr[32];
            int hl = snprintf(hdr,sizeof(hdr),"$%zu\r\n",len);
            send_response_len(client_fd, hdr, (size_t)hl);   // header（长度总是文本，无 \0）
            send_response_len(client_fd, val, len);          // 数据（按 sdslen，二进制安全）
            send_response_len(client_fd, "\r\n", 2);         // trailer
        }
        else {
            send_response(client_fd, "$-1\r\n");
        }
        return 1;
    }

    //DEL 分支
    else if(strcasecmp(cmd_name,"DEL")==0){
        if(cmd->argc !=2){
            send_response(client_fd,"-ERR wrong number of arguments for 'DEL'\r\n");
            return 1;
        }
        int deleted =storage_del(store,cmd->argv[1]);
        char response[16];
        snprintf(response,sizeof(response),":%d\r\n",deleted);
        send_response(client_fd, response);
        return 1;
    }
    //未知命令
    else{
        send_response(client_fd, "-ERR unknown command\r\n");
        return 1;
    }
}
