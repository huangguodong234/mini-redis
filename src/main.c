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
#include <errno.h>        //errno()：全局变量，当系统调用错误时，记录错误的类型
#include <stdlib.h>       // atoi()：字符串转整数（读取自定义端口）

#include "server.h"
#include "storage.h"
#include "zset.h"  

#define DEFAULT_PORT 6379 //默认监听端口（可用命令行参数或 PORT 环境变量覆盖）
#define BACKLOG 10        //等待连接队列的最大长度（B6 修复：统一定义在此处，server.c 不再重复定义）


// 全局标志：是否继续运行（用 sig_atomic_t 保证在信号处理函数中安全读写
volatile sig_atomic_t Keep_running =1;    //volatile -告诉编译器这是一个可以修改的变量
//sig_atomic_t 一种特殊的整数类型，保证不会被信号处理打断一半

// SIGINT 处理函数：当用户按下 Ctrl+C 时调用
void handle_sighing(int sig){
    (void) sig;   //避免编译器报未使用参数的警告
    Keep_running =0;  //通知主循环退出  
}

// 注册信号处理
void setup_signal_handler(void){
    //注册信号处理
    struct sigaction sa;        //信号处理结构体
    memset(&sa,0,sizeof(sa));
    sa.sa_handler =handle_sighing;  
    sigemptyset(&sa.sa_mask);        // sigemptyset()不屏蔽信号函数 
    sa.sa_flags =0;
    sigaction(SIGINT,&sa,NULL);      //将SIGINT（Ctrl+c触发终止进程）绑定到sa中
}

int main(int argc, char **argv){
    srand((unsigned)time(NULL));  // 用当前时间播种随机数，使跳表结构不再每次启动相同

    // 解析监听端口：命令行参数 > 环境变量 PORT > 默认 6379
    int port = DEFAULT_PORT;
    const char *env_port = getenv("PORT");
    if (env_port && *env_port) {
        int ep = atoi(env_port);          // 环境变量优先于默认值
        if (ep > 0 && ep <= 65535) port = ep;
    }
    if (argc >= 2) {
        int ap = atoi(argv[1]);           // 命令行参数优先级最高
        if (ap > 0 && ap <= 65535) port = ap;
    }

    // 初始化存储引擎
    Storage *store = storage_init();
    ZSet *global_zset = zset_create();

    // 注册信号处理
    setup_signal_handler();

    /*创建服务器 socket（已绑定并监听）*/
    int server_fd = create_server_socket(port, BACKLOG);

    //主循环，不断接受新客户端,受 keep_running 控制 ----
    while(Keep_running){
        struct sockaddr_in client_addr;    //客户端地址
        socklen_t client_addr_len;            //客户端地址长度
        client_addr_len = sizeof(client_addr);
        /*client_fd 文件描述符 */
        int client_fd= accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
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
    
    /**
     * 做保活机制和配置内核接收和发射缓冲区
     * 对新 accept 出来的 client fd 做全套 socket 配置
     * 模仿 Redis：新客户端连接后一次性配置缓冲区、keepalive、nodelay，关闭Nagle算法
     * idle=60, interval=10, cnt=3
     */
    anetConfigClientSocket(client_fd);

    printf("客户端已连接：%s\n", inet_ntoa(client_addr.sin_addr));
    readQueryFromClient(client_fd, store, global_zset);
    }

    // ---- 清理资源 ----
    printf("\n正在关闭服务器,释放内存...\n");
    zset_free(global_zset);
    storage_free(store);
    close(server_fd);
    printf("所有资源已释放，再见！\n");
    return 0;
}
