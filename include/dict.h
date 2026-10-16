#ifndef DICT_H
#define DICT_H

#include "sds.h"    // sds 类型（本层不再硬编码 sds，但 sds 版策略函数仍在此声明）

/* ============================================================
 * 通用哈希表 (dict)  —— mini-redis
 * ============================================================
 * 链地址法。key/value 都是 void*，具体是什么类型、怎么算哈希、怎么比较、
 * 怎么销毁，全部由建表时传入的 dictType（函数指针表）决定。
 *
 * 于是同一份核心代码能同时服务两个语义完全不同的调用方：
 *
 *     storage  ──► dictCreate(&sdsDictType)        destructor = sdsfree  → 拥有
 *     skiplist ──► dictCreate(&skipIndexDictType)  destructor = 空操作   → 借用
 *
 * 判断"这是谁的表"不需要任何 if / enum —— 建表时把函数表存进 d->type，
 * 之后每次删除自动走对应分支。
 *
 * ★ 设计推导、Redis 对照笔记、渐进式 rehash 实现记录、未做的取舍（union /
 *   哈希种子）见 docs/字典设计.md —— 头文件只留接口与硬约定。
 *
 * ------------------------------------------------------------
 * 三条会崩的硬约定（用之前必须知道）
 * ------------------------------------------------------------
 * (1) NULL 表示"不拷贝"与"不销毁"是两件事：
 *       keyDup == NULL        → 存调用方给的指针，不复制
 *       keyDestructor == NULL → 删除/销毁时什么都不做（借用）
 *
 * (2) keyCompare 的语义必须是「相等返回 0」（同 strcmp）。
 *     它用于判断"桶里是否已有同一个 key"，不是 memcmp ——
 *     key 的相等语义完全由调用方定义。
 *
 * (3) hashFunction 必须与 keyCompare 口径一致：
 *     比较判定相等的两个 key，必须得到同一个哈希值，否则会落到不同桶查不到。
 * ============================================================ */

typedef struct dictEntry dictEntry;
typedef struct dict dict;

/* ---------- 回调签名 ---------- */
typedef unsigned long (*dictHashFunc)(const void *key);
typedef int   (*dictCompareFunc)(void *privdata, const void *key1, const void *key2);
typedef void *(*dictDupFunc)(void *privdata, const void *obj);
typedef void  (*dictFreeFunc)(void *privdata, void *obj);

/* 可选钩子：整数 key 直接把值本身当哈希（本项目未用，为将来非 sds key 留位） */
typedef uint64_t (*dictKeyFromU64Func)(const void *key);

/* ---------- dictType：字典的"操作手册"（C 版虚函数表） ----------
 * 只读、无状态，可被无数个 dict 共享（sdsDictType 只占一份内存）。
 * hashFunction / keyCompare 必填；其余可传 NULL。 */
typedef struct dictType {
    dictHashFunc        hashFunction;    /* 必填：算哈希 */
    dictCompareFunc     keyCompare;      /* 必填：相等返回 0 */
    dictKeyFromU64Func  keyFromU64;      /* 可选：整数 key 取值回调 */
    dictDupFunc         keyDup;          /* 可 NULL：NULL = 不拷贝 */
    dictDupFunc         valDup;          /* 可 NULL：NULL = 不拷贝 */
    dictFreeFunc        keyDestructor;   /* 可 NULL：NULL = 借用，不释放 */
    dictFreeFunc        valDestructor;   /* 可 NULL：NULL = 借用，不释放 */
} dictType;

/* ---------- 节点 ----------
 * ⚠ 本结构体不持有任何内存的所有权：它持有的两个指针归谁，
 *   完全由 d->type 的 destructor 决定。这就是 storage（拥有）与
 *   skiplist（借用）能共用同一个结构体的原因。 */
struct dictEntry {
    void *key;                /* 任意类型，由 dictType 解释 */
    void *val;                /* 任意类型，由 dictType 解释
                               * （将来可改 union 实现小值内联，见设计文档 §五） */
    unsigned long hash;       /* 缓存哈希值：rehash / 查找不重算 */
    struct dictEntry *next;
};

/* ---------- 表 ----------
 * ★ 渐进式 rehash：扩容不是"停下搬完"，而是【两张表并存 + 每次操作搬一点】。
 *   搬迁期间的状态：
 *     - buckets       主表（rehash 期间 = 新表，只增不减）
 *     - buckets_old   旧表（只减不增）；非 NULL 表示正在搬迁
 *     - rehashidx     旧表中"下一个要搬的桶"；-1 = 未在搬迁
 *   查/删要同时看两张表（数据可能还在旧表）；新增只进新表。
 *   这样把一次 O(n) 的长停顿，摊成 n 次 O(1) 的小步。 */
struct dict {
    dictType *type;          /* 操作手册：建表时定下，之后不变 */
    void *privdata;          /* 回调透传的上下文（本项目为 NULL） */
    dictEntry **buckets;     /* 主表（rehash 期间 = 新表，只增不减） */
    dictEntry **buckets_old; /* rehash 期间的旧表；非 NULL = 正在搬迁 */
    long rehashidx;          /* 旧表搬到第几个桶了；-1 = 未在搬迁 */
    int old_used;            /* ★ 旧表里【还剩多少个键值对】——完成判定用它，
                              *   不用 rehashidx：路径 A 会插队搬走后面的桶，
                              *   只看下标会误判"扫到末尾 = 搬完了"。 */
    int size;                /* 总键值对个数（两张表合计） */
    int capacity;            /* 主表桶数 */
    int old_capacity;        /* 旧表桶数（仅 rehash 期间有效） */
};

/* ============================================================
 * 生命周期
 * ============================================================ */

/* 创建表。type 不得为 NULL（hashFunction/keyCompare 必填），privdata 可 NULL。
 * ⚠ 不拷贝 type，调用方需保证其生命周期覆盖整个 dict（实践上都是 static 变量）。 */
dict *dictCreate(dictType *type, void *privdata);

/* 销毁整张表：按 d->type 的 destructor 销毁每个 key/value，释放节点与桶数组。
 * ⚠ 若表中的 key/value 由别处拥有（如跳表节点），对应的 destructor 必须是
 *   空操作 / NULL —— 否则会 double-free。 */
void dictRelease(dict *d);

/* ============================================================
 * 增删查
 * ============================================================ */

/* 插入；key 已存在则【更新】其 value 并返回 1（不新增节点）。
 *
 * ★ 所有权：不拷贝、也不释放 key/value —— 谁负责最终释放由 d->type 的
 *   destructor 决定。失败时同样不释放（通用表不知道它们归谁），
 *   归还责任在调用方。
 *
 * ★ 两个通用化的行为变化（对 storage 的旧调用方是需要注意的）：
 *   - key 已存在时，传入的 key 不会被释放（旧 hashtable_set 会替调用方
 *     sdsfree(key)）—— 表里那条 key 才是 owner，调用方传的这份要自己处理。
 *   - key 已存在时，旧 value 也不会被释放（通用表不知道旧 value 归谁）。
 *     需要释放的调用方（storage）自己先 dictFind 拿旧值再释放。
 *
 * ⚠ 接受 NULL value（只用 key 的字典）；key 为 NULL 直接返回 0。 */
int dictAdd(dict *d, void *key, void *val);

/* 查找，返回内部 value 指针（不拷贝、不转移所有权）；找不到返回 NULL。
 * ⚠ 返回的是表内【活引用】：调用方不得 free（除非它就是 owner），
 *   且有效性依赖"调用期间表不被改"。storage_get 会 sdsdup 成快照再外传。
 * ⚠ 无法区分"key 不存在"与"key 存在但 value 为 NULL" —— 本项目两个调用方
 *   的 value 都非空（sds / SkipNode*），故不受影响。 */
void *dictFind(dict *d, const void *key);

/* 删除 key：按 d->type 的 destructor 销毁其 key/value，摘链，释放节点。
 * 返回 1 删到；0 不存在。
 * ⚠ 这里就是"通用"最关键的一处：storage 的表在这里真的 sdsfree，
 *   跳表索引的表在这里什么都不做。 */
int dictDelete(dict *d, const void *key);

/* 当前键值对个数。 */
int dictSize(dict *d);

/* ============================================================
 * sds 版策略（样板，同时供跳表索引复用）
 * ============================================================
 * 跳表的 member 也是 sds，所以 skipIndexDictType 的 hash/compare 直接指向
 * 这两个函数，只把 destructor 换成空操作。
 * ★ 导出而非 static：否则跳表只能再抄一份 djb2 —— 那正是要消灭的重复。 */
unsigned long dictSdsHash(const void *key);
int dictSdsKeyCompare(void *privdata, const void *key1, const void *key2);

/* sdsfree 的策略函数包装。
 * 为什么要包一层：sdsfree 类型是 void(char*)，而 dictFreeFunc 是
 * void(*)(void*, void*)，直接赋值类型不兼容（告警 + UB）。 */
void dictSdsDestructor(void *privdata, void *obj);

/* storage 用：拥有 key/value 的 sds 字典策略（destructor = sdsfree） */
extern dictType sdsDictType;

#endif
