/* ============================================================
 * loadgen.c —— 压测客户端（多进程 + 流水线）
 * ============================================================
 *
 * 用法：
 *   ./loadgen <port> <并发进程数> <流水线深度> <持续秒数>
 *   ./loadgen 6379 8 100 3.0        # 8 条连接，每条连发 100 条，跑 3 秒
 *
 * 输出：标准输出一行 —— 所有子进程完成的【总请求数】。
 *       QPS = 总请求数 / 持续秒数（由调用方计算）。
 *
 * ★ 为什么用 C 多进程而不是 Python 多线程：
 *   Python 的 GIL 会让「多线程压测」实际上串行执行 ——
 *   并发 2 时测出来偏高、并发 4 时反而下降（客户端成了瓶颈）。
 *   这不是被测服务器的行为，而是测量工具的假象。
 *   ★ 测量工具本身不能成为瓶颈，否则测的是工具而不是被测对象。
 *
 * ★ 为什么用多进程而不是多线程（在 C 里）：
 *   进程间完全隔离，避免共享计数器带来的 cache line 争用，
 *   也不会因为一个连接的阻塞影响到别的连接。
 *   ★ 每个子进程 = 一条独立的 TCP 连接 = 一个独立的事件源。
 *
 * ★ 流水线（pipelining）的作用：
 *   depth=1 时每条命令都要等一个 RTT（受到延迟限制）；
 *   depth=100 时连发 100 条再收 100 条回复，把 RTT 摊薄到 1/100，
 *   于是测出来的是【服务端处理能力】而不是【网络往返速度】。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <time.h>

#define PING_CMD "*1\r\n$4\r\nPING\r\n"
#define PING_LEN 14
#define RECV_BUF (1 << 20)

static double now_sec(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* 写满 total 字节；返回 0 成功，-1 失败 */
static int send_all(int fd, const char *buf, size_t total) {
    size_t off = 0;
    while (off < total) {
        ssize_t w = write(fd, buf + off, total - off);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

/* 收满 need 条回复（按 '\n' 计数）；返回 0 成功，-1 失败 */
static int recv_n(int fd, char *rb, int need) {
    while (need > 0) {
        ssize_t r = read(fd, rb, RECV_BUF);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        for (ssize_t k = 0; k < r; k++)
            if (rb[k] == '\n') need--;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr,
                "用法: %s <port> <并发进程数> <流水线深度> <持续秒数>\n", argv[0]);
        return 2;
    }
    int port     = atoi(argv[1]);
    int nproc    = atoi(argv[2]);
    int depth    = atoi(argv[3]);
    double dur   = atof(argv[4]);

    if (port <= 0 || nproc <= 0 || depth <= 0 || dur <= 0) {
        fprintf(stderr, "参数必须为正数\n");
        return 2;
    }

    /* 预先拼好一批命令，循环发送时不用重复构造 */
    char *batch = malloc((size_t)PING_LEN * (size_t)depth);
    if (!batch) return 1;
    for (int i = 0; i < depth; i++)
        memcpy(batch + (size_t)i * PING_LEN, PING_CMD, PING_LEN);
    const size_t batch_bytes = (size_t)PING_LEN * (size_t)depth;

    int (*pipes)[2] = malloc(sizeof(int[2]) * (size_t)nproc);
    if (!pipes) { free(batch); return 1; }
    for (int i = 0; i < nproc; i++) {
        if (pipe(pipes[i]) != 0) { perror("pipe"); return 1; }
    }

    for (int i = 0; i < nproc; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }

        if (pid == 0) {
            /* ---------------- 子进程：一条连接 ---------------- */
            close(pipes[i][0]);
            long count = 0;

            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) goto report;

            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons((uint16_t)port);
            inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
                goto report;

            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

            char *rb = malloc(RECV_BUF);
            if (!rb) goto report;

            double end = now_sec() + dur;
            while (now_sec() < end) {
                if (send_all(fd, batch, batch_bytes) != 0) break;
                if (recv_n(fd, rb, depth) != 0) break;
                count += depth;
            }
            free(rb);
            close(fd);

        report:
            /* ★ 用 write 而不是 printf —— fork 后子进程里 stdio 缓冲是复制的，
             *   用 printf 可能造成重复输出。管道是单一结果通道，最干净。 */
            if (write(pipes[i][1], &count, sizeof(count)) != sizeof(count)) {
                /* 父进程已退出等，忽略 */
            }
            _exit(0);
        }
        close(pipes[i][1]);
    }

    /* ---------------- 父进程：汇总 ---------------- */
    long total = 0;
    for (int i = 0; i < nproc; i++) {
        long v = 0;
        ssize_t r = read(pipes[i][0], &v, sizeof(v));
        if (r == (ssize_t)sizeof(v)) total += v;
        close(pipes[i][0]);
    }
    while (wait(NULL) > 0) { /* 收尸 */ }

    printf("%ld\n", total);
    free(pipes);
    free(batch);
    return 0;
}
