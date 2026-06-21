//服务端流程：创建socket(int domain（地址族）,int type（套接字类型）,int protocol（传输协议）我们填 0-TCP)，绑定端口(bing(套接字描述符,地址结构体指针, 地址结构体的大小)),
//开始监听（listen（套接字描述符, 等待队列最大长度））接受客户端的连接（accept(监听的套接字描述符，客户端地址结构体指针, 客户端地址长度变量的指针)）,
//读写数据（read（文件描述符，存放数据的数组，存放的数据的最大字节数-1） write（文件描述符，要发送的数据的数组，要发送的字节数）），挂断（close(要挂断的描述符))
#include <stdio.h>      // printf, perror（自定义错误提示字符串）-输出失败原因
#include <stdlib.h>     // exit（）
#include <string.h>     // memset（目标内存的起始地址, 要填充的字节值, 填充的字节数)）-初始化函数 , strlen（）  
#include <unistd.h>     // read（）, write（）, close（）
#include <signal.h>     //信号处理    sigaction结构体 （sa_handler:信号处理指针，sa_mask:暂时屏蔽的信号，sa_flags:额外选项
#include <sys/types.h>  // 一些类型定义  volatile   sig_atomic_t
#include <sys/socket.h> // socket（）, bind（）, listen（）, accept（）
#include <netinet/in.h> // sockaddr_in 结构体{sin_family：地址家族，IPv4 填 AF_INET   sin_port：端口号，必须用 htons() 转成网络字节序 sin_addr.s_addr：IP 地址，INADDR_ANY 表示监听本机所有网络接口}
#include <arpa/inet.h>  // htons（）, inet_ntoa()-表示把二进制IP地址转化为人类可以看的懂的语言
#include "protocol.h"   //解释器
#include "storage.h"    //存储器
#include "zset.h"       
#include <strings.h>      //snprintf()拼接字符串
#include <errno.h>        //errno()：全局变量，当系统调用错误时，记录错误的类型

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

// 发送响应给客户端（我们直接发送 RESP 格式字符串）
void send_response(int client_fd,const char*resp){
    if(!resp) return;
    write(client_fd,resp,strlen(resp));
}

// 核心：根据解析出的命令操作存储引擎，并返回 RESP 响应
void handle_command(int client_fd,Command *cmd,Storage *store,ZSet *zset)  //(文件描述符，解释器，存储器-里面有命令执行代码)
{
    //空命令
    if (cmd->argc<1){
        send_response(client_fd,"-ERR no command\r\n");
        return;
    }
    char *cmd_name =cmd->argv[0];    // 命令名，比如 "SET"

    //PING 命令-检测两台设备之间网络通不通、延迟高不高
    if(strcasecmp(cmd_name,"PING")==0){
        send_response(client_fd,"+PONG\r\n");
        return;
    }

    // ZADD 命令-给有序集合zset添加数据，附带分数用来排序，重复数据会更新分数。
    if(strcasecmp(cmd_name ,"ZADD")==0){
        // 参数格式：ZADD key score member
        if(cmd->argc !=4){
            send_response(client_fd, "-ERR wrong number of arguments for 'ZADD'\r\n");
            return;
        }

        char *key=cmd->argv[1];     // ZSET 的键名（暂未实现多键，先忽略，直接使用全局 zset）
        (void)key;                  // 避免编译警告
        double score =atof(cmd->argv[2]); // atof()把字符串数字转为浮点型
        char *member=cmd->argv[3];
        zset_add(zset,member,score);
        send_response(client_fd, "+OK\r\n");
        return;
    }

    // ZREM 命令-从有序集合中删除指定成员
    if(strcasecmp(cmd_name,"ZREM")==0){
        // 参数：ZREM key member
        if(cmd->argc !=3){
            send_response(client_fd, "-ERR wrong number of arguments for 'ZREM'\r\n");
            return;
        }
        char *key=cmd->argv[0];
        (void)key;
        char *member=cmd->argv[2];
        int result=zset_rem(zset,member);
        char response[16];
        snprintf(response,sizeof(response),":%d\r\n",result);
        send_response(client_fd,response);
        return;
    }

    //ZSCORE命令 指定成员 member 对应的分数（score）
    if(strcasecmp(cmd_name,"ZSCORE")==0){
        if(cmd->argc!=3){
            send_response(client_fd, "-ERR wrong number of arguments for 'ZSCORE'\r\n");
            return;
        }
        char *key=cmd->argv[1];
        (void)key;
        char *member=cmd->argv[2];
        double score=zset_score(zset,member);
        if(score ==-1.0){
            send_response(client_fd, "$-1\r\n");   // nil
        }else{
            char buf[64];
            snprintf(buf,sizeof(buf),"%g",score);
            char response[128];
            snprintf(response,sizeof(response),"$%zu\r\n%s\r\n",strlen(buf),buf);
            send_response(client_fd,response);
        }
        return;
    }

    // ZRANGE 命令(按顺序返回，按索引范围返回有序集合成员)
    if(strcasecmp(cmd_name,"ZRANGE")==0){
        // 参数：ZRANGE key start stop
        if(cmd->argc !=4){
            send_response(client_fd, "-ERR wrong number of arguments for 'ZRANGE'\r\n");
            return;
        }

        char *key=cmd->argv[1];
        (void)key;
        int start =atoi(cmd->argv[2]); //atoi()把字符串数字转为整型
        int stop=atoi(cmd->argv[3]);
        char **member =zset_range(zset,start,stop);

        // 构造 RESP 数组回复
        // 先计算成员个数
        int count =0;
        while(member[count]) count++;

        // 发送数组头
        char header[32];
        snprintf(header,sizeof(header),"*%d\r\n",count);
        send_response(client_fd,header);

        // 发送每个成员（批量字符串）
        for(int i=0;i<count;i++){
            char item[256];
            snprintf(item,sizeof(item),"$%zu\r\n%s\r\n",strlen(member[i]),
            member[i]);
            send_response(client_fd,item);
            free(member[i]);
        }
        free(member);
        return;
    }

    // 比较命令名，忽略大小写（SET / set / Set 都行）
    //SET命令
    if(strcasecmp(cmd_name,"SET")==0){
        if(cmd->argc !=3){
            send_response(client_fd,"-ERR wrong number of arguments for 'SET'\r\n");
            return;
        }
        storage_set(store,cmd->argv[1],cmd->argv[2]);
        send_response(client_fd,"+ok\r\n");
    }

    //GET 分支
    else if(strcasecmp(cmd_name,"GET")==0){
        if(cmd->argc !=2){
            send_response(client_fd,"-ERR wrong number of arguments for 'GET'\r\n");
            return;
        }
        char *val= storage_get(store,cmd->argv[1]);

        if(val){
            char response[256];
            snprintf(response,sizeof(response),"$%lu\r\n%s\r\n",strlen(val),val);
            //如果找到值：构造 RESP 批量字符串：$长度\r\n值\r\n。用 snprintf() 安全拼接，然后发送
            send_response(client_fd,response);
        }
        else {
            send_response(client_fd, "$-1\r\n");
        }
    }

    //DEL 分支
    else if(strcasecmp(cmd_name,"DEL")==0){
        if(cmd->argc !=2){
            send_response(client_fd, "-ERR wrong number of arguments for 'DEL'\r\n");
            return;
        }
        int deleted =storage_del(store,cmd->argv[1]);
        char response[16];
        snprintf(response,sizeof(response),":%d\r\n",deleted);
        send_response(client_fd, response);
    }
    //未知命令
    else{
        send_response(client_fd, "-ERR unknown command\r\n");
    }
}


int main() {
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
