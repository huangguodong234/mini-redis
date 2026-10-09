/* ============================================================
 * server.c —— 连接生命周期 + 服务器 socket 创建
 * ============================================================
 *
 * ★ 本文件只做两件事：
 *     ① 服务器 socket 的「创建 → 绑定 → 监听」（create_server_socket）
 *     ② 客户端对象的内存生命周期（create_client / free_client）
 *
 * ★ 本文件【不】包含（拆出去的理由都是「变化原因不同」）：
 *
 *   ┌─ 解析层 ──────────────────────────── src/parser.c
 *   │   processInputBuffer / processMultibulkBuffer / processInlineCommand
 *   │   变化原因：协议规则变了（RESP 版本、inline 语法）
 *   │   ↑ 和 socket、和事件循环都无关 —— 它只认字节流
 *   │
 *   ├─ 响应编码层 ──────────────────────── src/reply.c
 *   │   send_response + 7 个 send_*
 *   │   变化原因：RESP 输出格式变了
 *   │   ↑ 和 writeToClient 是一对（入队/出队），本该同侧
 *   │
 *   └─ 网络驱动层 ──────────────────────── src/networking.c
 *       acceptTcpHandler / readQueryFromClient / freeClient / writeToClient
 *       变化原因：网络模型变了（阻塞 → epoll → 以后 io_uring）
 *
 *   拆分前这三个职责全挤在本文件里（526 行），于是「改协议解析」和
 *   「改 socket 创建」会打在同一份代码上 —— 这就是耦合的定义：
 *   不是文件里东西多，而是不相关的变化互相牵动。
 *
 * ★ 本文件也【不】再定义 g_server —— 它属于网络驱动层（见 networking.c）。
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>      // printf, perror, fprintf
#include <stdlib.h>     // malloc, free, exit
#include <string.h>     // memset
#include <unistd.h>     // close
#include <sys/socket.h> // socket(), bind(), listen()
#include <netinet/in.h> // sockaddr_in / INADDR_ANY / htons

#include "server.h"
#include "anet.h"       // anetSetReuseAddr
#include "sds.h"

/* ------------------------------------------------------------
 * create_client —— 分配并初始化一个客户端对象
 * ------------------------------------------------------------
 * ★ 只做「内存 + 状态初始化」，不做任何 socket 操作 ——
 *   所以它不需要知道 fd 从哪来（accept 的细节留在 networking.c）。
 *   这也是为什么它留在这个文件：它是对象生命周期，不是网络行为。
 */
Client *create_client(int fd){
    Client *c=malloc(sizeof(Client));
    if(!c) return NULL;

    c->fd=fd;
    c->qb_pos=0;
    c->querybuf = sdsempty();   // 用 sds 动态缓冲区，自动扩容，无固定大小限制
    if (!c->querybuf) {         // OOM：释放并返回 NULL
        free(c);
        return NULL;
    }
    // ★ 输出缓冲区（非阻塞模型必须）：待发送的数据攒在这里，
    //   由 beforeSleep / 可写回调调 writeToClient 发出。
    //   生产者在 reply.c 的 send_response，消费者在 networking.c。
    c->outbuf = sdsempty();
    if (!c->outbuf) {
        sdsfree(c->querybuf);
        free(c);
        return NULL;
    }
    c->multibulklen = 0;
    c->bulklen = -1;
    c->cmd_start = 0;
    c->argv = NULL;
    c->argc = 0;
    c->argv_max = 0;
    /* ★ 链表指针置 NULL。真正挂进链表由 networking.c 的
     *   acceptTcpHandler 调 clients_add 完成（在 create_client 之后）。 */
    c->next = NULL;
    c->prev = NULL;

    return c;
}

/* ------------------------------------------------------------
 * free_client —— 释放客户端
 * ------------------------------------------------------------
 * ★ 真正的关闭逻辑在 networking.c 的 freeClient()：
 *   「撤关注 → 关 fd → 摘链表 → 释放」四步需要访问事件循环和客户端
 *   链表，属于网络驱动层的职责（本文件连 ae.h 都没包含）。
 *
 *   这里保留 free_client 作为兼容入口，直接转发。
 * ★ el 传 NULL：本条路径【不撤关注】，调用方必须自己保证该 fd 已从
 *   事件循环里摘掉（或在事件循环销毁之后才调用）。这是旧的阻塞模型
 *   遗留入口，新代码请直接用 freeClient(el, c)。
 */
void free_client(Client *c) {
    freeClient(NULL, c);
}

/* ------------------------------------------------------------
 * create_server_socket —— 创建「已绑定并监听」的服务器 socket
 * ------------------------------------------------------------
 * 服务端流程：
 *   创建 socket(int 地址族, int 套接字类型, int 传输协议) → 我们填 0 即 TCP
 *   绑定端口 bind(套接字描述符, 地址结构体指针, 地址结构体大小)
 *   开始监听 listen(套接字描述符, 等待队列最大长度)
 *   之后由事件循环 accept 新连接（见 networking.c 的 acceptTcpHandler）
 *
 * ★ 失败处理策略是「fail-fast」：socket 创建/绑定/监听失败都直接 exit，
 *   因为没有监听 socket 的服务器没有任何可用性可言，硬撑着只会更难排查。
 *   （客户端的失败则可以优雅降级 —— 见 acceptTcpHandler 里的 close+continue）
 */
int create_server_socket(int port, int backlog){
    int server_fd;     //server_fd 套接字描述符
    struct sockaddr_in server_addr;   //服务器地址

    //创建 socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket 创建失败");
        exit(EXIT_FAILURE);    //EXIT_FAILURE  表示程序异常退出
    }
    printf("socket 创建成功\n");

    //解决重启 TIME_WAIT 端口占用
    anetSetReuseAddr(server_fd);

    //  绑定地址和端口
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind 失败");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("绑定端口 %d 成功\n", port);

    //开始监听
    if (listen(server_fd, backlog) == -1) {
        perror("listen 失败");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("服务器正在监听 0.0.0.0:%d ...\n", port);  // INADDR_ANY 如实写成 0.0.0.0

    return server_fd;
}
