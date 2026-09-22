/* bench_old_skip.c —— 旧版（O(n) find + O(n) for 循环 range）对照微基准。
 * 直接 include 旧版 skiplist.c，测同样操作，用于和新版对比、给出"改造前"数字。
 * 编译：gcc -O2 -I/tmp/oldskiplist -Isrc bench_old_skip.c -o bench_old_skip
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "skiplist.h"
#include "skiplist.c"

static double now_us(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec*1e6 + (double)ts.tv_nsec/1e3;
}
static double ns_per(long long calls, const double us){ return us*1e3/(double)calls; }

int main(int argc, char **argv){
    int n = argc>1 ? atoi(argv[1]) : 200000;
    long long REP = atoll(argv[2] ? argv[2] : "2000000");
    srand(12345);
    Skiplist *sl = skiplist_create();
    for(int i=0;i<n;i++){
        char buf[32]; snprintf(buf,sizeof(buf),"m%d",i);
        sds m=sdsnew(buf);
        skiplist_add(sl, m, (double)(rand()%1000000));
    }

    /* 旧 find：第 0 层线性扫描 O(n) */
    {
        long long R = REP<1000000?REP:1000000;
        sds *keys=malloc(sizeof(sds)*R);
        for(long long i=0;i<R;i++){ char buf[32]; snprintf(buf,sizeof(buf),"m%d",rand()%n); keys[i]=sdsnew(buf); }
        volatile uintptr_t sink=0;
        double t0=now_us();
        for(long long i=0;i<R;i++){ SkipNode *nd=skiplist_find(sl,keys[i]); if(nd) sink=(uintptr_t)nd->member; }
        double dt=now_us()-t0;
        printf("OLD find    n=%-7d %24.1f ns/op  (O(n) 第0层线性扫)\n", n, ns_per(R,dt));
        for(long long i=0;i<R;i++) sdsfree(keys[i]);
        free(keys);
    }

    /* 旧 range：for 循环走第 0 层到 start 再收集（正是本次要淘汰的实现） */
    {
        int starts[] = {0, n/2, n-1000<0?0:n-1000};
        const char *names[] = {"OLD range start=0   ", "OLD range start=n/2 ", "OLD range start≈n   "};
        for(int k=0;k<3;k++){
            int st=starts[k], stop=st+9; if(stop>=n) stop=n-1;
            long long R=REP; double t0=now_us();
            for(long long i=0;i<R;i++){ sds *res=skiplist_range(sl,st,stop); free(res); }
            double dt=now_us()-t0;
            printf("%s n=%-7d %7.1f ns/op  (for 走到 start=%d)\n", names[k], n, ns_per(R,dt), st);
        }
    }
    skiplist_free(sl);
    return 0;
}
