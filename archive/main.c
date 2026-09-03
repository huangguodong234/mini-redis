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


//服务端流程：创建socket(int domain（地址族）,int type（套接字类型）,int protocol（传输协议）我们填 0-TCP)，绑定端口(bing(套接字描述符,地址结构体指针, 地址结构体的大小)),
//开始监听（listen（套接字描述符, 等待队列最大长度））接受客户端的连接（accept(监听的套接字描述符，客户端地址结构体指针, 客户端地址长度变量的指针)）,
//读写数据（read（文件描述符，存放数据的数组，存放的数据的最大字节数-1） write（文件描述符，要发送的数据的数组，要发送的字节数）），挂断（close(要挂断的描述符))
//服务端流程：创建socket(int domain（地址族）,int type（套接字类型）,int protocol（传输协议）我们填 0-TCP)，绑定端口(bing(套接字描述符,地址结构体指针, 地址结构体的大小)),
//开始监听（listen（套接字描述符, 等待队列最大长度））接受客户端的连接（accept(监听的套接字描述符，客户端地址结构体指针, 客户端地址长度变量的指针)）,
//读写数据（read（文件描述符，存放数据的数组，存放的数据的最大字节数-1） write（文件描述符，要发送的数据的数组，要发送的字节数）），挂断（close(要挂断的描述符))
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>      // printf, perror（自定义错误提示字符串）-输出失败原因
#include <stdlib.h>     // exit（）
#include <string.h>     // memset（目标内存的起始地址, 要填充的字节值, 填充的字节数)）-初始化函数 , strlen（）  
#include <unistd.h>     // read（）, write（）, close（）
#include <signal.h>     //信号处理    sigaction结构体 （sa_handler:信号处理指针，sa_mask:暂时屏蔽的信号，sa_flags:额外选项
#include <time.h>       // time() 用于 srand() 播种
#include <sys/types.h>  // 一些类型定义  volatile   sig_atomic_t
#include <sys/socket.h> // socket（）, bind（）, listen（）, accept（）
#include <netinet/in.h> // sockaddr_in 结构体{sin_family：地址家族，IPv4 填 AF_INET   sin_port：端口号，必须用 htons() 转成网络字节序 sin_addr.s_addr：IP 地址，INADDR_ANY 表示监听本机所有网络接口}
#include <arpa/inet.h>  // htons（）, inet_ntoa()-表示把二进制IP地址转化为人类可以看的懂的语言
#include "protocol.h"   //解释器
#include "storage.h"    //存储器
#include "zset.h"       
#include <strings.h>      //snprintf()拼接字符串
#include <errno.h>        //errno()：全局变量，当系统调用错误时，记录错误的类型
#include <stdbool.h>

#define PORT 6379         //定死服务器监听的端口号
#define BACKLOG 10        //等待连接队列的最大长度，
#define BUFFER_SIZE 1024   //一次读取数据的最大字节数。

// 全局标志：是否继续运行（用 sig_atomic_t 保证在信号处理函数中安全读写
volatile sig_atomic_t Keep_running =1;    //volatile -告诉编译器这是一个可以修改的变量
//sig_atomic_t 一种特殊的整数类型，保证不会被信号处理打断一半

// SIGINT 处理函数：当用户按下 Ctrl+C 时调用
void handle_sighing(int sig){
    (void) sig;   //避免编译器报未使用参数的警告
    Keep_running =0;  //通知主循环退出  
}

int main() {
    srand((unsigned)time(NULL));  // 用当前时间播种随机数，使跳表结构不再每次启动相同
    int server_fd, client_fd;      //server_fd  套接字描述符 client_fd 文件描述符
    struct sockaddr_in server_addr, client_addr;    //服务器地址和客户端地址
    socklen_t client_addr_len;            //客户端地址长度
    char buffer[BUFFER_SIZE];             //暂时存放的数据
    int n;      //当前记录的字节数
    Storage *store = storage_init();
    ZSet *global_zset =zset_create();
    
    //注册信号处理
    struct sigaction sa;        //信号处理结构体
    memset(&sa,0,sizeof(sa));
    sa.sa_handler =handle_sighing;  
    sigemptyset(&sa.sa_mask);        // sigemptyset()不屏蔽信号函数 
    sa.sa_flags =0;
    sigaction(SIGINT,&sa,NULL);      //将SIGINT（Ctrl+c触发终止进程）绑定到sa中


    //创建socker
    server_fd = socket(AF_INET, SOCK_STREAM, 0);   
    if (server_fd == -1) {
        perror("socket 创建失败");
        exit(EXIT_FAILURE);    //EXIT_FAILURE  表示程序异常退出
    }
    printf("socket 创建成功\n");

    // 设置地址可重用（避免重启时端口被占用，端口在关闭后会处于TIME_WAIT状态。端口默认要等待1-2分钟才能再次使用）
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    //即使处于TIME_WAIT状态下，可以bing成功

    //  绑定地址和端口
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind 失败");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("绑定端口 %d 成功\n", PORT);

    //开始监听
    if (listen(server_fd, BACKLOG) == -1) {
        perror("listen 失败");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("服务器正在监听 127.0.0.1:%d ...\n", PORT);

    //主循环，不断接受新客户端,受 keep_running 控制 ----
    while(Keep_running){
        client_addr_len = sizeof(client_addr);
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        //当没有客户端连接时，accept() 会让进程睡着等待。
        //比如按 Ctrl+C 时，内核向进程发送 SIGINT 信号。
        //任何正在阻塞的系统调用（如 accept, read, sleep）会立即被中断并返回 -1
        //同时把全局错误码 errno 设置为 EINTR

        // 如果被信号中断（EINTR）且需要退出，则退出循环
        if (client_fd == -1) {
             // 如果被信号中断（EINTR）且需要退出，则退出循环
            if(errno ==EINTR   ){
                if(!Keep_running){
                    break;
                }else{
                    continue;     //收到其他信号中断（如调试），继续等待
                }
            }
            perror("accept 失败");
            continue;
    }
    printf("客户端已连接：%s\n", inet_ntoa(client_addr.sin_addr));


    // 处理同一个客户端的多个命令
    while((n=read(client_fd,buffer,BUFFER_SIZE-1))>0){
        buffer[n]='\0';
        Command *cmd=parse_command(buffer);
        if(cmd){
            handle_command(client_fd, cmd, store, global_zset);
            free_command(cmd);
        }else{
            send_response(client_fd, "-ERR protocol error\r\n");
        }
    }
    if(n==0) printf("客户端断开\n");
    else if(n==-1) perror("read");
    close(client_fd);
}
    // ---- 清理资源 ----
    printf("\n正在关闭服务器,释放内存...\n");
    zset_free(global_zset);
    storage_free(store);
    close(server_fd);
    printf("所有资源已释放，再见！\n");
    return 0;
}
