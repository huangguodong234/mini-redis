//自己编写解释器的头文件    传进来的字符串 *2\r\n$3\r\n123\r\n$4\r\n2345\r\n 
#ifndef PROTOCOL_H     // 如果没有定义 PROTOCOL_H 这个宏
#define PROTOCOL_H     //就定义他

#include "sds.h"       // 解析出的参数用 sds 存

// Command：把"参数个数 + 参数数组"打包成一个小结构体传给命令层。
// 它只是 server.c 把 c->argc / c->argv 提交给 handle_command 时的薄包装
// （一次栈上赋值，几个时钟周期，相对 ~107µs/命令可忽略）。
// 注意：Command 是"借视图"不是"所有权容器"——cmd.argv 指向解析器创建的
// sds 数组，所有权仍归调用方（server.c）；命令层只读使用。唯一例外是
// SET 分支的 steal 优化：用后把 cmd->argv[1]/cmd->argv[2] 置 NULL
// （即把原数组那两格置 NULL），调用方循环释放空槽是安全 no-op、不 double-free。
typedef struct {
    int argc;      // 参数个数（含命令名本身，如 "SET" 3 个参数）
    sds *argv;     // 参数数组，指向解析器创建的 sds 数组（借视图）
} Command;

// 供第三层直接调用的辅助函数
int parse_multibulk_header(const char *p, const char **next, int *argc);
int parse_bulk_header(const char *p, const char **next, long *len);
int extract_bulk_content(const char *p, long len, sds *out, const char **next, const char *buf_end); // 提取为 sds，调用者负责 sdsfree

#endif  //结束头文件保护
