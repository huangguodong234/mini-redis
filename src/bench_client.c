// ============================================================
// bench_client.c —— mini-redis 原生 C 语言压测客户端
// ============================================================
// 功能：
//   1. 在一个 TCP 连接内向服务器连续发送 10 万个 SET 命令
//   2. 再发送 10 万个随机 key 的 GET 命令
//   3. 精确记录服务器纯处理时间（不含进程启动开销）
//
// 用法：
//   编译：gcc -o bench_client src/bench_client.c
//   运行：./bench_client
//
// 注意：本程序不依赖项目内的任何头文件，只使用标准库和系统调用
// ============================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 6379
#define IP   "127.0.0.1"

#define TOTAL_KEYS   100000   // 插入的 key 总数
#define TEST_ROUNDS  100000   // 随机查询次数

// ---------- 发送一个字符串到服务器 ----------
void send_data(int fd, const char *data) {
    write(fd, data, strlen(data));
}

// ---------- 从服务器读取响应并丢弃 ----------
void read_response(int fd) {
    char buf[1024];
    read(fd, buf, sizeof(buf) - 1);
    // 为了性能，不做解析，直接消费掉
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, IP, &addr.sin_addr);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("连接服务器失败");
        return 1;
    }
    printf("已连接到服务器 %s:%d\n", IP, PORT);

    char cmd[128];
    clock_t start, end;
    double elapsed;

    // ==================== 阶段 1：插入 10 万个 key ====================
    printf("开始插入 %d 个 key...\n", TOTAL_KEYS);
    start = clock();

    for (int i = 0; i < TOTAL_KEYS; i++) {
        // 构造 RESP 格式的 SET 命令：SET key_00000 value ... SET key_99999 value
        snprintf(cmd, sizeof(cmd), 
                 "*3\r\n$3\r\nSET\r\n$7\r\nkey_%05d\r\n$5\r\nvalue\r\n", i);
        send_data(fd, cmd);
        read_response(fd);   // 消费掉 +OK\r\n
    }

    end = clock();
    elapsed = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("插入 %d 个 key 耗时: %.3f 秒\n", TOTAL_KEYS, elapsed);
    if (elapsed > 0) {
        printf("插入 QPS: %.2f\n", TOTAL_KEYS / elapsed);
    }

    // ==================== 阶段 2：随机查询 10 万次 ====================
    printf("开始随机查询 %d 次...\n", TEST_ROUNDS);
    srand(time(NULL));
    start = clock();

    for (int i = 0; i < TEST_ROUNDS; i++) {
        int key_id = rand() % TOTAL_KEYS;   // 均匀随机分布在 0 ~ 99999
        snprintf(cmd, sizeof(cmd),
                 "*2\r\n$3\r\nGET\r\n$7\r\nkey_%05d\r\n", key_id);
        send_data(fd, cmd);
        read_response(fd);
    }

    end = clock();
    elapsed = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("随机查询 %d 次耗时: %.3f 秒\n", TEST_ROUNDS, elapsed);
    if (elapsed > 0) {
        printf("查询 QPS: %.2f\n", TEST_ROUNDS / elapsed);
    }

    close(fd);
    printf("测试完成。\n");
    return 0;
}
