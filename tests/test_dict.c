/*
 * 通用哈希表（dict）单元测试（断言式）
 *
 * 覆盖点：
 *   1. 插入 / 查询 / 更新 / 删除
 *   2. 自动扩容后所有键仍可查（一次性搬迁最容易出错的地方）
 *   3. 重复 key 更新：size 不增加、新 value 生效
 *   4. 二进制安全：含 '\0' 的键与值
 *   5. ★ 通用性（本轮新增，这是改造的目的所在）：
 *      a. 同一份 dict 核心代码，换一张 dictType 就能存
 *         【非 sds】的 value（本测试存一个自定义 struct 指针）；
 *      b. 【借用语义】的 dictType（destructor 空操作）删除/销毁时
 *         不会去 free key/value——跳表索引就靠这条才能
 *         把属于自己的 member/SkipNode 放进去。
 *
 * ⚠️ 所有权纪律（与 src/ 保持一致）：
 *   - dictAdd 【不拷贝也不释放】key/value：成功后它们归谁由
 *     d->type 的 destructor 决定；失败时也不释放（通用表不知道它们归谁），
 *     归还责任在调用方。
 *   - dictFind 返回的是内部活引用，调用方【不得】free；
 *   - 查询用的临时 sds（如 sdsnew("key_0")）由本测试自己创建并释放。
 *   绝不能把字符串字面量直接传给这些接口——sds 要读 s[-1] 反推 header，
 *   字面量前面没有 header，会读到越界内存。
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dict.h"

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

/* ============================================================
 * 第 6 节用的“非 sds”策略函数：证明 dict 与类型无关
 * ============================================================
 * 这些函数对应一张假想的「整数 key + 自定义对象 value」的 dictType。
 * 它们和 src/dict.c 里的 sdsDictType 是平级关系——同一份
 * dict 核心代码，两套完全不同的类型/销毁语义。 */

/* 模拟跳表节点：一个被别处（栈上）拥有的对象 */
typedef struct { double score; } FakeObj;

/* 整数 key 的哈希：直接用值本身（对整数而言比 djb2 更快且无冲突） */
static unsigned long intKeyHash(const void *key)
{
    return (unsigned long)(*(const int *)key);
}

/* 整数 key 的比较：相等返回 0（同 strcmp 语义） */
static int intKeyCompare(void *privdata, const void *k1, const void *k2)
{
    (void)privdata;
    int a = *(const int *)k1;
    int b = *(const int *)k2;
    return (a > b) - (a < b);
}

/* 空操作 destructor：借用语义——知道要销毁，但故意不做事。
 * （跳表索引的 skipIndexDictType 用的就是这个手法。） */
static void noopKeyDestructor(void *privdata, void *key) { (void)privdata; (void)key; }
static void noopValDestructor(void *privdata, void *val) { (void)privdata; (void)val; }

int main(void) {
    printf("=== test_dict ===\n");

    /* ★ 初始容量由 dict 内部定（DICT_INITIAL_CAPACITY = 16）。
     * 要触发扩容，往里面塞够多键即可（负载因子 0.75）。
     * 挂 sdsDictType：拥有 key/value，销毁即 sdsfree。 */
    dict *ht = dictCreate(&sdsDictType, NULL);
    CHECK(ht != NULL, "dictCreate 失败");
    if (!ht) return 1;

    int cap_before = ht->capacity;

    /* ---------- 1. 插入并触发扩容 ---------- */
    for (int i = 0; i < 40; i++) {
        sds k = mksds("key_%d", i);
        sds v = mksds("val_%d", i);
        CHECK(dictAdd(ht, k, v) == 1, "插入 key_%d 失败（返回 0）", i);
        /* k/v 所有权已移交哈希表（sdsDictType 会 sdsfree），这里不能 free */
    }
    CHECK(dictSize(ht) == 40, "插入 40 个后 size 应为 40，实际 %d", dictSize(ht));
    CHECK(ht->capacity > cap_before, "负载因子 0.75 应已触发扩容，capacity 实际 %d", ht->capacity);

    /* ---------- 2. 扩容后数据完整性 ---------- */
    for (int i = 0; i < 40; i++) {
        sds k = mksds("key_%d", i);
        char *got = (char *)dictFind(ht, k);
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
        /* ★ 关键区别：通用的 dictAdd 不会替调用方释放旧 value
         *（它不知道旧 value 归谁），所以“先探旧值再释放”的１行
         * 必须由调用方（storage_set_steal）自己写。这里演示同样的写法。 */
        sds k = sdsnew("key_0");
        sds v = sdsnew("NEW");
        void *old = dictFind(ht, k);
        if (old) sdsfree((sds)old);
        CHECK(dictAdd(ht, k, v) == 1, "更新 key_0 失败");
        CHECK(dictSize(ht) == 40, "更新已有 key 不应增加 size，实际 %d", dictSize(ht));

        /* ★ 这条 sdsfree 是 ASan 抓出来的：key 已存在时，dictAdd 只更新 value、
         *   把新 key 原样返回给调用方（表里那条 key 才是 owner），所以查询用的
         *   这份 k 必须由本测试释放。原先 hashtable_set 会替我们 sdsfree(key)，
         *   通用版不再这么做——它不知道 k 是"多出来的一份"还是"借用的 member"。
         *   （storage 侧不受影响：它每次 SET 传的都是新的 key，不会重复。） */
        sdsfree(k);

        sds q = sdsnew("key_0");
        char *got = (char *)dictFind(ht, q);
        CHECK(got != NULL && strcmp(got, "NEW") == 0,
              "更新后值应为 NEW，实际 %s", got ? got : "(null)");
        sdsfree(q);
    }

    /* ---------- 4. 删除 ---------- */
    {
        sds k = sdsnew("key_1");
        CHECK(dictDelete(ht, k) == 1, "删除 key_1 应返回 1");
        CHECK(dictDelete(ht, k) == 0, "重复删除 key_1 应返回 0");
        CHECK(dictFind(ht, k) == NULL, "删除后 key_1 仍可被查到");
        sdsfree(k);
    }

    /* ---------- 5. 二进制安全 ---------- */
    {
        const char kraw[] = {'k', '\0', '1'};
        const char vraw[] = {'v', '\0', '2'};
        CHECK(dictAdd(ht, sdsnewlen(kraw, 3), sdsnewlen(vraw, 3)) == 1,
              "插入含 \\0 的键失败");

        sds q = sdsnewlen(kraw, 3);
        char *got = (char *)dictFind(ht, q);
        CHECK(got != NULL, "含 \\0 的键查不到（哈希或比较被 \\0 截断）");
        if (got) {
            CHECK(sdslen(got) == 3, "含 \\0 的值长度应为 3，实际 %zu", sdslen(got));
            CHECK(memcmp(got, vraw, 3) == 0, "含 \\0 的值内容被破坏");
        }
        sdsfree(q);
    }

    dictRelease(ht);   /* 按 sdsDictType 释放所有残留的 sds 键值 */

    /* ---------- 6. ★ 通用性：同一份 dict 代码，换一张 dictType ---------- */
    /* 这一段是本轮改造的目的所在：验证 dict 真正与类型无关。
     * 我们用一张「整数 key + 任意 struct 指针 value」的 dictType，
     * 而且 destructor 全为空操作（借用语义）——模拟跳表索引的用法。
     * 如果 dict 核心里还残留任何 sds 假设（比如直接调 sdsfree），
     * 这里会立刻崩或漏。 */
    {
        /* 借用语义：销毁时什么都不做（对应 skipIndexDictType） */
        dictType borrowType = {
            intKeyHash,                 /* hashFunction  : 整数 key */
            intKeyCompare,              /* keyCompare    : 整数比较 */
            NULL,                       /* keyFromU64    : 本测试直接用 hashFunction */
            NULL, NULL,                 /* keyDup/valDup : 不拷贝 */
            noopKeyDestructor,          /* ★ 借用 → 不释放 */
            noopValDestructor,          /* ★ 借用 → 不释放 */
        };

        dict *borrow = dictCreate(&borrowType, NULL);
        CHECK(borrow != NULL, "创建借用语义的整数 key 字典失败");
        if (borrow) {
            /* key 是整数的地址，value 是一个自定义结构指针。
             * 它们都由本测试拥有（栈上），故 destructor 必须是空操作。 */
            static int keys[3] = {100, 200, 300};
            static FakeObj objs[3];
            for (int i = 0; i < 3; i++) {
                objs[i].score = (double)(i + 1) * 1.5;
                CHECK(dictAdd(borrow, &keys[i], &objs[i]) == 1, "插入整数 key %d 失败", keys[i]);
            }
            CHECK(dictSize(borrow) == 3, "整数 key 字典 size 应为 3，实际 %d", dictSize(borrow));

            /* 取出来的 value 应该就是原来那个对象的地址（指针身份，不是拷贝） */
            FakeObj *got = (FakeObj *)dictFind(borrow, &keys[1]);
            CHECK(got == &objs[1], "取回的应是原对象地址（借用语义不拷贝）");
            CHECK(got && got->score == 3.0, "对象内容应为 3.0，实际 %.1f", got ? got->score : -1.0);

            /* ★ 删除：整数 key 不被 free（它在静态数组里）、
             *   FakeObj 不被 free（它在栈上）——全靠 destructor 是空操作。
             *   若 dict 里还硬编码着 sdsfree，这里会崩在非 sds 指针上。 */
            CHECK(dictDelete(borrow, &keys[1]) == 1, "删除整数 key 200 失败");
            CHECK(dictFind(borrow, &keys[1]) == NULL, "删除后不应再查到");
            CHECK(objs[1].score == 3.0, "借用语义下 value 不应被动到");

            /* 销毁整表：剩下的 key/value 也不应被 free */
            dictRelease(borrow);
            CHECK(objs[0].score == 1.5 && objs[2].score == 4.5,
                  "借用语义下 dictRelease 不应动到 key/value");
        }
    }

    /* ---------- 7. ★ 渐进式 rehash：搬迁【进行中】的正确性 ----------
     * 这是最容易出 bug 的地方：搬迁期间数据分散在两张表里，任何一处
     * 忘了看旧表，都会表现成"key 时有时无"这类极难复现的问题。
     * 本节专门在"搬迁中"这个状态下做增删查，对拍参考数组。 */
    {
        enum { M = 20000 };
        dict *rd = dictCreate(&sdsDictType, NULL);
        CHECK(rd != NULL, "创建 rehash 测试表失败");
        if (rd) {
            /* 参考数组：ref[i] != NULL 表示 key_i 应该存在 */
            char name[32];
            sds *ref = calloc(M, sizeof(sds));
            CHECK(ref != NULL, "calloc ref 失败");

            long mismatch_add = 0, mismatch_find = 0, mismatch_del = 0;
            long seen_rehashing = 0;

            for (int i = 0; i < M; i++) {
                snprintf(name, sizeof(name), "rk_%d", i);
                sds k = sdsnew(name);
                if (sdslen(k) == 0) { sdsfree(k); continue; }

                /* 中途插入：模拟"新 key 进新表"，同时确保搬迁仍在进行。
                 * ⚠ value 必须是真的 sds：sdsDictType 的 valDestructor 是
                 *   sdsfree，传一个假指针（如 (void*)1）会在删除时崩在
                 *   sds_type() 读 s[-1] 上。这是「策略表决定怎么销毁」的直接后果—
                 *   ——用了 sdsDictType，就必须传真的、能 sdsfree 的东西。 */
                sds v = sdsnew("v");
                if (dictAdd(rd, k, v) != 1) {
                    sdsfree(k); sdsfree(v);
                    mismatch_add++;
                } else {
                    ref[i] = k;   /* key/v 所有权已归 dict，仅记录"存在" */
                }
                if (rd->rehashidx != -1) seen_rehashing++;

                /* 搬迁中查自己 */
                sds q = sdsnew(name);
                void *got = dictFind(rd, q);
                if (got == NULL) mismatch_find++;
                sdsfree(q);
            }

            /* 搬迁仍在中途 → 此时两种表都有数据，正是要测的状态 */
            CHECK(seen_rehashing > 0, "整个过程中从未进入 rehash 状态，测试无效");

            /* 搬迁中【反向】删除一半（删的可能在旧表也可能在新表） */
            for (int i = 0; i < M; i += 2) {
                snprintf(name, sizeof(name), "rk_%d", i);
                sds q = sdsnew(name);
                int r = dictDelete(rd, q);
                if (r != 1) mismatch_del++;
                if (ref[i]) { ref[i] = NULL; }   /* 已删，不再持有 */
                sdsfree(q);
            }

            /* 搬迁中查全部：删过的应查不到，没删的应查得到 */
            long wrong_alive = 0, wrong_dead = 0;
            for (int i = 0; i < M; i++) {
                snprintf(name, sizeof(name), "rk_%d", i);
                sds q = sdsnew(name);
                void *got = dictFind(rd, q);
                if (ref[i] && got == NULL) wrong_alive++;
                if (!ref[i] && got != NULL)     wrong_dead++;
                sdsfree(q);
            }

            CHECK(mismatch_add == 0, "搬迁中有 %ld 次插入失败", mismatch_add);
            CHECK(mismatch_find == 0, "搬迁中有 %ld 次刚插入就查不到（新表/旧表切换 bug）", mismatch_find);
            CHECK(mismatch_del == 0, "搬迁中有 %ld 次删除失败（漏查旧表）", mismatch_del);
            CHECK(wrong_alive == 0, "搬迁中有 %ld 个存活 key 查不到", wrong_alive);
            CHECK(wrong_dead == 0, "搬迁中有 %ld 个已删 key 仍能查到", wrong_dead);
            CHECK(dictSize(rd) == M - M / 2,
                  "size 应为 %d，实际 %d（两张表合计计数有误）", M - M / 2, dictSize(rd));

            free(ref);
            dictRelease(rd);   /* 可能仍在 rehash 中 → 必须两张表都释放 */
        }
    }

    if (failures) {
        printf("=== test_dict 失败 %d 项 ===\n", failures);
        return 1;
    }
    printf("=== test_dict 全部通过 ===\n");
    return 0;
}
