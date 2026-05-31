//编写解释器代码   传进来的字符串   *2\r\n$3\r\nSET\r\n$3\r\nGET\r\n

#include <stdio.h>   
#include <stdlib.h>    //malloc() free()  atoi()-把字符串转成整数返回
#include <string.h>    //strlen() strncpy() strcmp()  strcpy()
#include <ctype.h>     //isdigit()  判断字符是否是数字
#include "protocol.h"    //我们自己的头文件

static int  parse_int(const char **p)
 {
    const char *start=*p;     //记录数字开始位置
    while(**p && isdigit((unsigned char)**p))      // 只要当前字符是数字就一直向后移动     
        (*p)++;
    int len =*p-start;  //计算数字长度
    char num[16];    //临时数组存放数字字符串
    strncpy(num,start,len);  // 复制 len 个字符
    num[len]='\0';      // 手动添加字符串结束符
    return atoi(num);   //把字符串转成整数返回
}

// 辅助函数：跳过 \r\n
static const char *skip_crlf(const char*p){
    if(*p=='\r') p++;    // 如果当前字符是回车，跳过一个
    if(*p=='\n') p++;    //如果当前字符是换行，跳过一个
    return p;            
}


Command *parse_command(const char *input)
{
    //如果输入为空或首字符不是 '*' 就报错
    if(!input || *input !='*') return NULL;

    const char *p=input+1;  // 跳过 '*'，指向数组长度部分
    int argc=parse_int(&p);   // 解析参数个数，并移动 p
    if(argc <=0 || argc >10) return NULL;   // 参数个数不合法就失败

    // 分配 Command 结构体
    Command *cmd =(Command*)malloc(sizeof(Command));
    cmd ->argc=argc; //记录参数个数
    cmd->argv=(char **)malloc(sizeof(char*)*argc);

    // 循环解析每一个参数字符串
    for(int i=0;i<argc;i++){
        p=skip_crlf(p);  //跳过 \r\n
        if(*p !='$') goto fail; // 批量字符串必须以 '$' 开头
        p++;                      // 跳过 '$'
        int len =parse_int(&p);  // 解析字符串长度
        if(len<0) goto fail;
        p=skip_crlf(p);

        cmd->argv[i]=malloc(len +1);
        strncpy(cmd->argv[i],p,len);    // 复制 len 个字符
        cmd->argv[i][len]='\0';
        p+=len;                 //// 移动指针到当前字符串末尾
    }return cmd;   // 成功返回命令结

    fail: // 解析过程中任何一步失败就跳到这里清理内存
    for(int i=0;i<cmd->argc;i++) free(cmd ->argv[i]);
    free(cmd->argv);
    free(cmd);
    return NULL;
}

// 释放 Command 占用的所有内存
void free_command(Command *cmd){
    if(!cmd) return;   // 空指针检查
    for(int i=0;i<cmd->argc;i++){
        free(cmd->argv[i]);
    }
    free(cmd->argv);
    free(cmd);
}
