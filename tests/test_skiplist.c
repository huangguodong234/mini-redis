/*
 * 跳表单元测试（断言式）
 *
 * 覆盖点：
 *   1. 乱序插入后第 0 层必须按 score 升序
 *   2. 随机压力（500 个成员）：排序不破、成员不丢
 *   3. 删除：返回值、size、删除后不可查
 *   4. 更新分数：会删除旧节点重新插入，顺序随之变化
 *
 * ⚠️ 所有权纪律：
 *   - skiplist_add 无论返回 1 还是 0，member 的所有权都已转移（成功→跳表持有，
 *     失败/同分→函数内已 sdsfree），调用方【不得】再碰；
 *   - skiplist_range 返回的是【借用引用】数组：只能 free(数组)，
 *     元素归跳表所有，绝不能 sdsfree。
 *   绝不能把字符串字面量直接当 sds 传（sds 要读 s[-1] 反推 header）。
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skiplist.h"

static int failures = 0;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("  [FAIL] %s:%d: ", __FILE__, __LINE__);         \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

/* 校验 range 结果：长度 == size、score 非降序、每个成员都真在表里 */
static void check_range_sorted(Skiplist *sl, const char *tag) {
    sds *r = skiplist_range(sl, 0, -1);
    CHECK(r != NULL, "%s: skiplist_range 返回 NULL", tag);
    if (!r) return;

    int n = 0;
    while (r[n]) n++;
    CHECK(n == sl->size, "%s: range 返回 %d 个成员，size 为 %d", tag, n, sl->size);

    double prev = -1e300;
    int sorted = 1, complete = 0;
    for (int i = 0; i < n; i++) {
        SkipNode *nd = skiplist_find(sl, r[i]);
        if (!nd) { complete++; continue; }
        if (nd->score < prev) sorted = 0;
        prev = nd->score;
    }
    CHECK(sorted, "%s: range 输出的 score 不是升序（跳表排序被破坏）", tag);
    CHECK(complete == 0, "%s: range 里有 %d 个成员在跳表中查不到", tag, complete);

    free(r);   /* 只释放数组本身，元素是借用引用 */
}

int main(void) {
    printf("=== test_skiplist ===\n");

    Skiplist *sl = skiplist_create();
    CHECK(sl != NULL, "skiplist_create 失败");
    if (!sl) return 1;

    /* ---------- 1. 乱序插入后的显式顺序 ---------- */
    {
        const char *names[6] = {"a", "b", "c", "d", "e", "f"};
        const double scores[6] = {30, 10, 50, 20, 40, 5};
        for (int i = 0; i < 6; i++) {
            CHECK(skiplist_add(sl, sdsnew(names[i]), scores[i]) == 1,
                  "插入 %s 失败", names[i]);
        }
        CHECK(sl->size == 6, "插入 6 个后 size 应为 6，实际 %d", sl->size);

        sds *r = skiplist_range(sl, 0, -1);
        CHECK(r != NULL, "range 返回 NULL");
        if (r) {
            /* 按 score 升序：f(5) b(10) d(20) a(30) e(40) c(50) */
            const char *expect[6] = {"f", "b", "d", "a", "e", "c"};
            int n = 0;
            while (r[n]) n++;
            CHECK(n == 6, "range 应返回 6 个，实际 %d", n);
            for (int i = 0; i < n && i < 6; i++) {
                CHECK(strcmp(r[i], expect[i]) == 0,
                      "第 %d 位应为 %s，实际 %s", i, expect[i], r[i]);
            }
            free(r);
        }
    }

    /* ---------- 2. 随机压力 ---------- */
    {
        enum { N = 500 };
        char name[16];
        srand(12345);   /* 固定种子，保证可复现 */
        for (int i = 0; i < N; i++) {
            snprintf(name, sizeof(name), "m%04d", i);
            double sc = (double)(rand() % 100000) / 7.0;
            CHECK(skiplist_add(sl, sdsnew(name), sc) == 1, "插入 %s 失败", name);
        }
        CHECK(sl->size == 6 + N, "size 应为 %d，实际 %d", 6 + N, sl->size);
        check_range_sorted(sl, "随机压力后");
    }

    /* ---------- 3. 删除 ---------- */
    {
        sds k = sdsnew("m0000");
        CHECK(skiplist_del(sl, k) == 1, "删除 m0000 应返回 1");
        CHECK(skiplist_del(sl, k) == 0, "重复删除 m0000 应返回 0");
        CHECK(skiplist_find(sl, k) == NULL, "删除后 m0000 仍可被查到");
        sdsfree(k);
        check_range_sorted(sl, "删除后");
    }

    /* ---------- 4. 更新分数会重新排序 ---------- */
    {
        /* "a" 原有 score 30；改成 1e9 后应被移到队尾。
         * ⚠ 这里必须用"保证大于所有随机分数"的值：随机成员的 score 最大约
         * 99999/7 ≈ 14285，用 9999 会被随机成员盖过，断言就会偶发失败。 */
        CHECK(skiplist_add(sl, sdsnew("a"), 1e9) == 1, "更新 a 的分数失败");

        sds q = sdsnew("a");
        SkipNode *nd = skiplist_find(sl, q);
        CHECK(nd != NULL, "更新后查不到 a");
        CHECK(nd && nd->score == 1e9, "更新后 a 的 score 应为 1e9，实际 %f",
              nd ? nd->score : 0.0);
        sdsfree(q);

        /* 更新不应改变 size（先删旧节点再插新节点） */
        CHECK(sl->size == 6 + 500 - 1, "更新分数不应改变 size，实际 %d", sl->size);

        sds *r = skiplist_range(sl, 0, -1);
        if (r) {
            int n = 0;
            while (r[n]) n++;
            CHECK(n > 0 && strcmp(r[n - 1], "a") == 0,
                  "score 1e9 的 a 应排在最后一位，实际最后是 %s",
                  n > 0 ? r[n - 1] : "(空)");
            free(r);
        }
    }

    skiplist_free(sl);

    if (failures) {
        printf("=== test_skiplist 失败 %d 项 ===\n", failures);
        return 1;
    }
    printf("=== test_skiplist 全部通过 ===\n");
    return 0;
}
