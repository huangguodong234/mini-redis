/*
 * 哈希表单元测试（断言式）
 *
 * 覆盖点：
 *   1. 插入 / 查询 / 更新 / 删除
 *   2. 自动扩容后所有键仍可查（一次性搬迁最容易出错的地方）
 *   3. 重复 key 更新：size 不增加、旧 value 被释放、新 value 生效
 *   4. 二进制安全：含 '\0' 的键与值
 *
 * ⚠️ 所有权纪律（与 src/ 保持一致）：
 *   - hashtable_set 无论返回 1 还是 0，key/value 的所有权都已转移，
 *     调用方【不得】再 sdsfree 它们；
 *   - hashtable_get 返回的是内部活引用，调用方【不得】free；
 *   - 查询用的临时 sds（如 sdsnew("key_0")）由本测试自己创建并释放。
 *   绝不能把字符串字面量直接传给这些接口——sds 要读 s[-1] 反推 header，
 *   字面量前面没有 header，会读到越界内存。
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hashtable.h"

static int failures = 0;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("  [FAIL] %s:%d: ", __FILE__, __LINE__);         \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

/* 造一个真实 sds（所有权归调用方），避免把字面量当 sds 用 */
static sds mksds(const char *fmt, int i) {
    char buf[64];
    snprintf(buf, sizeof(buf), fmt, i);
    return sdsnew(buf);
}

int main(void) {
    printf("=== test_hashtable ===\n");

    HashTable *ht = hashtable_create(4);
    CHECK(ht != NULL, "hashtable_create 失败");
    if (!ht) return 1;

    /* ---------- 1. 插入并触发扩容 ---------- */
    /* 初始容量 4、负载因子阈值 0.75 → 第 4 次插入（3/4=0.75）触发第一次扩容，
     * 插入 10 个会经历 4→8→16→32 多轮搬迁。 */
    for (int i = 0; i < 10; i++) {
        sds k = mksds("key_%d", i);
        sds v = mksds("val_%d", i);
        CHECK(hashtable_set(ht, k, v) == 1, "插入 key_%d 失败（返回 0）", i);
        /* k/v 所有权已移交哈希表，这里不能 free */
    }
    CHECK(ht->size == 10, "插入 10 个后 size 应为 10，实际 %d", ht->size);
    CHECK(ht->capacity > 4, "负载因子 0.75 应已触发扩容，capacity 实际 %d", ht->capacity);

    /* ---------- 2. 扩容后数据完整性 ---------- */
    for (int i = 0; i < 10; i++) {
        sds k = mksds("key_%d", i);
        char *got = hashtable_get(ht, k);
        CHECK(got != NULL, "扩容后 key_%d 丢失", i);
        if (got) {
            char want[64];
            snprintf(want, sizeof(want), "val_%d", i);
            CHECK(strcmp(got, want) == 0, "key_%d 应为 %s，实际 %s", i, want, got);
        }
        sdsfree(k);
    }

    /* ---------- 3. 重复 key 更新 ---------- */
    {
        sds k = sdsnew("key_0");
        sds v = sdsnew("NEW");
        CHECK(hashtable_set(ht, k, v) == 1, "更新 key_0 失败");
        CHECK(ht->size == 10, "更新已有 key 不应增加 size，实际 %d", ht->size);

        sds q = sdsnew("key_0");
        char *got = hashtable_get(ht, q);
        CHECK(got != NULL && strcmp(got, "NEW") == 0,
              "更新后值应为 NEW，实际 %s", got ? got : "(null)");
        sdsfree(q);
    }

    /* ---------- 4. 删除 ---------- */
    {
        sds k = sdsnew("key_1");
        CHECK(hashtable_del(ht, k) == 1, "删除 key_1 应返回 1");
        CHECK(hashtable_del(ht, k) == 0, "重复删除 key_1 应返回 0");
        CHECK(hashtable_get(ht, k) == NULL, "删除后 key_1 仍可被查到");
        sdsfree(k);
    }

    /* ---------- 5. 二进制安全 ---------- */
    {
        const char kraw[] = {'k', '\0', '1'};
        const char vraw[] = {'v', '\0', '2'};
        CHECK(hashtable_set(ht, sdsnewlen(kraw, 3), sdsnewlen(vraw, 3)) == 1,
              "插入含 \\0 的键失败");

        sds q = sdsnewlen(kraw, 3);
        char *got = hashtable_get(ht, q);
        CHECK(got != NULL, "含 \\0 的键查不到（哈希或比较被 \\0 截断）");
        if (got) {
            CHECK(sdslen(got) == 3, "含 \\0 的值长度应为 3，实际 %zu", sdslen(got));
            CHECK(memcmp(got, vraw, 3) == 0, "含 \\0 的值内容被破坏");
        }
        sdsfree(q);
    }

    hashtable_free(ht);

    if (failures) {
        printf("=== test_hashtable 失败 %d 项 ===\n", failures);
        return 1;
    }
    printf("=== test_hashtable 全部通过 ===\n");
    return 0;
}
