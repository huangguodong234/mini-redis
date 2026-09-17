//自己编写解释器的头文件    传进来的字符串 *2\r\n$3\r\n123\r\n$4\r\n2345\r\n 
#ifndef PROTOCOL_H     // 如果没有定义 PROTOCOL_H 这个宏
#define PROTOCOL_H     //就定义他

#include "sds.h"       // Command.argv 用 sds 存参数

//定义一个结构体，把解析后的命令打包在一起
typedef struct{
    int argc;       //参数个数,数组长度
    sds *argv;      //参数数组（每个元素是 sds，所有权归创建者）
}Command;

// 供第三层直接调用的辅助函数
int parse_multibulk_header(const char *p, const char **next, int *argc);
int parse_bulk_header(const char *p, const char **next, long *len);
int extract_bulk_content(const char *p, long len, sds *out, const char **next, const char *buf_end); // 提取为 sds，调用者负责 sdsfree

#endif  //结束头文件保护
