/*
 * bench_syscall.c —— 系统调用基线测量（不依赖 mini-redis）
 *
 * 目的：回答「一次 write() 到底该花多少纳秒」这个问题。
 *
 * 为什么需要它：
 *   在 mini-redis 上测得「每次 write 约 5~33 µs」，但无法判断这是
 *   【代码的问题】还是【运行环境的固有成本】。本程序测的是同一个
 *   write() 系统调用，但剥掉了所有应用层逻辑，得到纯粹的环境基线。
 *
 * 于是对比变成：
 *   基线（本程序）  vs  服务器实测    →  差值才是应用层该负责的部分
 *
 * 测什么（每一项都是受控变量）：
 *   ① write 到 loopback，5 / 50 / 500 / 4096 字节，看数据量是否影响耗时
 *   ② read 到 loopback，看读侧成本
 *   ③ epoll_wait 空转（0 超时）与 1ms 超时的成本
 *   ④ 一次 clock_gettime 的成本（确认计时本身不污染测量）
 *   ⑤ 一次 getpid（最轻的 syscall）作为「syscall 地板价」参考
 *
 * 关键设计：对端进程【持续读取】，避免发送缓冲填满导致 EAGAIN。
 *   如果对端不读，write 会返回 EAGAIN（一次快速失败），
 *   测出来的就不是「成功写入」的成本了。
 *
 * 用法：
 *   gcc -O2 -o /tmp/bench_syscall bench/bench_syscall.c
 *   /tmp/bench_syscall            # 默认每项 20 万次
 *   /tmp/bench_syscall 1000000    # 指定次数
 *   或 make syscall
 *
 * 判读：
 *   · loopback 上 write 小数据通常在 1~3 µs（原生 Linux）
 *   · WSL2 因为要穿越 Hyper-V 边界，通常偏高（实测 5 µs 量级）
 *   · 若本程序也测出 5 µs → 是环境特性，不是 mini-redis 的问题
 *   · 若本程序只有 1 µs 而服务器测出 5 µs → 差值在应用层，值得查
 *
 * 端口用随机高位值，避免与本机其他服务冲突。
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* ---------------- 计时 ---------------- */
static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t pct(const uint64_t *sorted, long n, double p)
{
    long i = (long)(p * (double)n);
    if (i >= n) i = n - 1;
    if (i < 0) i = 0;
    return sorted[i];
}

/*
 * 统一的统计输出。lat[] 会被就地排序，所以调用后不要再使用原顺序。
 * 用中位数与 p99 而不是平均值：syscall 延迟分布有长尾，
 * 平均值会被少数被抢占的样本拉偏。
 */
static void report(const char *label, uint64_t *lat, long n)
{
    qsort(lat, (size_t)n, sizeof(uint64_t), cmp_u64);
    uint64_t sum = 0;
    for (long i = 0; i < n; i++) sum += lat[i];
    printf("  %-34s 中位 %7llu ns | 均 %7.0f | p99 %8llu | 最大 %9llu\n",
           label,
           (unsigned long long)lat[n / 2],
           (double)sum / (double)n,
           (unsigned long long)pct(lat, n, 0.99),
           (unsigned long long)lat[n - 1]);
}

/* ---------------- 建立一个「对端持续读取」的 loopback 连接 ----------------
 *
 * 返回客户端 fd；服务端 fd 通过 *srv_fd 返回（父进程持有，用于收尾）。
 * 子进程负责 accept + 无限 read —— 这样发送缓冲永远不会满，
 * write 走的总是「成功写入」路径。
 *
 * 为什么用 fork 而不是线程：子进程被调度到另一个核，
 * 更接近真实的客户端-服务器分离场景。
 */
static int make_pair(int *srv_fd, int *child_pid, size_t bufsize)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return -1; }

    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;                 /* ★ 让内核分配空闲端口，避免冲突 */

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind"); close(lfd); return -1;
    }
    socklen_t alen = sizeof(addr);
    if (getsockname(lfd, (struct sockaddr *)&addr, &alen) != 0) {
        perror("getsockname"); close(lfd); return -1;
    }
    if (listen(lfd, 4) != 0) { perror("listen"); close(lfd); return -1; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); close(lfd); return -1; }

    if (pid == 0) {
        /* ---- 子进程：不停读，直到连接关闭 ---- */
        int c = accept(lfd, NULL, NULL);
        if (c < 0) _exit(0);
        /* 缓冲开大一点，减少子进程自身的 syscall 次数 ——
         * 它不该成为瓶颈，否则测的是它而不是 write。 */
        char *buf = malloc(1 << 22);
        if (!buf) _exit(0);
        while (read(c, buf, 1 << 22) > 0) { }
        _exit(0);
    }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return -1; }
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("connect"); close(s); close(lfd); return -1;
    }
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* ★ 和 mini-redis 一致：非阻塞。否则 write 会真正阻塞，
     *   测出来的就不是「非阻塞 write 的成本」了。 */
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);

    /* 发送缓冲设小，让内核路径更接近「小负载」场景；
     * 设大也行，但对端在读，影响不大。这里保持默认。 */
    (void)bufsize;

    close(lfd);
    *srv_fd = lfd;          /* 已关闭，占位；真正的清理靠 child_pid */
    *child_pid = (int)pid;
    return s;
}

static void teardown(int fd, int pid)
{
    if (fd >= 0) close(fd);
    if (pid > 0) {
        /* 客户端 fd 关闭后子进程的 read 返回 0，会自然退出 */
        int st;
        waitpid((pid_t)pid, &st, 0);
    }
}

/* ---------------- ① write 成本 vs 数据量 ---------------- */
static void test_write(long n)
{
    printf("\n① write() 到 loopback —— 看数据量是否影响耗时\n");

    static const size_t sizes[] = { 5, 50, 500, 4096 };
    const int nsizes = (int)(sizeof(sizes) / sizeof(sizes[0]));

    uint64_t *lat = malloc(sizeof(uint64_t) * (size_t)n);
    if (!lat) { fprintf(stderr, "malloc 失败\n"); return; }

    for (int k = 0; k < nsizes; k++) {
        size_t sz = sizes[k];
        int srv, pid;
        int fd = make_pair(&srv, &pid, 0);
        if (fd < 0) { free(lat); return; }

        char *buf = malloc(sz);
        memset(buf, 'x', sz);

        long eagain = 0;
        for (long i = 0; i < n; i++) {
            uint64_t t0 = now_ns();
            ssize_t w = write(fd, buf, sz);
            uint64_t t1 = now_ns();
            lat[i] = t1 - t0;
            if (w < 0) eagain++;
        }

        char label[64];
        snprintf(label, sizeof(label), "write %zu 字节", sz);
        report(label, lat, n);
        if (eagain) printf("      ⚠ 其中 %ld 次返回错误（EAGAIN 等），会拉低均值\n", eagain);

        free(buf);
        teardown(fd, pid);
        usleep(100000);     /* 让子进程有时间退出 */
    }
    free(lat);
}

/* ---------------- ② read 成本 ---------------- */
/*
 * 量的是「服务器 read 客户端发来的数据」的成本。
 *
 * ⚠ 这里必须让【对端写入、本进程读取】。
 *   第一版写错成「本进程 write 灌数据再 read 同一个 fd」——
 *   那是向自己的发送缓冲写、再从接收缓冲读，两者不相干，
 *   结果 read 永远返回 EAGAIN，测不出任何样本。
 *
 * 做法：子进程负责【写】，父进程负责【读】。
 */
static void test_read(long n)
{
    printf("\n② read() 从 loopback —— 读侧成本\n");

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return; }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind"); close(lfd); return;
    }
    socklen_t alen = sizeof(addr);
    getsockname(lfd, (struct sockaddr *)&addr, &alen);
    if (listen(lfd, 4) != 0) { perror("listen"); close(lfd); return; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); close(lfd); return; }

    if (pid == 0) {
        /* ---- 子进程：只写，把数据源源不断灌进来 ---- */
        int c = accept(lfd, NULL, NULL);
        if (c < 0) _exit(0);
        char *buf = malloc(64);
        if (!buf) _exit(0);
        memset(buf, 'y', 64);
        /* 父进程读走多少就补多少；写不动就短暂让出 CPU */
        for (;;) {
            ssize_t w = write(c, buf, 64);
            if (w <= 0) {
                if (errno == EAGAIN) { usleep(200); continue; }
                break;                 /* 父进程关了连接 */
            }
        }
        _exit(0);
    }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); close(lfd); return; }
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("connect"); close(s); close(lfd); return;
    }
    close(lfd);

    uint64_t *lat = malloc(sizeof(uint64_t) * (size_t)n);
    if (!lat) { close(s); return; }

    char tmp[64];
    long reads = 0, blocked = 0;
    for (long i = 0; i < n; i++) {
        uint64_t t0 = now_ns();
        ssize_t r = read(s, tmp, sizeof(tmp));
        uint64_t t1 = now_ns();
        if (r > 0) lat[reads++] = t1 - t0;
        else       blocked++;         /* EAGAIN：子进程还没补上数据 */
    }

    if (reads > 0) report("read 64 字节", lat, reads);
    else           printf("  （没有采到样本）\n");
    if (blocked)
        printf("      ⚠ %ld 次 EAGAIN（对端没跟上，不代表服务器 read 慢）\n", blocked);

    free(lat);
    close(s);
    int st;
    waitpid(pid, &st, 0);
}

/* ---------------- ③ epoll_wait 成本 ---------------- */
static void test_epoll(long n)
{
    printf("\n③ epoll_wait() 空转 —— 事件循环每轮的固定成本\n");

    int ep = epoll_create1(0);
    if (ep < 0) { perror("epoll_create1"); return; }

    uint64_t *lat = malloc(sizeof(uint64_t) * (size_t)n);
    if (!lat) { close(ep); return; }
    struct epoll_event evs[64];

    /* 无超时（立即返回）：纯 syscall 成本 */
    for (long i = 0; i < n; i++) {
        uint64_t t0 = now_ns();
        epoll_wait(ep, evs, 64, 0);
        uint64_t t1 = now_ns();
        lat[i] = t1 - t0;
    }
    report("epoll_wait 超时=0（纯 syscall）", lat, n);

    free(lat);
    close(ep);
}

/* ---------------- ④ 计时器自身成本 ---------------- */
static void test_clock(long n)
{
    printf("\n④ 计时开销 —— 确认探针本身不污染测量\n");

    uint64_t *lat = malloc(sizeof(uint64_t) * (size_t)n);
    if (!lat) return;
    for (long i = 0; i < n; i++) {
        uint64_t t0 = now_ns();
        uint64_t t1 = now_ns();
        lat[i] = t1 - t0;
    }
    report("两次 clock_gettime", lat, n);
    free(lat);

    /* getpid 是最廉价的 syscall，作为「syscall 地板价」 */
    lat = malloc(sizeof(uint64_t) * (size_t)n);
    if (!lat) return;
    for (long i = 0; i < n; i++) {
        uint64_t t0 = now_ns();
        getpid();
        uint64_t t1 = now_ns();
        lat[i] = t1 - t0;
    }
    report("getpid（syscall 地板价）", lat, n);
    free(lat);
}

/* ---------------- 环境信息 ---------------- */
static void print_env(void)
{
    printf("=== 系统调用基线（不经过 mini-redis）===\n\n");

    /* 内核版本 */
    FILE *f = fopen("/proc/version", "r");
    if (f) {
        char line[256];
        if (fgets(line, sizeof(line), f)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            printf("内核   : %s\n", line);
        }
        fclose(f);
    }

    /* 是否 WSL */
    int is_wsl = 0;
    f = fopen("/proc/sys/kernel/osrelease", "r");
    if (f) {
        char line[256];
        if (fgets(line, sizeof(line), f)) {
            if (strstr(line, "microsoft") || strstr(line, "WSL")) is_wsl = 1;
        }
        fclose(f);
    }
    printf("环境   : %s\n", is_wsl ? "WSL（syscall 要穿越 Hyper-V 边界，预期偏高）"
                                   : "原生 Linux");

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    printf("CPU    : %ld 核\n", ncpu > 0 ? ncpu : -1);
}

int main(int argc, char **argv)
{
    long n = (argc > 1) ? atol(argv[1]) : 200000;
    if (n < 1000) n = 1000;

    print_env();
    printf("每项 %ld 次\n", n);
    printf("（用中位数而非平均值：syscall 分布有长尾，均值会被少数被抢占的样本拉偏）\n");

    test_clock(n);
    test_epoll(n);
    test_write(n);
    test_read(n);

    printf("\n--- 判读方法 ---\n");
    printf("  · 拿本程序的 write 结果 与 mini-redis 实测值对比：\n");
    printf("      两者接近 → 那个成本是【环境固有】的，不是代码问题\n");
    printf("      本程序明显更低 → 差值在应用层，值得继续查\n");
    printf("  · 原生 Linux 的 loopback write 小数据通常在 1~3 µs 区间；\n");
    printf("    WSL2 因为要穿越虚拟化边界，偏高是正常的\n");
    printf("  · getpid 那行给出这个环境的「syscall 地板价」，\n");
    printf("    任何 syscall 都不可能比它更快\n");

    return 0;
}
