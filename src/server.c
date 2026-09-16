/* 我的mini-redis和Redis源码处理TCP字节流的区别：
 *
 * 1. 我的mini-redis的处理方式：
 *    直接设一个固定的缓冲区大小，用 read() 读取。每次 read() 后，
 *    直接把接收到的数据交给解析器。
 *
 *    这会造成两个问题：
 *
 *    (1) 粘包问题：
 *        当几条相邻的字节流都比较短时，一次 read() 可能把它们全部接收。
 *        但我的解析器每次只处理一条命令，处理完就返回，剩余数据被丢弃。
 *        后果：后面的命令丢失，客户端收不到回复。
 *
 *    (2) 半包问题：
 *        当字节流很长时，一次 read() 读不完。解析器收到的字符串被截断，
 *        末尾不是 \r\n 结束，格式不合法，解析器返回 NULL。
 *        后果：命令执行失败，且已读取的半截数据被下次 read() 覆盖，
 *        永远拼不出完整命令。
 *
 * 2. Redis的处理方式：
 *    给每个客户端设一个动态缓冲区（SDS），把读到的数据追加到缓冲区尾部。
 *    设计一个游标（qb_pos），标记当前解析进度。
 *
 *    核心机制：
 *    - 解析过程中边解析边移动游标。当发现数据不够（缺 \r\n 或字符串
 *      未接收完整），游标停在当前位置，函数直接返回。
 *    - 下次新数据到达，追加到缓冲区尾部，从上次游标位置继续解析——
 *      就像游戏存档，不需要重打第一关。这就是"状态机"的核心思想：
 *      解析器本身无状态，状态全部存在游标和缓冲区里。
 *    - 解析完一条命令后，游标已指向下一条命令的起始位置。while 循环
 *      检查缓冲区里还有未处理数据，立刻进入下一轮解析，无需再次 read()。
 *      多条命令在一次 processInputBuffer 调用中全部处理完——这就是
 *      解决粘包的关键。
 *    - 当游标前方堆积了大量已解析完的无用数据时，Redis 会用 sdsrange
 *      把未处理部分移到缓冲区头部，释放前面的空间。SDS 的 sdsrange 是
 *      O(1) 更新 len 字段，不需要重新分配内存。这个优化在长连接场景下
 *      很重要。
 *
 *    缓冲区的优势：
 *    - 自动扩容，不受固定大小限制。SDS 的空间预分配策略保证扩容
 *      不会频繁触发 realloc。
 *    - 记录 len 字段，O(1) 获取缓冲区当前长度。在单条数据特别大时，
 *      不需要从数据头遍历到数据尾来判断缓冲区状态。
 */


#define _POSIX_C_SOURCE 200809L
#include <stdio.h>      // printf, perror（自定义错误提示字符串）-输出失败原因
#include <stdlib.h>     // exit（）
#include <unistd.h>     // read（）, write（）, close（）
#include <sys/socket.h> // socket（）, bind（）, listen（）, accept（）
#include <netinet/in.h> // sockaddr_in 结构体{sin_family：地址家族，IPv4 填 AF_INET   sin_port：端口号，必须用 htons() 转成网络字节序 sin_addr.s_addr：IP 地址，INADDR_ANY 表示监听本机所有网络接口}
#include <string.h>    // strlen, memset
#include <errno.h>     // errno、EINTR（B7 修复：区分"被信号打断"和"客户端断开"）

#include "server.h"
#include "protocol.h"   
#include "commands.h"   // handle_command

// 注意：PORT / BACKLOG 统一定义在 main.c（B6 修复：本文件不再用宏，
// create_server_socket 严格使用传入的 port / backlog 参数）

// 创建客户端结构体
Client *create_client(int fd){
    Client *c=malloc(sizeof(Client));
    if(!c) return NULL;

    c->fd=fd;
    c->qb_pos=0;
    c->qb_len=0;
    c->multibulklen = 0;
    c->bulklen = -1;
    c->cmd_start = 0;
    c->argv = NULL;
    c->argc = 0;
    c->argv_max = 0;
    memset(c->querybuf,0,BUFFER_SIZE); //缓冲区清空

    return c;
}

// 释放客户端（关闭连接并释放内存）
void free_client(Client *c) {
    if (!c) return;
    if (c->argv) {
        for (int i = 0; i < c->argc; i++) free(c->argv[i]);
        free(c->argv);
    }
    close(c->fd);
    free(c);
}

// 发送响应给客户端（我们直接发送 RESP 格式字符串）
// 循环 write，处理部分写入和 EINTR（信号打断），保证整条响应发完，RESP 协议不截断
void send_response(int client_fd,const char*resp){
    if(!resp) return;
    size_t len = strlen(resp);
    const char *p = resp;
    while (len > 0) {
        ssize_t n = write(client_fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;      // 被信号打断，重试
            break;                             // 其余写错误（如对端关闭），放弃本次
        }
        p += n;
        len -= (size_t)n;
    }
}

/*
*服务端流程：创建socket(int domain（地址族）,int type（套接字类型）,int protocol（传输协议）我们填 0-TCP)，绑定端口(bing(套接字描述符,地址结构体指针, 地址结构体的大小)),
*开始监听（listen（套接字描述符, 等待队列最大长度））接受客户端的连接（accept(监听的套接字描述符，客户端地址结构体指针, 客户端地址长度变量的指针)）,
*读写数据（read（文件描述符，存放数据的数组，存放的数据的最大字节数-1） write（文件描述符，要发送的数据的数组，要发送的字节数）），挂断（close(要挂断的描述符))
*/

/*创建服务器 socket（已绑定并监听）*/
int create_server_socket(int port, int backlog){
    int server_fd;     //server_fd 套接字描述符
    struct sockaddr_in server_addr;   //服务器地址
    
    //创建socker
    server_fd = socket(AF_INET, SOCK_STREAM, 0);   
    if (server_fd == -1) {
        perror("socket 创建失败");
        exit(EXIT_FAILURE);    //EXIT_FAILURE  表示程序异常退出
    }
    printf("socket 创建成功\n");

    //解决重启TIME_WAIT端口占用
    anetSetReuseAddr(server_fd);

    //  绑定地址和端口
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons((uint16_t)port);   // B6 修复：用参数，不用宏

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind 失败");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("绑定端口 %d 成功\n", port);

    //开始监听
    if (listen(server_fd, backlog) == -1) {          // B6 修复：用参数
        perror("listen 失败");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("服务器正在监听 0.0.0.0:%d ...\n", port);  // INADDR_ANY 如实写成 0.0.0.0

    return server_fd;
}

/*
 * 第一层：readQueryFromClient
 * 对应 Redis 的 readQueryFromClient
 *
 * 职责：
 *   1. 创建 Client 结构体，封装 fd、缓冲区、游标
 *   2. 循环从 socket 读取数据，追加到 querybuf 尾部
 *   3. 每次读取后调用 processInputBuffer（第二层）尝试解析命令
 *   4. 客户端断开或出错时退出循环，释放 Client
 */
void readQueryFromClient(int client_fd, Storage *store, ZSet *zset){
    // 创建客户端状态结构体
    Client *c = create_client(client_fd);
    if (!c) {
        fprintf(stderr, "创建客户端失败\n");
        close(client_fd);
        return;
    }

    int nread = 0;   // 初始化：若首轮循环因缓冲区满（avail==0）直接跳出，nread 也不可未定义
    // 2. 循环读取数据
    while(1){
        // 计算剩余空间（预留1字节给'\0'）
        size_t avail = BUFFER_SIZE - c->qb_len - 1;
        if (avail == 0) {
            // 缓冲区满，此处简单断开（后续可改为扩容）
            fprintf(stderr, "客户端 %d 缓冲区已满\n", c->fd);
            break;
        }

        nread=read(c->fd,c->querybuf + c->qb_len,avail);
        if(nread<0){
            // B7 修复：被信号打断（SIGINT/调试器 attach 等）nread<0,不是客户端断开，
            // 继续读即可；真正的读错误才退出
            if(errno==EINTR){
                continue;
            }
            break;   // 出错
        }
        if(nread==0){
            break;   // 客户端断开
        }

        // 更新缓冲区长度并追加结束符
        c->qb_len +=nread;
        c->querybuf[c->qb_len]='\0';

        // 3. 调用第二层解析（处理粘包/半包）
        int prc = processInputBuffer(c, store, zset);
        if (prc == -1) {
            fprintf(stderr, "客户端 %d 协议错误，断开连接\n", c->fd);
            break;
        }
    }

    if (nread==0){
       printf("客户端断开\n"); 
    }else if(nread<0){
        perror("read");
    }

    // 4. 释放客户端
    free_client(c);
}

/*
 * 第二层：processInputBuffer
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
 */
int processInputBuffer(Client *c,Storage *store, ZSet *zset){
    // 循环解析：只要游标后面还有数据，就尝试解析命令（处理粘包）
    while(c->qb_pos < c->qb_len){
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
            send_response(c->fd, "-ERR protocol error\r\n");
            return -1;
        }else if(rc == -2){
            // 命令层要求断开（如 OOM），错误响应已在命令层发出，直接断开
            return -1;
        }
    }

    // 缓冲区压缩：把已解析完的无用数据移除
    if (c->qb_pos > 0){
        if(c->qb_pos < c->qb_len){
            // 还有未处理的数据，移到缓冲区头部
            memmove(c->querybuf, c->querybuf + c->qb_pos, c->qb_len - c->qb_pos);
        }
        c->qb_len -= c->qb_pos;
        c->qb_pos = 0;
    }
    return 1;
}

/*
 * 第三层：processMultibulkBuffer
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
            for (int i = 0; i < c->argc; i++) free(c->argv[i]);
            free(c->argv);
            c->argv = NULL;
            c->argv_max = 0;
            c->argc = 0;
        }
        c->argv = malloc(argc * sizeof(char *));
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
        if (c->qb_pos >= c->qb_len) return 0;
        p = c->querybuf + c->qb_pos;
        // 状态二：解析 $L\r\n
        if (c->bulklen == -1) {
            rc = parse_bulk_header(p, &next, &bulklen);
            if (rc == 0) return 0;
            if (rc < 0)  return -1;

            c->bulklen = bulklen;
            c->qb_pos = next - c->querybuf;
        }

        // 状态三：提取内容
        p = c->querybuf + c->qb_pos;
        char *content = NULL;
        rc = extract_bulk_content(p, c->bulklen, &content, &next, c->querybuf + c->qb_len);
        if (rc == 0) return 0;
        if (rc < 0)  return -1;

        c->argv[c->argc++] = content;
        c->qb_pos = next - c->querybuf;
        c->bulklen = -1;
        c->multibulklen--;
    }

    /* ========== 所有参数提取完毕，执行命令 ========== */
    Command cmd;
    cmd.argc = c->argc;
    cmd.argv = c->argv;
    int hrc = handle_command(c->fd, &cmd, store, zset);

    // 释放 argv
    for (int i = 0; i < c->argc; i++) free(c->argv[i]);
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
 * 第三层（inline 分支）：processInlineCommand
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
    const char *buf_end = c->querybuf + c->qb_len;

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

    char **argv = malloc(argc * sizeof(char *));
    if (!argv) return -1;                         // OOM，断开

    // 第二遍：逐个复制 token
    int idx = 0;
    size_t i = 0;
    while (idx < argc && i < line_len) {
        while (i < line_len && (p[i] == ' ' || p[i] == '\t')) i++;
        size_t start = i;
        while (i < line_len && p[i] != ' ' && p[i] != '\t') i++;
        size_t tok_len = i - start;
        char *tok = malloc(tok_len + 1);
        if (!tok) {
            for (int j = 0; j < idx; j++) free(argv[j]);
            free(argv);
            return -1;                            // OOM，断开
        }
        memcpy(tok, p + start, tok_len);
        tok[tok_len] = '\0';
        argv[idx++] = tok;
    }

    Command cmd;
    cmd.argc = argc;
    cmd.argv = argv;
    int hrc = handle_command(c->fd, &cmd, store, zset);

    // 释放 argv（无论命令成败都要释放）
    for (int j = 0; j < argc; j++) free(argv[j]);
    free(argv);

    // 游标越过本行（含行结束符）
    c->qb_pos += (size_t)(line_end - p) + 1;
    return (hrc == -1) ? -2 : 1;
}
