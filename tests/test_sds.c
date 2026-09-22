/*
 * SDS 单元测试（断言式：有失败则返回非 0 退出码，供 make unit 判定）
 *
 * 覆盖点：
 *   1. 基本创建、O(1) 长度、可当 C 字符串用
 *   2. 二进制安全（数据含 '\0' 不被截断）
 *   3. TYPE_5 边界（len ∈ [16,31]）—— 历史上因有符号 char 算术右移而崩溃的回归点
 *   4. 动态扩容（反复追加，长度与内容都要对）
 *   5. sdsrange 压缩（server.c 用它移动 querybuf 未处理部分）
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sds.h"

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
    printf("=== test_sds ===\n");

    /* ---------- 1. 基本创建与长度 ---------- */
    sds s = sdsnew("hello");
    CHECK(s != NULL, "sdsnew 返回 NULL");
    CHECK(sdslen(s) == 5, "sdslen(\"hello\") 期望 5，实际 %zu", sdslen(s));
    CHECK(strcmp(s, "hello") == 0, "sds 应可直接当 C 字符串用");
    CHECK(sds_type(s) == SDS_TYPE_5, "5 字节应落在 TYPE_5，实际 %d", (int)sds_type(s));

    /* ---------- 2. 二进制安全 ---------- */
    const char raw[] = {'A', '\0', 'x', 'y'};
    sds bin = sdsnewlen(raw, sizeof(raw));
    CHECK(bin != NULL, "sdsnewlen 返回 NULL");
    CHECK(sdslen(bin) == 4, "含 \\0 的数据长度应为 4，实际 %zu", sdslen(bin));
    CHECK(memcmp(bin, raw, 4) == 0, "含 \\0 的数据内容被破坏");
    /* strlen 会停在 '\0'（=1），这正是必须一律用 sdslen 而非 strlen 的原因 */
    CHECK(strlen(bin) == 1, "strlen 应停在 \\0 处得 1，实际 %zu", strlen(bin));

    sds dup = sdsdup(bin);
    CHECK(dup != NULL, "sdsdup 返回 NULL");
    CHECK(dup && sdslen(dup) == 4, "sdsdup 后长度应为 4，实际 %zu", dup ? sdslen(dup) : 0);
    CHECK(dup && memcmp(dup, raw, 4) == 0, "sdsdup 后二进制内容被破坏");

    /* ---------- 3. TYPE_5 边界回归 ---------- */
    /* 历史 bug：len ∈ [16,31] 时 len<<3 ≥ 0x80，作为有符号 char 是负数，
     * s[-1] >> 3 算术右移会得到巨大 size_t，导致 sdsdup 拷贝越界崩溃。 */
    /* ⚠ 源缓冲区必须真的够长：sdsnewlen 会 memcpy initlen 个字节，
     * 拿上面那个 4 字节的 raw 去构造 40 字节的 sds 是栈上越界读（ASan 直接报）。 */
    char src[64];
    memset(src, 'q', sizeof(src));
    for (size_t n = 0; n <= 40; n++) {
        sds t = sdsnewlen(src, n);
        CHECK(t != NULL, "sdsnewlen(len=%zu) 返回 NULL", n);
        if (!t) continue;
        CHECK(sdslen(t) == n, "len=%zu 时 sdslen 应为 %zu，实际 %zu", n, n, sdslen(t));
        sds d = sdsdup(t);
        CHECK(d != NULL, "len=%zu 时 sdsdup 返回 NULL", n);
        if (d) {
            CHECK(sdslen(d) == n, "len=%zu 时 sdsdup 后长度应为 %zu，实际 %zu", n, n, sdslen(d));
            sdsfree(d);
        }
        sdsfree(t);
    }

    /* ---------- 4. 动态扩容 ---------- */
    sds big = sdsempty();
    CHECK(big != NULL, "sdsempty 返回 NULL");
    for (int i = 0; i < 5000; i++) {
        /* 必须用返回值接住：可能 realloc 搬家，旧指针失效 */
        sds nb = sdscatlen(big, "x", 1);
        CHECK(nb != NULL, "第 %d 次 sdscatlen 失败", i);
        if (!nb) break;
        big = nb;
    }
    CHECK(sdslen(big) == 5000, "追加 5000 次后长度应为 5000，实际 %zu", sdslen(big));
    {
        int all_x = 1;
        for (size_t i = 0; i < sdslen(big); i++) {
            if (big[i] != 'x') { all_x = 0; break; }
        }
        CHECK(all_x, "追加后内容不是全 'x'（扩容搬移丢数据）");
    }
    CHECK(sds_type(big) == SDS_TYPE_16, "5000 字节应落在 TYPE_16，实际 %d", (int)sds_type(big));

    /* ---------- 5. sdsrange 压缩 ---------- */
    sds r = sdsnew("0123456789");
    sdsrange(r, 4, -1);
    CHECK(sdslen(r) == 6, "sdsrange(4,-1) 后长度应为 6，实际 %zu", sdslen(r));
    CHECK(strcmp(r, "456789") == 0, "sdsrange(4,-1) 后内容应为 456789，实际 '%s'", r);

    sds b2 = sdsnewlen(raw, 4);
    sdsrange(b2, 1, -1);
    CHECK(sdslen(b2) == 3, "二进制 sdsrange 后长度应为 3，实际 %zu", sdslen(b2));
    CHECK(memcmp(b2, raw + 1, 3) == 0, "二进制 sdsrange 后内容错误");

    sdsfree(s); sdsfree(bin); sdsfree(dup); sdsfree(big); sdsfree(r); sdsfree(b2);

    if (failures) {
        printf("=== test_sds 失败 %d 项 ===\n", failures);
        return 1;
    }
    printf("=== test_sds 全部通过 ===\n");
    return 0;
}
