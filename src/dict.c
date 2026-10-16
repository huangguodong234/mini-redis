// ============================================================
// dict.c —— mini-redis 通用哈希表的具体实现
// ============================================================
// 链地址法：桶是单向链表，插入用头插，查找/删除遍历桶内链。
//
// ★ 本文件是【通用】的：它不认识 sds，也不认识 SkipNode。
//   所有"key/value 是什么类型、怎么算哈希、怎么比较、怎么销毁"的知识，
//   都通过 d->type（dictType 函数指针表）在运行时取得。
//
// ★ 设计推导 / Redis 对照笔记 / 本轮未做的取舍见 docs/字典设计.md。
//
// ⚠ 全文件只有末尾的 sdsDictType 一块依赖 sds —— 那是给用 sds 的调用方
//   （storage 与跳表索引）共用的样板。除此之外核心是类型无关的：
//   将来要加整数 key 的字典，再写一张 dictType 即可，前半部分一行不用动。
// ============================================================

#include <stdio.h>
#include <stdlib.h>
#include "dict.h"

#define DICT_INITIAL_CAPACITY 16
#define LOAD_FACTOR_THRESHOLD 0.75

// ==================== djb2 哈希函数 ====================
// 按 sds 的 len 逐字节哈希，二进制安全：
// 关键：走 sdslen 而非 '\0'，key 里即使含 '\0' 也不会提前终止
// 算法：hash = 5381;  hash = hash * 33 + 下一个字符
//      因为 33 = 32 + 1 = (2^5 + 1)，所以 hash*33 = (hash<<5) + hash
//
// ★ 注意签名：收 const void*，内部转回 sds 再走 sdslen。
//   这一层转换就是"void* key 允许任意类型 key"的代价与价值所在：
//   转换只发生在策略函数内部（一处），而不是散落在核心逻辑里。
//   ⚠ sds 必须非空：sdslen 会读 s[-1] 反推 header。调用方（dictAdd/
//     dictFind/dictDelete）已统一判空，故此处不再重复判。
static unsigned long hash_djb2_sds(const void *key)
{
    const sds str = (const sds)key;
    unsigned long hash = 5381;
    size_t len = sdslen(str);
    for (size_t i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + (unsigned char)str[i];
    }
    return hash;
}

// ========== 创建哈希表 ==========
// ★ 建表时把 type 记进 d->type —— 这一刻决定了"这是谁的表"。之后所有
//   del/release 都经 d->type->xxxDestructor 走各自分支，核心代码里
//   【没有任何一处】if (是跳表) / if (是storage)。
dict *dictCreate(dictType *type, void *privdata)
{
    // 操作手册是必须的：hashFunction/keyCompare 缺一不可，
    // 否则插入时会空指针跳转。这里前置拦截，避免把崩溃推迟到运行时深处。
    if (!type || !type->hashFunction || !type->keyCompare) {
        fprintf(stderr, "dictCreate: dictType 不完整（hashFunction/keyCompare 必填）\n");
        return NULL;
    }

    dict *d = malloc(sizeof(dict));
    if (!d) {
        fprintf(stderr, "dictCreate: malloc dict 失败\n");
        return NULL;
    }
    d->type = type;
    d->privdata = privdata;
    d->capacity = DICT_INITIAL_CAPACITY;
    d->size = 0;
    // ★ 渐进式 rehash 的初始状态：没有旧表、不在搬迁中
    d->buckets_old = NULL;
    d->old_capacity = 0;
    d->rehashidx = -1;
    d->old_used = 0;

    // calloc 分配并清零，所有桶初始为 NULL
    d->buckets = calloc((size_t)d->capacity, sizeof(dictEntry *));
    if (!d->buckets) {
        fprintf(stderr, "dictCreate: calloc buckets 失败\n");
        free(d);
        return NULL;
    }
    return d;
}

// ==================== 单个节点的销毁 ====================
// "所有权外包"落地的唯一一处：key/value 怎么死完全由 d->type 决定。
//   storage 的表   → destructor = sdsfree  → 这里真的释放
//   skiplist 索引  → destructor = 空操作   → 这里什么都不做
//
// ★ 判空不是防御性冗余：destructor 允许为 NULL 表示"借用，不释放"，
//   直接调用会空指针跳转崩溃，所以 NULL 检查是接口契约的一部分。
static void dictFreeEntry(dict *d, dictEntry *he)
{
    if (!he) return;
    if (d->type->keyDestructor)
        d->type->keyDestructor(d->privdata, he->key);
    if (d->type->valDestructor)
        d->type->valDestructor(d->privdata, he->val);
    free(he);
}

// ==================== 查节点：用 rehashidx 决定"查哪张表" ====================
// 这是渐进式 rehash 的核心读取原语。三条路径（add 查重 / find / delete 定位）
// 全都走它，保证查找逻辑只有一处实现、不会被漏掉。
//
// 返回：命中的节点指针；找不到返回 NULL。
// ⚠ 返回的是表内【活引用】，调用方不得 free。
//
// ┌─ 用 rehashidx 省掉一次查找（核心优化）────────────────────────────┐
// │ 令 idx = hash % old_capacity，即该 key 在【旧表】的桶号。           │
// │                                                                  │
// │   idx <  rehashidx → 这个旧桶【已经被顺序扫描搬走】了。            │
// │                      所以数据一定不在旧表 → 【跳过旧表】。          │
// │                      实测：rehash 中 87.5% 的查找落在这一侧，       │
// │                      全部省掉了旧表访问。                          │
// │                                                                  │
// │   idx >= rehashidx → 这个旧桶还没被顺序扫描搬 → 数据【可能】在旧表。│
// │                      先查旧表（命中率实测 84.4%），再查新表。       │
// └──────────────────────────────────────────────────────────────────┘
//
// ★ 这个优化依赖一条【不变量】：旧表中 [0, rehashidx) 区间恒为空。
//   它为什么成立：只有 dictRehashOneBucket 会推进 rehashidx，而它
//   推进前必然已经把那个桶搬空了（dictBucketRehash 末尾置 NULL）。
//   路径 A 只搬 idx >= rehashidx 的桶，
//   永远不动 rehashidx，所以不会在 [0, rehashidx) 里留下"已搬但下标没推"的桶。
//   → 关于这条不变量的完整论证与实测，见 docs/字典设计.md §六。
//
// ★ 为什么 idx >= rehashidx 时【先查旧表】而不是先查新表：
//   没有被顺序扫描搬走的桶，数据大概率还在旧表（实测命中率 84.4%）。
//   先查它可以用一次访问命中，省掉先翻新表再翻旧表的第二次访问。
//   注意：这是在本实现（路径 A 只在 if/else 的 else 分支之外单独生效、
//   且只搬 >= rehashidx 的桶）语义下成立的结论。
//
// ★ 标 static inline 是有意的：dictFind 是热路径（跳表每次 member 查都走它）。
//   实测：不加 inline 时跳表 find 从 261ns 退化到 ~360ns（+38%）。
//
// ┌─ ⚠ 实测：这个优化的收益【远小于预期】，如实记录 ──────────────────┐
// │ 结构上它确实省掉了大多数旧表访问（旧版查旧表时 81.3% 落在空槽，     │
// │ 那些访问都可以省掉）。但受控 A/B（交叉 7 轮取中位数）显示：         │
// │     新版 44.0 ns/op  vs  旧版 45.6 ns/op   → 仅 -3.5%，在噪声内。  │
// │                                                                  │
// │ 为什么省了 81% 的旧表访问却没换来速度：                            │
// │   被省掉的那些访问本身几乎不花钱 —— 落在空槽 = 一次取模 + 一次      │
// │   访存 + 一次判空，没有链表遍历。旧表桶数组 2 MB，热的部分 cache     │
// │   命中率不低。省掉"便宜的事"≈ 省不到东西。                         │
// │   真正贵的是命中非空槽后的链表遍历，而那部分省不掉。                │
// │                                                                  │
// │ ★ 保留它的理由是【结构清晰 + 与上游 Redis 语义对齐】，不是性能。    │
// │   它没有负作用（对拍 0 错误、ASan 干净），但不要宣称"提速"。        │
// └──────────────────────────────────────────────────────────────────┘
static inline dictEntry *dictFindEntry(dict *d, const void *key, unsigned long hash)
{
    // ---- 没在 rehash：只有一张表，直接查 ----
    if (d->buckets_old == NULL) {
        dictEntry *he = d->buckets[hash % (unsigned long)d->capacity];
        while (he) {
            if (d->type->keyCompare(d->privdata, he->key, key) == 0) return he;
            he = he->next;
        }
        return NULL;
    }

    // ---- rehash 中：用 idx 决定查哪张表 ----
    long idx = (long)(hash % (unsigned long)d->old_capacity);

    if (idx >= d->rehashidx) {
        // 该旧桶还没被顺序扫描搬走 → 数据大概率在旧表，先查它
        dictEntry *he = d->buckets_old[idx];
        while (he) {
            if (d->type->keyCompare(d->privdata, he->key, key) == 0) return he;
            he = he->next;
        }
    }
    // else: idx < rehashidx → 旧表该桶保证已空，跳过，直接查新表

    // 新表兜底。两种情况都会走到这里：
    //   ① idx >= rehashidx 但旧表没命中（该桶被路径 A 提前搬走了，或被删过）
    //   ② idx <  rehashidx（上面跳过旧表）
    // ⚠ ② 这种情况很关键：rehash 期间【新插入】的 key 只进新表，
    //   而它的 idx 完全可能 < rehashidx，所以绝不能"只查旧表"就返回。
    {
        dictEntry *he = d->buckets[hash % (unsigned long)d->capacity];
        while (he) {
            if (d->type->keyCompare(d->privdata, he->key, key) == 0) return he;
            he = he->next;
        }
    }
    return NULL;
}

// ==================== 扩容：从"一次性搬迁"改成"渐进式 rehash" ====================
//
// 【改造前的做法】dictResize：calloc 新桶数组 → 遍历全部旧桶把每个节点搬过去
//   → free 旧数组。一次调用搬完全部元素，耗时 O(n)。
//   实测代价（bench/bench_rehash，100 万键）：单次停顿最长 27.85 ms，
//   且随 N 线性增长（50 万 13.96ms / 200 万 53.86ms）。
//   这期间服务端完全无法处理其他请求 —— 就是「长尾延迟」。
//
// 【现在的做法】dictExpand 只做 O(1) 的准备工作（建新表、把旧表挂到
//   buckets_old、rehashidx 置 0），真正的搬运交给 dictRehashStep：
//   每次增删查顺手搬【一个桶】，把一次 O(n) 的长停顿摊成 n 次 O(1) 的小步。
//
// ★ 维持正确性的两条不变量（这是渐进式 rehash 的全部难点）：
//   (1) 新增只进【新表】—— 否则旧表永远搬不完。
//   (2) 查/删必须【两张表都看】—— 因为数据可能还在旧表。
//   只要守住这两条，搬迁期间任何时刻的语义都与"搬完了"一致。

// 启动一次扩容：只做 O(1) 的准备工作，不搬运任何元素。
// 返回：1 成功；0 失败（OOM）。
// 失败时表保持原样、仍然可用 —— dictAdd 会带着更高的负载因子继续插入
// （正确性不变，只是链变长），所以调用方可以忽略返回值，但要知道它发生过。
//
// 注：这里【不打日志】。库层代码直接写 stdout 在压测时每次扩容都是一次
// 同步 IO，且会淹没 test.sh 对服务器日志的分析（P1-4 修复，沿用）。
static int dictExpand(dict *d)
{
    // 已经在搬了就别再开一轮（正常路径不会走到，防御性）
    if (d->rehashidx != -1) return 1;

    int new_capacity = d->capacity * 2;
    dictEntry **new_buckets = calloc((size_t)new_capacity, sizeof(dictEntry *));
    if (!new_buckets) {
        fprintf(stderr, "dictExpand: 内存分配失败，扩容终止（将维持高负载因子运行）\n");
        return 0;
    }

    // ★ 关键：旧表【不释放、不搬空】，挂到 buckets_old 上慢慢搬。
    //   交接的一刻：新表成为主表（新增走它），旧表只减不增。
    d->buckets_old  = d->buckets;
    d->old_capacity = d->capacity;
    d->buckets      = new_buckets;
    d->capacity     = new_capacity;
    d->rehashidx    = 0;          // 从第 0 个桶开始搬
    d->old_used     = d->size;    // ★ 旧表此刻装着全部元素，逐个搬走后递减
    return 1;
}

//搬桶函数
// 把旧表第 idx 个桶的整条链搬到新表（头插），并清空该旧桶。
//
// 这是搬迁的原子单位 —— 对应注释里流程图的最内层
// "rehashEntriesInBucketAtIndex"。
//
// ★ 直接用缓存的 hash 重算下标，不重新调用 hashFunction：
//   这保证"搬到的桶"与"当初插入的桶"口径绝对一致
//   （即使 hashFunction 将来带了随机种子也不会错位）。
static void dictBucketRehash(dict *d, long idx)
{
    dictEntry *he = d->buckets_old[idx];
    while (he) {
        dictEntry *next = he->next;
        size_t new_idx = (size_t)(he->hash % (unsigned long)d->capacity);
        he->next = d->buckets[new_idx];
        d->buckets[new_idx] = he;
        he = next;
        d->old_used--;          // ★ 每搬走一个，旧表剩余数 -1
    }
    d->buckets_old[idx] = NULL;
}

//缓存不友好的搬桶函数
// 搬迁【一个】桶：从 rehashidx 开始找下一个非空桶并搬它（顺序扫描）。
// 返回：1 = 本次推进了（搬了桶或跳过了空桶）；0 = 已扫到旧表末尾。
//
// ⚠ 完成判定必须用【旧表是否还有数据】而不是只看 rehashidx ——
//   因为路径 A 会"插队"搬走 rehashidx 之后的桶，若只看下标，
//   可能出现"下标扫到末尾但旧表仍有数据"的错觉（本实现踩过这个坑）。
//   这里用 rehashidx >= old_capacity 表示"顺序扫描走完了"，
//   配合 dictRehashStepIfNeeded 里的 old_used 计数来决定何时真正释放。
static int dictRehashOneBucket(dict *d)
{
    // 单次调用最多跳过 10 个空桶（避免一次操作被大片空桶拖住）
    int empty_visits = 10;
    while (d->rehashidx < d->old_capacity &&
           d->buckets_old[d->rehashidx] == NULL) {
        d->rehashidx++;
        if (--empty_visits == 0) return 1;
    }
    if (d->rehashidx >= d->old_capacity) return 0;   // 顺序扫描走完

    dictBucketRehash(d, d->rehashidx);
    d->rehashidx++;
    return 1;
}

// 收尾：旧表已空 → 释放它，回到"单表"状态。
// ★ 为什么要单独一个函数：路径 A 和路径 B 都可能把旧表搬空（路径 A 插队搬、
//   路径 B 顺序搬），两处都要检查，所以收口在这里。
// ★ 判定用 old_used == 0（而不是 rehashidx >= old_capacity）：
//   路径 A 会搬走 rehashidx 之后的桶，此时下标可能还没扫到末尾，
//   但旧表其实已经空了 —— 用下标判定会白白多查很多次旧表。
static inline void dictCheckRehashingCompleted(dict *d)
{
    if (d->buckets_old && d->old_used == 0) {
        free(d->buckets_old);
        d->buckets_old = NULL;
        d->old_capacity = 0;
        d->rehashidx = -1;
    }
}

// ★★ 渐进式 rehash 的入口：路径 A（缓存友好）与路径 B（顺序扫描）二选一。
//
// visited_hash：本次操作访问的 key 的哈希值（调用方刚算过，避免重复计算）。
//
// ★ 为什么是【二选一】而不是"两个都做"（已核对 Redis unstable 源码：
//   _dictRehashStepIfNeeded 就是 if/else，互斥）：
//   两种写法功能都正确，但语义不同 ——
//     · 二选一（本实现）：一次操作【最多搬一个桶】。单次操作的延迟上限
//       被钉死在一个桶的搬迁成本上，尖刺最小。
//     · 两个都做：一次操作可能搬两个桶，rehash 收敛更快（旧表残余更少，
//       于是更早退出"两张表"状态、后续查找更便宜），但单次延迟上限翻倍。
//   这是"单次延迟 vs 收敛速度"的取舍。本项目按 Redis 的选择取前者：
//   服务端更在意【单次操作的最坏延迟】。度量见 docs/字典设计.md §六。
//
// ⚠ 曾误以为"二选一 → 路径 A 不动 rehashidx → rehash 永不收敛"。
//   错在漏看 else 分支：路径 B 在【路径 A 条件不成立时】执行，
//   不是"走了 A 就永不走 B"。只要操作序列里出现任何一次"旧桶已扫过
//   或是空桶"的操作，rehashidx 就会前进。实测（30 万 key）：
//   走路径 A 65,301 次、走路径 B 163,846 次，rehashidx 推进到 121,765；
//   之后转纯读负载，54,200 次查找后旧表完全搬空。收敛成立。
//
// ┌─ 两个必须讲清楚的坑（本实现都踩过，第二个还中了堆越界）────────────┐
// │ 坑 1：新旧两张表的【桶数不同】（新表 = 旧表 × 2），所以【同一个 key   │
// │   在两表里的下标不同】。绝不能拿"新表下标"去索引旧表：              │
// │     - 新表下标可能 >= old_capacity → 越界读写 buckets_old；         │
// │     - 即使没越界，那个位置也与本次访问的 key 无关。                  │
// │   正确做法：路径 A 用【key 的 hash 对 old_capacity 取模】，         │
// │   得到该 key 在旧表里的真实桶号。                                   │
// │                                                                  │
// │ 坑 2：完成判定不能只看 rehashidx。路径 A 会"插队"搬走 rehashidx     │
// │   之后的桶，于是可能出现"下标还没扫到末尾但旧表已空"（白查）或       │
// │   反过来"下标扫到末尾但旧表还有数据"（丢数据）。                    │
// │   正确做法：单独维护 old_used 计数，用它判定（Redis 的 ht_used[0]）。│
// └──────────────────────────────────────────────────────────────────┘
//
// ★ 路径 A 只搬 idx >= rehashidx 的桶，【不推进 rehashidx】——
//   这正是 dictFindEntry 能用 rehashidx 判断"该查哪张表"的前提：
//   [0, rehashidx) 区间始终为空这条不变量没有被路径 A 破坏。
static inline void dictRehashStepIfNeeded(dict *d, unsigned long visited_hash)
{
    if (d->rehashidx == -1) return;        // 没在搬迁 → 直接返回

    // 本次访问的 key 在旧表里的桶号
    long vi = (long)(visited_hash % (unsigned long)d->old_capacity);

    if (vi >= d->rehashidx && d->buckets_old[vi] != NULL) {
        // ---- 路径 A：搬这个桶（缓存友好）----
        // 它刚被本次操作访问过，桶头与链上 key 大概率还在 CPU cache 里，
        // 此刻搬它几乎不额外付内存访问成本。
        // ⚠ 不动 rehashidx —— 它是按 visited_hash 算出来的随机位置，
        //   不是顺序扫描的下一个位置。
        dictBucketRehash(d, vi);
    } else {
        // ---- 路径 B：顺序扫描（缓存不友好）----
        // 走到这里说明该桶已被扫过（vi < rehashidx）或本来就是空的。
        // 这是唯一推进 rehashidx、使 rehash 得以结束的路径。
        (void)dictRehashOneBucket(d);
    }
    dictCheckRehashingCompleted(d);
}

// ========== 插入/更新 ==========
// 参数：d 是表指针，key/val 是任意类型的指针（本函数不拷贝，按原样存）
// 行为：如果 key 已存在，则更新它的 val；不存在则在桶链表头部插入新节点
//
// ★ 所有权：本函数既不拷贝也不释放 key/value。
//   "谁负责最终释放"由 d->type 的 destructor 决定（可能在 dictDelete/
//   dictRelease 时释放，也可能永远不释放=借用）。
//   失败路径同理【不释放】——因为通用表不知道它们归谁，归还必须由
//   调用方做（storage_set_steal / skiplist_add 的注释里写明了）。
int dictAdd(dict *d, void *key, void *val)
{
    // 表或 key 异常时返回失败，不做任何写入（val 允许为 NULL，只用 key 的字典）。
    // ⚠ 与旧 hashtable_set 的一处有意差别：旧版在这里替调用方 sdsfree 归还
    //   key/value；通用版【不能】—— 它不知道这两个指针是拥有的还是借用的
    //   （跳表索引传来的 member 属于 SkipNode，free 了就 double-free）。
    //   销毁权已外包，失败路径的归还责任也随之外包给调用方。
    if (!d || !key) return 0;

    // 检查是否需要扩容（在新增元素之前）-检查 负载因子=元素数量/容器容量
    double load_factor = (double)d->size / d->capacity;
    if (load_factor >= LOAD_FACTOR_THRESHOLD) {
        // 扩容失败不致命：旧表依然可用，继续以更高的负载因子插入（只是链变长），
        // 所以不因此判定本次 SET 失败——真正决定成败的是下面能否存下节点。
        // ★ 注意 dictExpand 只是【启动】搬迁（O(1)），不搬元素。
        (void)dictExpand(d);
    }

    // 计算桶下标（哈希只在插入时算一次，然后缓存进节点）
    unsigned long hash = d->type->hashFunction(key);
    int index = (int)(hash % (unsigned long)d->capacity);

    // ★ 渐进式 rehash：把本次 key 的 hash 告诉搬迁逻辑 → 走路径 A 优先搬
    //   "这个 key 在旧表里所属的桶"（缓存友好）。必须在算完 hash 之后调。
    dictRehashStepIfNeeded(d, hash);

    // 检查链表中是否已存在该 key
    // ★ 相等判定走 keyCompare（不是 memcmp）：key 的相等语义完全由调用方定义。
    //   对 sds 它是二进制安全的 sdscmp；将来对 int64 它就是一次整数比较。
    //
    // ★ 不变量 (2)：查重必须【两张表都看】—— 正在 rehash 时，老 key 可能
    //   还在旧表没搬过来。只看新表会误判成"不存在"，导致同一个 key 存两份。
    //   搬过的那部分旧桶（下标 < rehashidx）已置 NULL，遍历它们成本可忽略。
    dictEntry *curr = dictFindEntry(d, key, hash);
    while (curr) {
        if (d->type->keyCompare(d->privdata, curr->key, key) == 0) {
            // 已存在 → 更新 value。
            // ⚠ 这里【不】销毁旧 value，也【不】释放调用方多传的 key：
            //   通用表不知道它们归谁（storage 的旧 value 要 sdsfree，
            //   跳表索引的旧 value 只是个 SkipNode* 不该动）。
            //   需要释放的调用方自己先 dictFind 拿旧值再释放 —— 见 dict.h。
            curr->val = val;
            return 1;       // 更新成功
        }
        curr = curr->next;
    }

    // 不存在 → 插入。★ 不变量 (1)：永远插入【新表】(d->buckets)。
    //   如果插进旧表，旧表就永远搬不完（只减不增这条前提被破坏）。
    dictEntry *new_node = malloc(sizeof(dictEntry));
    if (!new_node) {
        fprintf(stderr, "dictAdd: malloc dictEntry 失败\n");
        // P0-4 修复（沿用）：改造前这里 sdsfree 之后直接 return（void），命令层
        // 无从知晓，仍旧回 +OK —— 数据没进去却告诉客户端成功了。现在明确返回失败。
        return 0;
    }
    new_node->key = key;                   // 直接持有传入的指针（不拷贝）
    new_node->val = val;
    new_node->hash = hash;                 // 缓存哈希值，rehash/查找不重算
    new_node->next = d->buckets[index];    // 新节点指向原头节点
    d->buckets[index] = new_node;          // 桶指向新节点
    d->size++;
    return 1;
}

// ========== 查找 ==========
// 参数：d 是表指针，key 是查询键（只读，不接管、不释放）
// 返回：如果找到，返回内部 value 指针（不拷贝、不转移所有权）
//       如果找不到，返回 NULL
void *dictFind(dict *d, const void *key)
{
    // 改造前 hashtable_get 没有判空（传 NULL 会在 sdslen 里读 s[-1] 段错误）。
    // 通用版补上：这也是"能被多个调用方安全使用"的前提——不能只在当前
    // 调用方恰好不传 NULL 的前提下正确。
    if (!d || !key) return NULL;

    unsigned long hash = d->type->hashFunction(key);

    // ★ 渐进式 rehash：每次查找顺手推进一步（Redis 同样在 lookup 时推进，
    //   不只依赖写路径）。传入 hash → 路径 A 搬该 key 在旧表的桶（缓存友好）。
    dictRehashStepIfNeeded(d, hash);

    dictEntry *he = dictFindEntry(d, key, hash);
    return he ? he->val : NULL;   // 返回内部指针，外部不要 free
}

// ========== 删除 ==========
// 参数：d 是表指针，key 是查询键（只读，不接管）
// 返回：1 表示删除成功，0 表示 key 不存在
//
// ★ 这里就是"通用"最关键的一处：摘链 + 调用 dictFreeEntry，
//   而 dictFreeEntry 会按 d->type 决定 key/value 怎么死。
//   storage 的表在这里真的 sdsfree；跳表索引的表在这里什么都不做
//   （键值都是借用），只 free(dictEntry) 本身——索引条目归 dict 所有。
int dictDelete(dict *d, const void *key)
{
    if (!d || !key) return 0;

    unsigned long hash = d->type->hashFunction(key);

    // ★ 渐进式 rehash：删除路径同样推进一步，传入 hash → 路径 A
    dictRehashStepIfNeeded(d, hash);

    // ★ 两张表都要找：待删的 key 可能还在旧表没搬过来。
    //   两张表各扫一遍，命中哪张就从哪张摘 —— 用 (表指针, 桶下标) 区分来源。
    dictEntry **table = d->buckets;
    int cap = d->capacity;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            // 第二轮：旧表（仅 rehash 期间存在）
            if (!d->buckets_old) break;
            table = d->buckets_old;
            cap = d->old_capacity;
        }
        int index = (int)(hash % (unsigned long)cap);
        dictEntry *curr = table[index];
        dictEntry *prev = NULL;
        while (curr) {
            if (d->type->keyCompare(d->privdata, curr->key, key) == 0) {
                // 从链表中摘除节点
                if (prev == NULL) {
                    table[index] = curr->next;    // 删除头节点
                } else {
                    prev->next = curr->next;      // 删除中间节点
                }
                dictFreeEntry(d, curr);           // ← 按 d->type 决定怎么销毁
                d->size--;
                // ★ 若删的是【旧表】里的节点，old_used 也要递减 ——
                //   否则完成判定永远等不到 0，旧表会一直挂着（多一次查表开销）。
                if (pass == 1) d->old_used--;
                dictCheckRehashingCompleted(d);
                return 1;
            }
            prev = curr;
            curr = curr->next;
        }
    }
    return 0;
}

// ========== 个数 ==========
int dictSize(dict *d)
{
    return d ? d->size : 0;
}

// ========== 销毁 ==========
// 参数：d 是表指针
// 行为：释放表占用的所有内存（按 d->type 销毁 key/value、节点、桶数组、表本身）
void dictRelease(dict *d)
{
    if (!d) return;

    // ★ 正在 rehash 时【两张表都要释放】：旧表里还有未搬走的节点，
    //   只释放主表会漏掉它们（内存泄漏，且 key/value 也不会被 destructor 收起）。
    //   这里不需要逐个精算"哪张表有多少个"，两张表各扫一遍全释放即可
    //   —— 已搬走的旧桶已被置 NULL，不会重复释放。
    for (int i = 0; i < d->capacity; i++) {
        dictEntry *curr = d->buckets[i];
        while (curr) {
            dictEntry *next = curr->next;
            dictFreeEntry(d, curr);   // ← 同样按 d->type 决定 key/value 怎么销毁
            curr = next;
        }
    }
    free(d->buckets);

    if (d->buckets_old) {
        for (int i = 0; i < d->old_capacity; i++) {
            dictEntry *curr = d->buckets_old[i];
            while (curr) {
                dictEntry *next = curr->next;
                dictFreeEntry(d, curr);
                curr = next;
            }
        }
        free(d->buckets_old);
    }
    free(d);
}

// ============================================================
// sds 版策略函数（dictType 的样板实现）
// ============================================================
// 下面这三个函数是"策略可替换"的具体样板，同时也是跳表索引复用的对象：
// 跳表的 member 也是 sds，所以 skipIndexDictType 的 hash/compare 直接指向
// 这两个函数，只把 destructor 换成空操作。

// 导出版哈希（跳表索引的 dictType 需要它）
unsigned long dictSdsHash(const void *key)
{
    return hash_djb2_sds(key);
}

// 导出版比较（同 strcmp 语义：相等返回 0）
int dictSdsKeyCompare(void *privdata, const void *key1, const void *key2)
{
    (void)privdata;   // 本策略不需要上下文
    // sdscmp 是二进制安全比较（按 sdslen 逐字节），与 hash_djb2_sds 口径一致：
    // 两处都走 sdslen + 全字节，所以"比较相等 ⇒ 哈希相同"这条硬约定成立。
    return sdscmp((const sds)key1, (const sds)key2);
}

// sds 销毁器：把 sdsfree 包装成 dictFreeFunc 的签名。
// ★ 为什么必须包一层：sdsfree 的类型是 void(sds)，即 void(char*)；
//   而 dictFreeFunc 是 void(*)(void*, void*)。两者指针类型不兼容，
//   直接把 sdsfree 赋给函数指针字段会告警且属未定义行为。包一层转换
//   是 C 里做函数指针表的标准手法（Redis 的 dictSdsDestructor 同理）。
void dictSdsDestructor(void *privdata, void *obj)
{
    (void)privdata;
    sdsfree((sds)obj);
}

// storage 用的策略表：拥有 key/value 的 sds，销毁即 sdsfree。
// ★ 这就是"跳表那份表"的对照组——两张表唯一的差别就在最后两行。
dictType sdsDictType = {
    dictSdsHash,          // hashFunction   : djb2（二进制安全）
    dictSdsKeyCompare,    // keyCompare     : sdscmp（相等返回 0）
    NULL,                 // keyFromU64     : 本项目 sds key 用不到（留作将来整数 key）
    NULL,                 // keyDup         : NULL = 不拷贝（SET 走"所有权转移" steal）
    NULL,                 // valDup         : NULL = 不拷贝
    dictSdsDestructor,    // keyDestructor  : ★ 拥有 → sdsfree
    dictSdsDestructor,    // valDestructor  : ★ 拥有 → sdsfree
};
