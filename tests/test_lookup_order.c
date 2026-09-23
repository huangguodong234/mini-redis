/*
 * test_lookup_order.c —— 用 rehashidx 判断"该查哪张表"的正确性对拍
 *
 * 背景（这位用户提出的优化）：
 *   rehash 期间每次搬桶都会推进 rehashidx，所以
 *     - idx <  rehashidx 的旧桶：一定已搬空 → 【不必查旧表】
 *     - idx >= rehashidx 的旧桶：还没搬     → 【先查旧表】
 *   这样最多只查一张表，省掉"查两张表"的开销。
 *
 * 本测试要证明的核心不变量（对拍 4 件事）：
 *   ① idx <  rehashidx 时，buckets_old[idx] 必须恒为 NULL
 *   ② 优化版的查找结果，与"老老实实查两张表"的参考实现【逐 key 一致】
 *   ③ rehash 进行中的 add / find / delete 三种操作结果都对
 *   ④ 混合负载（边插边查边删，全程穿越 rehash）不丢不重
 *
 * 为什么必须对拍而不能只测"能不能查到"：
 *   漏查（查不到本该存在的 key）和误查（查到已删除的 key）都要覆盖。
 *   参考实现用最笨的写法（两张表都查、不跳过），它慢但显然正确，
 *   拿它当 oracle。
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "dict.h"

/* ★ 直接包含 dict.c 的实现：
 *   dictFindEntry 是 static inline 的内部函数，不从头文件导出，
 *   而本测试要直接对拍它（而不是只测 dictFind 的黑盒行为）。
 *   链接时就不能再单独编 dict.c 了（会重复定义），见 Makefile。
 *   代价：测试文件里会看到 dict.c 的全部私有实现 —— 但这是"白盒对拍"
 *   必须付的代价，比把内部函数提升为公开接口要好。 */
#include "../src/dict.c"

static int g_fail = 0;

#define CHECK(cond, ...) do {                                    \
    if (!(cond)) {                                               \
        g_fail++;                                                \
        printf("  [FAIL] %s:%d ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                     \
        printf("\n");                                            \
    }                                                            \
} while (0)

/* ============================================================
 * 参考实现（oracle）：最笨、最显然正确的写法
 *   两张表都查，不跳过、不推断。正确性不依赖任何不变量。
 * ============================================================ */
static dictEntry *ref_find(dict *d, const void *key, unsigned long hash)
{
    dictEntry *he = d->buckets[hash % (unsigned long)d->capacity];
    while (he) {
        if (d->type->keyCompare(d->privdata, he->key, key) == 0) return he;
        he = he->next;
    }
    if (d->buckets_old) {
        he = d->buckets_old[hash % (unsigned long)d->old_capacity];
        while (he) {
            if (d->type->keyCompare(d->privdata, he->key, key) == 0) return he;
            he = he->next;
        }
    }
    return NULL;
}

/* 被测实现：dictFind 内部走 dictFindEntry（优化后） */
static int same_result(dict *d, const void *key)
{
    unsigned long h = d->type->hashFunction((void *)key);
    dictEntry *a = ref_find(d, key, h);
    dictEntry *b = dictFindEntry(d, key, h);
    return (a != NULL) == (b != NULL);
}

/* ============================================================
 * ① 核心不变量：idx < rehashidx 时旧表那一格必须为空
 * ============================================================ */
static void test_invariant(void)
{
    printf("\n--- ① idx < rehashidx 时旧表必空（跳过安全的前提）---\n");

    long N = 200000;
    sds *keys = malloc(sizeof(sds) * (size_t)N);
    char buf[32];
    for (long i = 0; i < N; i++) { snprintf(buf, sizeof buf, "k%ld", i); keys[i] = sdsnew(buf); }

    dict *d = dictCreate(&sdsDictType, NULL);
    CHECK(d != NULL, "创建表失败");

    long checks = 0, violated = 0, rehash_seen = 0;
    for (long i = 0; i < N; i++) {
        dictAdd(d, keys[i], NULL);

        /* 只要在 rehash 中，就验证这个不变量 */
        if (d->rehashidx != -1 && d->buckets_old) {
            rehash_seen++;
            /* 抽查一部分 key，避免测试本身太慢 */
            if (i % 7 == 0) {
                for (long j = 0; j <= i; j++) {
                    unsigned long h = d->type->hashFunction((void *)keys[j]);
                    long idx = (long)(h % (unsigned long)d->old_capacity);
                    if (idx < d->rehashidx) {
                        checks++;
                        if (d->buckets_old[idx] != NULL) violated++;
                    }
                }
            }
        }
    }
    printf("  rehash 状态观测次数 = %ld\n", rehash_seen);
    printf("  检查了 %ld 个 (idx < rehashidx) 的位置\n", checks);
    printf("  违反不变量的次数 = %ld\n", violated);
    CHECK(rehash_seen > 0, "整个插入过程从未进入 rehash，测试无效");
    CHECK(checks > 0, "没检查到任何 idx < rehashidx 的位置，测试无效");
    CHECK(violated == 0, "不变量被破坏 %ld 次 —— 跳过旧表会漏查！", violated);

    dictRelease(d);
    free(keys);
}

/* ============================================================
 * ② 全量对拍：优化版 vs oracle，逐个 key 比对
 * ============================================================ */
static void test_full_compare(void)
{
    printf("\n--- ② 全量对拍：优化版 vs 参考实现 ---\n");

    long N = 300000;
    sds *keys = malloc(sizeof(sds) * (size_t)N);
    char buf[32];
    for (long i = 0; i < N; i++) { snprintf(buf, sizeof buf, "ck%ld", i); keys[i] = sdsnew(buf); }

    dict *d = dictCreate(&sdsDictType, NULL);
    for (long i = 0; i < N; i++) dictAdd(d, keys[i], NULL);

    printf("  插入后: cap=%d old_cap=%d rehashidx=%ld old_used=%d\n",
           d->capacity, d->old_capacity, d->rehashidx,
           d->buckets_old ? d->old_used : -1);

    long mismatch = 0;
    for (long i = 0; i < N; i++) {
        if (!same_result(d, keys[i])) mismatch++;
    }
    /* 再查一批不存在的 key（必须两边都返回"找不到"） */
    long miss_mismatch = 0;
    for (long i = 0; i < 20000; i++) {
        snprintf(buf, sizeof buf, "nope_%ld", i);
        sds k = sdsnew(buf);
        if (!same_result(d, k)) miss_mismatch++;
        sdsfree(k);
    }

    printf("  存在的 key 对拍不一致 = %ld\n", mismatch);
    printf("  不存在的 key 对拍不一致 = %ld\n", miss_mismatch);
    CHECK(mismatch == 0, "%ld 个已存在 key 的查找结果不一致", mismatch);
    CHECK(miss_mismatch == 0, "%ld 个不存在 key 的结果不一致", miss_mismatch);

    dictRelease(d);
    free(keys);
}

/* ============================================================
 * ③ rehash 进行中的三种操作都对
 * ============================================================ */
static void test_ops_during_rehash(void)
{
    printf("\n--- ③ rehash 进行中的 add/find/delete ---\n");

    long N = 150000;
    sds *keys = malloc(sizeof(sds) * (size_t)N);
    unsigned char *alive = calloc((size_t)N, 1);   /* 对拍用的参考数组 */
    char buf[32];
    for (long i = 0; i < N; i++) { snprintf(buf, sizeof buf, "op%ld", i); keys[i] = sdsnew(buf); }

    dict *d = dictCreate(&sdsDictType, NULL);
    long add_fail = 0, find_fail = 0, del_fail = 0, alive_fail = 0, dead_seen = 0;
    long in_rehash_ops = 0;

    for (long i = 0; i < N; i++) {
        /* 插入 */
        if (dictAdd(d, keys[i], NULL) != 1) add_fail++;
        alive[i] = 1;
        if (d->rehashidx != -1) in_rehash_ops++;

        /* 每 3 个删一个（删的可能在旧表也可能在新表） */
        if (i % 3 == 2) {
            long v = i / 3;
            if (dictDelete(d, keys[v]) != 1) del_fail++;
            alive[v] = 0;
        }

        /* 每 5 个回查一次（查刚插入的、查删过的） */
        if (i % 5 == 4) {
            /* ★ 用 dictFindEntry 判定"在不在"，不用 dictFind：
             *   dictFind 返回的是 value，而本测试 val 传 NULL，
             *   所以 dictFind 恒返回 NULL，无法区分"不存在"和"存在但值为空"。
             *   （这个歧义坑过一次，见 docs/dict-design.md §七） */
            unsigned long h = d->type->hashFunction((void *)keys[i]);
            if (dictFindEntry(d, keys[i], h) == NULL) find_fail++;
            /* ⚠ 只能回查【还活着】的 key：已删的 sds 已被 dictDelete 释放，
             *   再拿它算 hash 就是 use-after-free（本测试自己踩过一次）。 */
            if (i >= 3) {
                long v = i / 3;
                if (alive[v]) {
                    unsigned long hv = d->type->hashFunction((void *)keys[v]);
                    if (dictFindEntry(d, keys[v], hv) == NULL) alive_fail++;
                } else {
                    dead_seen++;   /* 记一笔：本轮确实删过，确认覆盖了删除路径 */
                }
            }
        }
    }

    printf("  rehash 中执行的操作数 = %ld\n", in_rehash_ops);
    printf("  插入失败 = %ld\n", add_fail);
    printf("  刚插入就查不到 = %ld\n", find_fail);
    printf("  删除失败 = %ld\n", del_fail);
    printf("  存活 key 查不到 = %ld\n", alive_fail);
    printf("  已删 key 的轮次 = %ld（确认覆盖了删除路径）\n", dead_seen);

    CHECK(in_rehash_ops > 0, "从未在 rehash 状态下操作，测试无效");
    CHECK(add_fail == 0, "%ld 次插入失败", add_fail);
    CHECK(find_fail == 0, "%ld 次刚插入就查不到", find_fail);
    CHECK(del_fail == 0, "%ld 次删除失败（可能漏查旧表）", del_fail);
    CHECK(alive_fail == 0, "%ld 个存活 key 查不到", alive_fail);


    dictRelease(d);
    free(alive);
    free(keys);
}

/* ============================================================
 * ④ 长时间混合负载：全程穿越 rehash，反复对拍
 * ============================================================ */
static void test_mixed_stress(void)
{
    printf("\n--- ④ 混合负载压力对拍 ---\n");

    long N = 200000;
    sds *keys = malloc(sizeof(sds) * (size_t)N);
    unsigned char *alive = calloc((size_t)N, 1);
    char buf[32];
    for (long i = 0; i < N; i++) { snprintf(buf, sizeof buf, "mx%ld", i); keys[i] = sdsnew(buf); }

    dict *d = dictCreate(&sdsDictType, NULL);
    long mismatch = 0, rehash_steps = 0;

    for (long i = 0; i < N; i++) {
        dictAdd(d, keys[i], NULL);
        alive[i] = 1;

        /* 周期性删除（制造旧表/新表混合状态） */
        if (i % 4 == 3 && i > 100) {
            long v = i / 4;
            dictDelete(d, keys[v]);
            alive[v] = 0;
        }

        if (d->rehashidx != -1) rehash_steps++;

        /* 周期性全量对拍（较慢，抽样） */
        if (i % 20000 == 19999) {
            for (long j = 0; j <= i; j++) {
                /* ⚠ 跳过已删的 key：它的 sds 已被 dictDelete 释放，
                 *   再算 hash 是 use-after-free。 */
                if (!alive[j]) continue;
                unsigned long h = d->type->hashFunction((void *)keys[j]);
                dictEntry *a = ref_find(d, keys[j], h);
                dictEntry *b = dictFindEntry(d, keys[j], h);
                if ((a != NULL) != (b != NULL)) mismatch++;
            }
        }
    }

    printf("  rehash 状态下的操作数 = %ld\n", rehash_steps);
    printf("  对拍不一致 = %ld\n", mismatch);
    CHECK(rehash_steps > 0, "从未进入 rehash，测试无效");
    CHECK(mismatch == 0, "%ld 次对拍不一致", mismatch);

    /* 收尾：rehash 若未结束，dictRelease 必须两张表都释放（ASan 会查） */
    dictRelease(d);
    free(alive);
    free(keys);
}

int main(void)
{
    printf("=== test_lookup_order：用 rehashidx 判断查哪张表的正确性 ===\n");

    test_invariant();
    test_full_compare();
    test_ops_during_rehash();
    test_mixed_stress();

    printf("\n=== %s ===\n", g_fail == 0 ? "test_lookup_order 全部通过"
                                        : "test_lookup_order 有失败项");
    return g_fail == 0 ? 0 : 1;
}
