//自己编写解释器的头文件    传进来的字符串 *2\r\n$3\r\n123\r\n$4\r\n2345\r\n 
#ifndef PROTOCOL_H     // 如果没有定义 PROTOCOL_H 这个宏
#define PROTOCOL_H     //就定义他

#include "sds.h"       // 解析出的参数用 sds 存

// 注：早期版本用 Command{int argc; sds *argv;} 结构体把参数打包传给命令层。
// 现在命令层直接收 (int argc, sds *argv)，不再需要这个薄包装——
// 它只是一次栈上赋值（几个时钟周期，相对 ~107µs/命令可忽略），
// 反而让"谁拥有 argv、哪些槽已被转移"的所有权语义更绕。

// 供第三层直接调用的辅助函数
int parse_multibulk_header(const char *p, const char **next, int *argc);
int parse_bulk_header(const char *p, const char **next, long *len);
int extract_bulk_content(const char *p, long len, sds *out, const char **next, const char *buf_end); // 提取为 sds，调用者负责 sdsfree

#endif  //结束头文件保护
