/*
 * 有序集合（ZSet）单元测试（断言式）
 *
 * 这一层是"所有权边界"最关键的一层，重点验证两件事：
 *   A. zset_add 是 steal 语义：传进去的 sds 所有权移交跳表，调用方【不得】再 free；
 *      同分路径（跳表不存储、内部 sdsfree）也不能让调用方 double-free。
 *   B. zset_range 返回的是【可拥有的深拷贝】：调用方必须逐个 sdsfree 再 free 数组，
 *      且与跳表内部数据无关（改了表也不影响已返回的副本）。
 *
 * ⚠️ 绝不能把字符串字面量直接当 sds 传（sds 要读 s[-1] 反推 header）。
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zset.h"

static int failures = 0;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("  [FAIL] %s:%d: ", __FILE__, __LINE__);         \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

int main(void) {
    printf("=== test_zset ===\n");

    ZSet *zs = zset_create();
    CHECK(zs != NULL, "zset_create 失败");
    if (!zs) return 1;

    /* ---------- 1. ZADD（steal 语义） ---------- */
    /* m0..m4 的 score 依次为 5,4,3,2,1（故意与名字顺序相反） */
    for (int i = 0; i < 5; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "m%d", i);
        sds m = sdsnew(buf);
        CHECK(zset_add(zs, m, (double)(5 - i)) == 1, "zset_add %s 失败", buf);
        /* 注意：这里【不能】sdsfree(m) —— 所有权已移交跳表 */
    }

    /* ---------- 2. ZRANGE：按 score 升序，且返回值是深拷贝 ---------- */
    sds *owned = NULL;
    {
        sds *r = zset_range(zs, 0, -1);
        CHECK(r != NULL, "zset_range 返回 NULL");
        if (r) {
            int n = 0;
            while (r[n]) n++;
            CHECK(n == 5, "range 应有 5 个成员，实际 %d", n);

            const char *expect[5] = {"m4", "m3", "m2", "m1", "m0"};
            for (int i = 0; i < n && i < 5; i++) {
                CHECK(strcmp(r[i], expect[i]) == 0,
                      "第 %d 位应为 %s，实际 %s", i, expect[i], r[i]);
            }
            /* 先留住一份，等下面改动跳表后再验证它不受影响 */
            owned = r;
        }
    }

    /* ---------- 3. ZSCORE ---------- */
    {
        bool found = false;
        sds q = sdsnew("m0");
        double sc = zset_score(zs, q, &found);
        CHECK(found, "m0 应该存在（found=false）");
        CHECK(sc == 5.0, "m0 的 score 应为 5，实际 %f", sc);
        sdsfree(q);

        bool miss = true;
        sds q2 = sdsnew("nope");
        (void)zset_score(zs, q2, &miss);
        CHECK(!miss, "不存在的成员 found 应为 false");
        sdsfree(q2);
    }

    /* ---------- 4. ZREM ---------- */
    {
        sds rm = sdsnew("m0");
        CHECK(zset_rem(zs, rm) == 1, "删除 m0 应返回 1");
        CHECK(zset_rem(zs, rm) == 0, "重复删除 m0 应返回 0");
        sdsfree(rm);
    }

    /* ---------- 5. 同分 ZADD：跳表不存储，内部 sdsfree 传入的 member ---------- */
    /*   这是 steal 契约最容易写错的路径：若调用方再去 free，就是 double-free。 */
    {
        sds m = sdsnew("m1");          /* m1 现有 score 就是 4 */
        CHECK(zset_add(zs, m, 4.0) == 1, "同分 ZADD 应返回成功");
        /* m 已被跳表 sdsfree，此后绝不能再碰它 */
    }

    /* ---------- 6. 深拷贝验证：上面改了表，之前那份副本必须原样 ---------- */
    if (owned) {
        int n = 0;
        while (owned[n]) n++;
        CHECK(n == 5, "副本个数应仍为 5（不受表中删除影响），实际 %d", n);
        if (n == 5) {
            CHECK(strcmp(owned[0], "m4") == 0, "副本第 0 位应仍为 m4，实际 %s", owned[0]);
            CHECK(strcmp(owned[4], "m0") == 0, "副本第 4 位应仍为 m0，实际 %s", owned[4]);
        }
        /* zset_range 返回可拥有的副本 → 逐个释放，再释放数组 */
        for (int i = 0; i < n; i++) sdsfree(owned[i]);
        free(owned);
    }

    zset_free(zs);

    if (failures) {
        printf("=== test_zset 失败 %d 项 ===\n", failures);
        return 1;
    }
    printf("=== test_zset 全部通过 ===\n");
    return 0;
}
