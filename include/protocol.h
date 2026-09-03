//自己编写解释器的头文件    传进来的字符串 *2\r\n$3\r\n123\r\n$4\r\n2345\r\n 
#ifndef PROTOCOL_H     // 如果没有定义 PROTOCOL_H 这个宏
#define PROTOCOL_H     //就定义他

//定义一个结构体，把解析后的命令打包在一起(链表结构)
typedef struct{
    int argc;       //参数个数,数组长度
    char **argv;    //指向字符串数组（指向数组首元素的指针）的指针
}Command;

// 供第三层直接调用的辅助函数
int parse_multibulk_header(const char *p, const char **next, int *argc);
int parse_bulk_header(const char *p, const char **next, long *len);
int extract_bulk_content(const char *p, long len, char **out, const char **next, const char *buf_end); // buf_end：缓冲区末尾（querybuf+qb_len），用于边界检查（B1 修复）

#endif  //结束头文件保护
