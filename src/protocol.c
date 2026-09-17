//编写解释器代码   传进来的字符串   *2\r\n$3\r\nSET\r\n$3\r\nGET\r\n

#include <stdio.h>   
#include <stdlib.h>    //malloc() free()  atoi()-把字符串转成整数返回
#include <string.h>    //strlen() strncpy() strcmp()  strcpy()
#include <ctype.h>     //isdigit()  判断字符是否是数字
#include "protocol.h"    //我们自己的头文件
#include "sds.h"         // extract_bulk_content 要返回 sds

// ===== 公开的辅助函数，供 processMultibulkBuffer 调用 =====

/*
 * 解析 *N\r\n
 * 成功返回 1，*next 指向 \r\n 之后的位置，*argc 被填充
 * 半包返回 0
 * 协议错误返回 -1
 */
int parse_multibulk_header(const char *p, const char **next, int *argc) {
    if (*p != '*') return -1;                     // 协议错误
    p++;                                          // 跳过 '*'
    const char *cr = strchr(p, '\r');             // 找 \r
    if (!cr) return 0;                            // 半包，数据不全
    if (*(cr + 1) != '\n') return 0;              // 也是半包，没有完整的 \r\n

    // 解析数字（先判上限再乘 10：n 最大 1024，n*10+9 ≤ 10249，不会 int 溢出）
    int n = 0;
    for (const char *q = p; q < cr; q++) {
        if (*q < '0' || *q > '9') return -1;      // 非法字符，协议错误
        if (n > 1024) return -1;                  // 提前退出：位数太多，防溢出
        n = n * 10 + (*q - '0');
    }
    if (n <= 0 || n > 1024) return -1;              // 参数个数不合理

    *argc = n;
    *next = cr + 2;                               // 指向 \r\n 之后
    return 1;                                     // 成功
}

/*
 * 解析 $L\r\n
 * 成功返回 1，*next 指向 \r\n 之后的位置，*len 被填充
 * 半包返回 0
 * 协议错误返回 -1
 */
int parse_bulk_header(const char *p, const char **next, long *len) {
    if (*p != '$') return -1;
    p++;
    const char *cr = strchr(p, '\r');
    if (!cr || *(cr + 1) != '\n') return 0;

    long l = 0;
    for (const char *q = p; q < cr; q++) {
        if (*q < '0' || *q > '9') return -1;
        if (l > 512L * 1024 * 1024) return -1;   // 提前退出：防 long 溢出（同 multibulk 的写法）
        l = l * 10 + (*q - '0');
    }
    if (l < 0 || l > 512 * 1024 * 1024) return -1;

    *len = l;
    *next = cr + 2;
    return 1;
}

/*
 * 检查并提取 L 字节内容
 * p 指向内容开头，len 是内容长度
 * 如果缓冲区数据还没到齐，返回 0（半包，等待下次 read）
 * 如果数据已完整但结尾不是 \r\n，返回 -1（协议错误）
 * 否则提取内容为 sds 到 *out（调用者负责 sdsfree），*next 指向内容末尾之后
 */
int extract_bulk_content(const char *p, long len, sds *out, const char **next, const char *buf_end) {
    if (p + len + 2 > buf_end) return 0;                       //半包（数据不够）
    if (p[len] != '\r' || p[len+1] != '\n') return -1;         //数据够但格式不对 → 协议错误
    *out = sdsnewlen(p, (size_t)len);                          // 直接构造 sds（二进制安全）
    if (!*out) return -1;                                       // 分配失败按错误处理
    *next = p + len + 2;
    return 1;
}
