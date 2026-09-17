#ifndef SERVER_H
#define SERVER_H

#include "anet.h"
#include "storage.h"
#include "zset.h"
#include "sds.h"
#include <stddef.h>

// 客户端结构体：封装每个连接的状态
typedef struct {
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
} Client;

// 创建客户端（分配并初始化）
Client *create_client(int fd);

// 释放客户端（关闭连接并释放内存）
void free_client(Client *c);

// 发送响应给客户端（按明确字节长度发送，二进制安全，数据可含 '\0'）
// 底层唯一执行 write() 循环的地方；上层辅助函数全部经由它把数据写出去。
void send_response_len(int client_fd, const void *data, size_t len);

// 发送响应给客户端（RESP 文本，按 '\0' 结尾的 C 字符串发送）
void send_response(int client_fd, const char *resp);

// ==================== RESP 编码 / 发送辅助函数 ====================
// 命令层（commands.c）只允许调用下面这些高层 API 来拼接并发送响应，
// 禁止直接碰 snprintf / send_response_len 的原始拼接细节——所有“拼接+发送”
// 逻辑都封装在 server.c 里，全部经由 send_response / send_response_len 发出。
// 每个函数只负责一“种” RESP 数据类型，职责单一、可复用。

// 简单字符串回复  +<s>\r\n   （s 例如 "OK"，不得含 \r\n）
void send_simple_string(int client_fd, const char *s);

// 错误回复  -<msg>\r\n   （msg 通常以 "ERR " 开头，由调用者传入）
void send_error(int client_fd, const char *msg);

// 整数回复  :<n>\r\n   （n 可正可负）
void send_integer(int client_fd, long n);

// 批量字符串回复  $<len>\r\n<len字节数据>\r\n
// 按明确字节长度发送，二进制安全（数据可含 '\0'）。
void send_bulk_string_len(int client_fd, const void *data, size_t len);

// 批量字符串回复（sds 版）：val == NULL 时发 $-1\r\n（nil），否则发完整批量字符串。
void send_bulk_string(int client_fd, sds val);

// 空批量字符串回复  $-1\r\n（比 send_bulk_string(fd, NULL) 更直白的写法）
void send_null_bulk(int client_fd);

// 数组头  *<n>\r\n   （数组中元素个数，随后逐条发元素）
void send_array_len(int client_fd, long n);

// 创建服务器 socket（已绑定并监听）
int create_server_socket(int port, int backlog);

/*
*第一层：检查缓存区，从socrek读数据，加入缓存区，调用processInputBuffer 
*对应 redis readQueryFromClient
*/
void readQueryFromClient(int client_fd, Storage *store, ZSet *zset);

/*
*第二层：调用processMultibulkBuffer，缩容缓存区
*对应Redis processInputBuffer
*/
int processInputBuffer(Client *c,Storage *store, ZSet *zset);

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