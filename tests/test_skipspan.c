/*
 * 跳表 span 专项测试
 *
 * 为什么要有这个测试：
 *   - 旧版 skiplist_range 靠 for 循环从表头走到 start（O(N)），不依赖 span；
 *   - 本次改造后 range 用 span 直接跳到 start 排名（O(log N)），并新增
 *     skiplist_get_rank（O(log N)）。这两者都依赖每层 span 在
 *     插入 / 更新 / 删除 过程中被正确维护。
 *   - 这里用一个「参考数组」做暴力真值，随机插入 / 更新 / 删除后，逐条对照：
 *       ① skiplist_get_rank 的排名  == 参考数组下标
 *       ② skiplist_range 的截取结果 == 参考数组切片
 *   - span 有任何一处算错，① 或 ② 就会在随机操作几百步内暴露。
 *
 * ⚠ 所有权纪律应沿用测试文件通用约定：skiplist_add 的 member 所有权转移交，
 *   skiplist_range 返回借用引用（只 free 数组，不 free 元素）。
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiplist.h"

static int failures = 0;
static int checked = 0;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("  [FAIL] %s:%d: ", __FILE__, __LINE__);         \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

/* 参考真值：member 名 + score，按 score 升序（平票按名字）维护 */
typedef struct {
    sds name;
    double score;
} RefEnt;
static RefEnt *ref;
static int ref_n;
static int ref_cap;

static int ref_cmp(const void *a, const void *b) {
    const RefEnt *x = a, *y = b;
    if (x->score < y->score) return -1;
    if (x->score > y->score) return  1;
    return strcmp(x->name, y->name);
}
static void ref_sort(void) {
    qsort(ref, (size_t)ref_n, sizeof(RefEnt), ref_cmp);
}
static int ref_find(const char *name) {
    for (int i = 0; i < ref_n; i++)
        if (strcmp(ref[i].name, name) == 0) return i;
    return -1;
}
static void ref_add(const char *name, double sc) {
    int i = ref_find(name);
    if (i >= 0) { ref[i].score = sc; ref_sort(); return; }
    if (ref_n == ref_cap) {
        ref_cap = ref_cap ? ref_cap * 2 : 64;
        ref = realloc(ref, (size_t)ref_cap * sizeof(RefEnt));
    }
    ref[ref_n].name = sdsnew(name);
    ref[ref_n].score = sc;
    ref_n++;
    ref_sort();
}
static void ref_del(const char *name) {
    int i = ref_find(name);
    if (i < 0) return;
    sdsfree(ref[i].name);
    ref[i] = ref[ref_n - 1];
    ref_n--;
    ref_sort();
}

/* 对照 1：skiplist_get_rank 应与参考下标一致（1-based rank） */
static void check_ranks(Skiplist *sl) {
    for (int i = 0; i < ref_n; i++) {
        sds q = sdsnew(ref[i].name);
        unsigned long r = skiplist_get_rank(sl, q);
        sdsfree(q);
        checked++;
        CHECK(r == (unsigned long)(i + 1),
              "get_rank(%s) = %lu，应为 %d", ref[i].name, r, i + 1);
    }
}

/* 对照 2：skiplist_range 应与参考数组切片一致（含负数索引 / 越界裁剪） */
static void check_range(Skiplist *sl, int start, int stop) {
    sds *r = skiplist_range(sl, start, stop);
    CHECK(r != NULL, "range(%d,%d) 返回 NULL", start, stop);
    if (!r) return;
    int n = 0;
    while (r[n]) n++;

    int rs = start, rt = stop;
    if (rs < 0) rs = ref_n + rs;
    if (rt < 0) rt = ref_n + rt;
    if (rs < 0) rs = 0;
    if (rt < 0) rt = 0;
    if (rs > rt || rs >= ref_n) {
        CHECK(n == 0, "range(%d,%d) 应返回空，实际 %d 个", start, stop, n);
        free(r);
        return;
    }
    if (rt >= ref_n) rt = ref_n - 1;
    int expect = rt - rs + 1;
    checked++;
    CHECK(n == expect,
          "range(%d,%d) 返回 %d 个，期望 %d", start, stop, n, expect);
    for (int k = 0; k < n && k < expect; k++) {
        checked++;
        if (strcmp(r[k], ref[rs + k].name) != 0) {
            CHECK(0, "range(%d,%d) 第 %d 个应为 %s，实际 %s",
                  start, stop, k, ref[rs + k].name, r[k]);
            break;
        }
    }
    free(r);  /* 借用引用数组，只 free 数组本身 */
}

int main(void) {
    printf("=== test_skipspan ===\n");

    Skiplist *sl = skiplist_create();
    CHECK(sl != NULL, "skiplist_create 失败");
    if (!sl) return 1;
    ref = NULL; ref_n = 0; ref_cap = 0;

    /* 确定性随机（可复现） */
    srand(20240917);

    /* ---------- 阶段 A：随机插入 / 更新 ---------- */
    enum { INS = 800 };
    for (int t = 0; t < INS; t++) {
        char name[24];
        /* 名字池小 → 频繁命中更新路径 */
        snprintf(name, sizeof(name), "k%02d", rand() % 90);
        double sc = (double)(rand() % 5000) / 3.0;
        if ((rand() & 3) == 0) sc = 100.0;   /* 制造大量平票 */

        sds m = sdsnew(name);
        CHECK(skiplist_add(sl, m, sc) == 1, "add %s 失败", name);
        ref_add(name, sc);
    }
    CHECK(sl->size == ref_n, "size %d ≠ 参考 %d", sl->size, ref_n);
    check_ranks(sl);
    for (int start = -3; start <= (int)ref_n + 1; start += 7)
        check_range(sl, start, (start % 2) ? -1 : (start + 5));

    /* ---------- 阶段 B：随机删除一半 ---------- */
    int removed = 0;
    for (int t = 0; t < INS && removed < INS / 2; t++) {
        char name[24];
        snprintf(name, sizeof(name), "k%02d", rand() % 90);
        sds q = sdsnew(name);
        int found = ref_find(name) >= 0;
        int del = skiplist_del(sl, q);
        sdsfree(q);
        CHECK(del == (found ? 1 : 0), "del %s：跳表 %d，参考是否存在 %d",
              name, del, found);
        if (found) { removed++; ref_del(name); }
    }
    CHECK(sl->size == ref_n, "删除后 size %d ≠ 参考 %d", sl->size, ref_n);
    check_ranks(sl);
    for (int start = 0; start <= (int)ref_n + 2; start += 5)
        check_range(sl, start, start + 3);

    /* ---------- 阶段 C：清空后再恢复一部分 ---------- */
    for (int t = 0; t < 40; t++) {
        char name[24];
        snprintf(name, sizeof(name), "x%d", t % 15);
        double sc = (double)(t * 3);
        sds m = sdsnew(name);
        skiplist_add(sl, m, sc);
        ref_add(name, sc);
    }
    check_ranks(sl);
    check_range(sl, 0, -1);
    check_range(sl, ref_n - 1, ref_n - 1);

    /* ---------- 阶段 D：O(1) find 与 不存在的 key 的 ranks ---------- */
    for (int i = 0; i < ref_n; i++) {
        sds q = sdsnew(ref[i].name);
        checked++;
        CHECK(skiplist_find(sl, q) != NULL, "find(%s) 应为命中", ref[i].name);
        sdsfree(q);
    }
    {
        sds q = sdsnew("not_exist_key");
        checked++;
        CHECK(skiplist_find(sl, q) == NULL, "find(不存在) 应为 NULL");
        CHECK(skiplist_get_rank(sl, q) == 0, "get_rank(不存在) 应为 0");
        sdsfree(q);
    }

    skiplist_free(sl);
    for (int i = 0; i < ref_n; i++) sdsfree(ref[i].name);
    free(ref);

    /* 至少真的做了对照，防止测试空跑 */
    CHECK(checked > 0, "没有任何对照检查执行");

    if (failures) {
        printf("=== test_skipspan 失败 %d 项 ===\n", failures);
        return 1;
    }
    printf("=== test_skipspan 全部通过（对照 %d 项）===\n", checked);
    return 0;
}
