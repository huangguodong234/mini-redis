#ifndef SDS_H
#define SDS_H

#include <stddef.h>   // size_t
#include <stdint.h>   // uint8_t/uint16_t/uint32_t/uint64_t

/* ============================================================
 * SDS —— Simple Dynamic String（简单动态字符串）
 * ------------------------------------------------------------
 * Redis 最基础的字符串类型，底层存储、协议解析、缓冲管理全靠它。
 * 理解了"它为什么存在、内存怎么摆、为什么这么摆"，这道题就通了。
 * ============================================================ */

/* ------------------------------------------------------------
 * 一、为什么需要 SDS？（C 字符串的 5 个缺陷）
 * ------------------------------------------------------------
 * 裸 C 字符串 = char* + 结尾 '\0'，有 5 个原生局限：

 *   (1) 取长度是 O(n)：strlen() 得从头扫到 '\0'。频繁取长度
 *       的场景（如算 RESP 长度行 $len）纯属浪费。

 *   (2) 不是二进制安全：数据里一旦出现 '\0'，strlen 就当字符串
 *       结束了，后面的内容被"切断"。而协议/数据的值完全可能
 *       含 '\0'（二进制图片、序列化数据……）。

 *   (3) 容易缓冲区溢出：strcpy/strcat 不检查目标容量，目标太
 *       小就写越界，是经典"栈/堆溢出"漏洞来源。

 *   (4) 改字符串 = 频繁内存重分配：每次拼接都重新 malloc 一块
 *       恰好够大的内存再拷贝。N 次 O(len) 拼接 = O(N*len)，
 *       还伴随碎片与系统调用开销。

 *   (5) 信息分散、不自描述：长度、容量、数据散落各处，字符串
 *       自己"不知道自己多大"，没法判断要不要扩容。

 * SDS 用"header(元数据) + 数据区"一次解决上面 5 个问题。
 * ------------------------------------------------------------ */

/* ------------------------------------------------------------
 * 二、核心设计：header + 数据区（内存布局）
 * ------------------------------------------------------------
 * 一整块 malloc 出来的内存，分成两段：

 *        char *s 指向这里（数据区起点，也是 sds 返回值）
 *              ↓
 *   ┌──────────────┬──────────────────────┬───┐
 *   │   sdshdr      │     字符数据区         │\0 │
 *   │ {len,alloc,   │   (真正的字符串内容)   │   │
 *   │   flags}      │                       │(结尾)│
 *   └──────────────┴──────────────────────┴───┘
 *     ↑ header         ↑ 从这里开始当普通 C 字符串用

 * 关键魔法：sds 其实就是一个 char*，但它指向的"数据区"前面
 * 紧挨着一个 sdshdr。所以：
 *   - 头部地址 = sds 指针 - 头部大小；
 *   - sdslen() 反向回溯到 header 读 len，O(1)；
 *   - s 指向的就是 '\0' 结尾的字符数据，strlen(s)、printf("%s",s)
 *     都能直接工作——对外"看起来就是个普通字符串"。
 *
 * 效果：API 只暴露 char*（s），调用者不用传一堆 sizeof/len 参数，
 * 自由度比裸 char* 还高；内部却 O(1) 拿长度、判容量，因为信息
 * 都在它身后。
 * ------------------------------------------------------------ */

/* ------------------------------------------------------------
 * 三、SDS 如何逐一解决那 5 个问题
 * ------------------------------------------------------------
 *   (1) 长度 O(1)   → len 存在 header，sdslen() 不扫描
 *   (2) 二进制安全  → 长度靠 len 记录，而非靠 '\0' 判断；
 *                      数据里可以有任意个 '\0'
 *   (3) 杜绝溢出    → 每次写前用 avail(alloc-len) 判断，空间不足
 *                      先扩容，绝不在满时直写
 *   (4) 减少重分配  → "空间预分配 + 惰性空间释放"，见第四节
 *   (5) 信息自描述  → len/alloc 都在 header，字符串"知道自己多长、
 *                      还有多少余量"，扩容决策 O(1)
 * ------------------------------------------------------------ */

/* ------------------------------------------------------------
 * 四、减少内存重分配的两个策略（性能关键）
 * ------------------------------------------------------------
 * ① 空间预分配（sdsMakeRoomFor）
 *    空间不足要扩容时，不是"刚好多 1 字节"，而是一次多给不少：
 *       - 新长度 < 1MB：翻倍（new_alloc = len*2 + 1）
 *       - 新长度 ≥ 1MB：只多 +1MB
 *    好处：连续多次小拼接，前几次都落在"已有多余空间"里，不必
 *    每次 realloc，均摊到每次 append 是 O(1)（平摊分析）。

 * ② 惰性空间释放（sdsclear）
 *    清空/截断时不急着把 alloc 缩回去，只把 len 归 0。下次再拼
 *    接直接复用这块空间，避免"用完就缩、缩完又扩"的反复 malloc。

 * 本实现已完整实现 1MB 阈值分档，与真实 Redis 策略一致，均摊
 * O(1) 追加。分规格头部（下节）也已实现，短字符串自动用小头部。
 * ------------------------------------------------------------ */

/* ------------------------------------------------------------
 * 五、分规格头部（TYPE_5 / 8 / 16 / 32 / 64）
 * ------------------------------------------------------------
 * 字符串长度差别很大（"OK" vs 几 GB 缓存）。若所有字符串都用
 * 一个固定大小头部，短字符串会白白浪费内存——比如只存 3 字节的
 * "abc"，若头部固定 17B，总占 18B，浪费 5 倍多。
 *
 * 所以 Redis 按"数据长度"分 5 档，每档用尽量小的整型存 len/alloc：

 *   类型          位宽(len/alloc)   头部总大小   适配数据长度
 *   SDS_TYPE_5        ——(见下)         1B         ≤ 31 字节
 *   SDS_TYPE_8        uint8 / uint8    3B         < 256 字节
 *   SDS_TYPE_16       uint16/uint16    5B         < 64KB
 *   SDS_TYPE_32       uint32/uint32    9B         < 4GB
 *   SDS_TYPE_64       uint64/uint64   17B         大字符串

 * 每个头部都以 1 字节 flags 开头：低 3 位 = 类型编号(SDS_TYPE_*)，
 * 高 5 位 = 附加标志（本实现未使用，保留）。
 * TYPE_5 是特例：没有独立 len/alloc 字段，len 就存在 flags 的
 * 高 5 位里（上限 31），所以头部只有 1 字节；也因此 TYPE_5 的
 * 串"分配后长度不可变"（alloc==len，无法再增长），要增长会先
 * 迁移到高一档 TYPE_8。
 *
 * 这样 "abc"(len=3) 只用 1B 头部 + 数据 + '\0'，比定长 17B 省很多。
 * ------------------------------------------------------------ */

/* ------------------------------------------------------------
 * 六、为什么结尾总留一个 '\0'？
 * ------------------------------------------------------------
 * 就算数据是二进制（本身可能就含 '\0'），也会在末尾多写一个
 * '\0'。目的：
 *   - sds 永远可零拷贝丢给任何"按 C 字符串处理"的库函数
 *     （printf、strlen、snprintf、文件读写……），无需转换；
 *   - 既能当"干净的动态数组"（按 len 操作），又能当"C 字符串"
 *     （按 '\0' 操作），两种用法都兼容。
 * 这就是 SDS "既能当字节数组、又能当字符串两用"的精髓。
 * ------------------------------------------------------------ */

/* ------------------------------------------------------------
 * 七、为什么头部必须 packed？（关键实现点）
 * ------------------------------------------------------------
 * 整个设计建立在这条换算上：flags 紧贴数据前 1 字节（s[-1]），
 * 数据 = malloc 基址 + 头部大小。若头部不 packed，结构体会因
 * 字段对齐在尾部插入填充，sizeof 就大于字段实际字节数——
 * "s - sizeof(hdr)" 算回去的地址不再等于 malloc 基址，free 会
 * 直接崩堆，flags 也不在 s[-1] 了。
 * （本实现里 sdshdr16 不 packed 时 sizeof 是 6 而非 5，正是这个坑。）
 * 所以 5 个 sdshdr 都用 __attribute__((__packed__)) 去掉填充。
 * ------------------------------------------------------------ */

/* ============================================================
 * 具体数据类型定义
 * ============================================================ */

/* 五档类型编号（按数据长度选最小的那档） */
#define SDS_TYPE_5  0   /* 极短：len 存于 flags 高 5 位，上限 31，alloc==len */
#define SDS_TYPE_8  1   /* uint8  len/alloc */
#define SDS_TYPE_16 2   /* uint16 len/alloc */
#define SDS_TYPE_32 3   /* uint32 len/alloc */
#define SDS_TYPE_64 4   /* uint64 len/alloc */

/* flags 里"类型"占低 3 位，用这个掩码取出；这 3 位所能表示的最大值 */
#define SDS_TYPE_MASK 7
#define SDS_TYPE_BITS 3

/* TYPE_5 里 len 存于高 5 位，这是高 5 位能表示的最大值 */
#define SDS_TYPE_5_LEN 31

/* 五种头部结构，每个都以 1 字节 flags 开头（对齐真实 Redis 布局）。
 * 必须 packed：去掉尾部对齐填充，保证 sizeof 恰好等于字段字节数，
 * 否则 "flags 在 data[-1]" 和 "data = base + sizeof(hdr)" 会错位。 */

/* TYPE_5：无独立 len/alloc 字段，len 存在 flags 高 5 位 */
struct __attribute__((__packed__)) sdshdr5 {
    unsigned char flags; /* 低3位=type，高5位=len */
};

/* TYPE_8 ~ TYPE_64：flags + len + alloc，位宽逐级增大 */
struct __attribute__((__packed__)) sdshdr8 {
    uint8_t len;    /* used */
    uint8_t alloc;  /* 不含结尾 '\0' */
    unsigned char flags; /* 低3位=type，高5位=标志 */
};
struct __attribute__((__packed__)) sdshdr16 {
    uint16_t len;
    uint16_t alloc;
    unsigned char flags;
};
struct __attribute__((__packed__)) sdshdr32 {
    uint32_t len;
    uint32_t alloc;
    unsigned char flags;
};
struct __attribute__((__packed__)) sdshdr64 {
    uint64_t len;
    uint64_t alloc;
    unsigned char flags;
};

/* sds 类型：就是一个 char*，指向"数据区"（各类头部都在它前面）。
 * 调用者拿到的永远是数据区起始地址，看起来就是一个普通字符串。 */
typedef char *sds;

/* 根据字符串长度选一个最省的头部类型（对齐 Redis sdsReqType）。
 * 注意用 "<"：长度等于 32/256/65536/4G 时正好落到高一档，
 * 不会卡在 TYPE_5 放不下的边界上。 */
static inline char sdsReqType(size_t string_size) {
    if (string_size < 1 << 5)                       return SDS_TYPE_5;
    if (string_size < 1 << 8)                       return SDS_TYPE_8;
    if (string_size < 1 << 16)                      return SDS_TYPE_16;
    if (string_size < 1ll << 32)                    return SDS_TYPE_32;
    return SDS_TYPE_64;
}

/* 某类型头部总大小（含 flags）。TYPE_5 只有 1 字节。 */
static inline size_t sdsHdrSize(char type) {
    switch (type & SDS_TYPE_MASK) {
        case SDS_TYPE_5:  return sizeof(struct sdshdr5);
        case SDS_TYPE_8:  return sizeof(struct sdshdr8);
        case SDS_TYPE_16: return sizeof(struct sdshdr16);
        case SDS_TYPE_32: return sizeof(struct sdshdr32);
        case SDS_TYPE_64: return sizeof(struct sdshdr64);
    }
    return 0; /* 不可达 */
}

/* 某类型里 len 字段的位宽（TYPE_5 特殊：无独立 len，返回 0） */
static inline size_t sdsTypeLenSize(char type) {
    switch (type & SDS_TYPE_MASK) {
        case SDS_TYPE_5:  return 0;   /* len 藏在 flags 里 */
        case SDS_TYPE_8:  return 1;
        case SDS_TYPE_16: return 2;
        case SDS_TYPE_32: return 4;
        case SDS_TYPE_64: return 8;
    }
    return 0;
}

/* 从 sds 数据指针反推类型。
 * flags 总是紧挨在数据前面 1 字节，s[-1] 低 3 位就是类型。 */
static inline char sds_type(const sds s) {
    return s[-1] & SDS_TYPE_MASK;
}

/* 取某类型头部的指针（按类型把数据指针往回挪一个头部大小，得到 struct） */
static inline struct sdshdr8  *sds_hdr8 (const sds s) { return (struct sdshdr8  *)(void *)(s - sizeof(struct sdshdr8)); }
static inline struct sdshdr16 *sds_hdr16(const sds s) { return (struct sdshdr16 *)(void *)(s - sizeof(struct sdshdr16)); }
static inline struct sdshdr32 *sds_hdr32(const sds s) { return (struct sdshdr32 *)(void *)(s - sizeof(struct sdshdr32)); }
static inline struct sdshdr64 *sds_hdr64(const sds s) { return (struct sdshdr64 *)(void *)(s - sizeof(struct sdshdr64)); }

/* ============================================================
 * 用法约定：sds 的 4 个坑
 * ============================================================
 * ① 几乎所有改变长度的函数都可能"搬家"(realloc)，旧 sds 指针
 *    可能失效，必须用返回值接住：
 *        s = sdscat(s, "x");    // 正确
 *        sdscat(s, "x");        // 错！旧 s 可能已被 free
 *
 * ② NULL 处理：分配失败返回 NULL，生产代码使用前要判空。
 *
 * ③ sdsfree 之后不能再碰那个指针（悬垂），也不能 free 两次。
 *
 * ④ 想改数据区请走 API（sdscatlen 等），不要手动越界写，
 *    否则绕过"先扩容再写"的保护，等于退回裸 C 字符串。
 * ------------------------------------------------------------ */

/* ============================================================
 * API 一览
 * ============================================================ */

/* ---------- 基础创建 ---------- */

/* 用"指定长度 + 内容"创建 sds。
 * initlen 精确指定字节数；init 可为 NULL（数据区填 0）。
 * 按长度拷贝，init 里含 '\0' 也不影响，二进制安全。 */
sds sdsnewlen(const void *init, size_t initlen);

/* 用 '\0' 结尾的 C 字符串创建 sds（底层即 sdsnewlen(init, strlen(init))）。 */
sds sdsnew(const char *init);

/* 创建空 sds：len=0，只有结尾 '\0'。 */
sds sdsempty(void);

/* 深拷贝一份 sds，返回新分配内存，原 s 不受影响。 */
sds sdsdup(const sds s);

/* ---------- 释放 ---------- */

/* 释放整块内存（header + 数据一起）。释放后 s 不可再用。 */
void sdsfree(sds s);

/* 清空内容但保留已分配容量（len 归 0，alloc 不变）——惰性释放，
 * 下次 append 直接复用这块空间。 */
void sdsclear(sds s);

/* ---------- 读取元信息 ---------- */

/* O(1) 返回当前长度（字节数）。 */
size_t sdslen(const sds s);

/* 返回剩余可用空间（alloc - len），append 前可用来判断够不够。 */
size_t sdsavail(const sds s);

/* ---------- 追加 / 扩容 ---------- */

/* 追加 addlen 字节的二进制数据到末尾。
 * 返回可能被 realloc 搬家后的新 sds（必须用返回值接住）。 */
sds sdscatlen(sds s, const void *t, size_t addlen);

/* 追加一个 '\0' 结尾的 C 字符串（等价 sdscatlen(s, t, strlen(t))）。 */
sds sdscat(sds s, const char *t);

/* 确保至少有 addlen 的额外可用空间，不够就扩容（空间预分配）。
 * 返回可能搬家后的新 sds；空间已够则原样返回 s。 */
sds sdsMakeRoomFor(sds s, size_t addlen);

/* ---------- 比较 ---------- */

/* 逐字节比较两个 sds：<0 表示 s1<s2，=0 相等，>0 表示 s1>s2。
 * 先 memcmp 相同前缀，再比长度，所以对二进制数据也正确。 */
int sdscmp(const sds s1, const sds s2);

/* 截取子串并就地覆盖 s：保留 [start,end]（含端点），start/end 可为负
 * （-1 表示最后一个字符）。只做 memmove + 更新 len，不重新分配。
 * sdsrange(s, pos, -1) 可用来把缓冲区从 pos 起的未处理数据移到头部。 */
sds sdsrange(sds s, long start, long end);

#endif /* SDS_H */
