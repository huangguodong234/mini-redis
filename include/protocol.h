//自己编写解释器的头文件    传进来的字符串 *2\r\n$3\r\n123\r\n$4\r\n2345\r\n 
#ifndef PROTOCOL_H     // 如果没有定义 PROTOCOL_H 这个宏
#define PROTOCOL_H     //就定义他

//定义一个结构体，把解析后的命令打包在一起(链表结构)
typedef struct{
    int argc;       //参数个数,数组长度
    char **argv;    //指向字符串数组（指向数组首元素的指针）的指针
}Command;

// 函数声明：把原始字符串解析成 Command
// 参数 input 是客户端发来的原始字符串，比如 "*3\r\n..."
Command *parse_command(const char *input);

// 函数声明：释放 Command 占用的内存
void free_command(Command*cmd);

#endif  //结束头文件保护
