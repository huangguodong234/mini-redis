/* bench_skipspan.c —— 跳表 span + O(1) find 改造的【算法层】微基准。
 *
 * 用途：端到端命令 QPS 被服务端框架（每条命令 ~20µs 的 RESP 解析 / send 段
 * 系统调用）压在 ~50k 量级，测不出跳表 O(1) vs O(n)、O(log n) vs O(n) 的差别。
 * 这里直接调用 skiplist API，绕过框架，定时跳表操作本身，用「规模递增看是否
 * 变慢」来实证复杂度：
 *
 *   * skiplist_find   : 表从 1 万 → 80 万，单次查找耗时【几乎不变】→ O(1)
 *   * skiplist_range  : 无论 start=0 还是 start=60 万，跳转+收集耗时基本一致
 *                        → 用 span 跳到起点是 O(log n)，不再沿第 0 层 for 循环
 *   * skiplist_get_rank: O(log n) 求排名
 *   * skiplist_add    : O(log n) 插入
 *
 * 用 stdin 传入表大小，便于脚本批量采样多档。
 * 编译：gcc -O2 -Iinclude bench/bench_skipspan.c src/skiplist.c src/sds.c -o ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "skiplist.h"

static double now_us(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec*1e6 + (double)ts.tv_nsec/1e3;
}

static double ns_per(long long calls, const double us){
    return us*1e3/(double)calls;
}

int main(int argc, char **argv){
    int n = argc>1 ? atoi(argv[1]) : 200000;
    long long REP = atoll(argv[2] ? argv[2] : "2000000");

    srand(12345);
    Skiplist *sl = skiplist_create();
    if(!sl){ fprintf(stderr,"create failed\n"); return 1; }

    // 灌入 n 个成员（score 随机 0..1e6，member 形如 m<idx>）
    for(int i=0;i<n;i++){
        char buf[32];
        snprintf(buf,sizeof(buf),"m%d",i);
        sds m = sdsnew(buf);
        skiplist_add(sl, m, (double)(rand()%1000000)/1.0);
        // skiplist_add 接管了 m 的所有权，不可再 free
    }

    /* ---------- skiplist_find：O(1) 哈希直查 ---------- */
    {
        // 随机取 REP 个 member 做查找（命中）
        sds *keys = malloc(sizeof(sds)*REP);
        for(long long i=0;i<REP;i++){
            char buf[32];
            snprintf(buf,sizeof(buf),"m%d", rand()%n);
            keys[i] = sdsnew(buf);
        }
        volatile uintptr_t sink=0;
        double t0=now_us();
        for(long long i=0;i<REP;i++){ SkipNode *nd=skiplist_find(sl,keys[i]); if(nd) sink=(uintptr_t)nd->member; }
        double dt=now_us()-t0;
        printf("find        n=%-8d %24.1f ns/op  (%lld 命中查找 %d 成员表)\n", n, ns_per(REP,dt), REP, n);
        for(long long i=0;i<REP;i++) sdsfree(keys[i]);
        free(keys);
    }

    /* ---------- skiplist_add：更新已存在成员的分数（find + 重排） ---------- */
    {
        long long R = REP<1000000?REP:1000000;
        sds *keys = malloc(sizeof(sds)*R);
        for(long long i=0;i<R;i++){
            char buf[32];
            snprintf(buf,sizeof(buf),"m%d", rand()%n);
            keys[i] = sdsnew(buf);
        }
        double t0=now_us();
        for(long long i=0;i<R;i++){
            char buf[32];
            snprintf(buf,sizeof(buf),"%d", rand()%1000000);
            sds score = sdsnew(buf);      // skiplist_add 会接管并 sdsfree
            skiplist_add(sl, keys[i], atof(buf));
            sdsfree(score);
        }
        double dt=now_us()-t0;
        printf("add(update) n=%-8d %24.1f ns/op  (%lld 次 O(log n) 更新)\n", n, ns_per(R,dt), R);
        // skiplist_add 已接管 keys[i]（member 所有权移交跳表），不可再 sdsfree
        free(keys);
    }

    /* ---------- skiplist_get_rank：O(log n) 求排名 ---------- */
    {
        long long R = REP<1000000?REP:1000000;
        sds *keys = malloc(sizeof(sds)*R);
        for(long long i=0;i<R;i++){
            char buf[32];
            snprintf(buf,sizeof(buf),"m%d", rand()%n);
            keys[i]=sdsnew(buf);
        }
        volatile unsigned long sink=0;
        double t0=now_us();
        for(long long i=0;i<R;i++) sink += skiplist_get_rank(sl, keys[i]);
        double dt=now_us()-t0;
        printf("get_rank    n=%-8d %24.1f ns/op  (O(log n)，sum=%lu)\n", n, ns_per(R,dt), (unsigned long)sink);
        for(long long i=0;i<R;i++) sdsfree(keys[i]);
        free(keys);
    }

    /* ---------- skiplist_range：span 跳到 start，O(log n) ---------- */
    {
        int starts[] = {0, n/4, n/2, n-1000 < 0 ? 0 : n-1000};
        const char *names[] = {"start=0   ", "start=n/4 ", "start=n/2 ", "start≈n   "};
        for(int k=0;k<4;k++){
            int st = starts[k];
            int stop = st+9;
            if(stop>=n) stop=n-1;
            volatile uintptr_t sink=0;
            long long R=REP;
            double t0=now_us();
            for(long long i=0;i<R;i++){
                sds *res=skiplist_range(sl, st, stop);
                if(res[0]) sink=(uintptr_t)res[0];
                free(res);
            }
            double dt=now_us()-t0;
            printf("range %s n=%-8d %7.1f ns/op  (跳转+收集 %d 个成员，起始位 %d)\n",
                   names[k], n, ns_per(R,dt), stop-st+1, st);
        }
    }

    skiplist_free(sl);
    return 0;
}
