/*
 * SDS —— Simple Dynamic String 实现
 *
 * 内存布局：
 *   malloc 出来的整块： [ sdshdr ][ 字符数据 ][ '\0' ]
 *   返回的 sds 指针指向"字符数据"的起始处。
 *
 * 关键换算：
 *   - sds 地址 - sizeof(sdshdr) = sdshdr 地址（header 在数据前面 2 个 size_t）
 *   - alloc 表示"数据区"的容量（不含结尾 '\0'），但 malloc 实际多分配 1 字节
 *     给结尾 '\0'，保证 sds 永远可当 C 字符串用（strlen/snprintf 等）。
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sds.h"

// ==================== 内部工具 ====================

// 从 sds 数据指针反推出 header 指针
static inline sdshdr *sds_hdr(const sds s) {
    return (sdshdr *)(void *)(s - sizeof(sdshdr));
}

// ==================== 创建 ====================

// 核心创建：len=initlen，内容拷贝自 init（可为 NULL 表示填 0）
sds sdsnewlen(const void *init, size_t initlen) {
    // 分配：header + 数据 + 结尾 '\0'
    sdshdr *sh = malloc(sizeof(sdshdr) + initlen + 1);
    if (!sh) return NULL;               // 分配失败

    sh->len = initlen;
    sh->alloc = initlen;                // 容量 = 当前长度

    if (initlen && init) {
        memcpy(sh + 1, init, initlen);  // sh+1 即数据区
    }
    // 结尾补 '\0'（保证可当 C 串用）
    ((char *)(sh + 1))[initlen] = '\0';

    // sds 指向数据区
    return (sds)(sh + 1);
}

// 以 C 字符串创建
sds sdsnew(const char *init) {
    if (!init) return sdsempty();
    return sdsnewlen(init, strlen(init));
}

// 创建空串
sds sdsempty(void) {
    return sdsnewlen("", 0);
}

// 深拷贝
sds sdsdup(const sds s) {
    if (!s) return NULL;
    return sdsnewlen(s, sdslen(s));
}

// ==================== 释放 / 清空 ====================

void sdsfree(sds s) {
    if (!s) return;
    free(sds_hdr(s));    // header 和数据是一整块，从 header 释放
}

void sdsclear(sds s) {
    sdshdr *sh = sds_hdr(s);
    sh->len = 0;                        // 逻辑长度归零
    s[0] = '\0';                        // 但数据区保留，下次可复用
}

// ==================== 取信息 ====================

size_t sdslen(const sds s) {
    return sds_hdr(s)->len;
}

size_t sdsavail(const sds s) {
    sdshdr *sh = sds_hdr(s);
    return sh->alloc - sh->len;
}

// ==================== 扩容 ====================

// 确保至少有 addlen 额外可用空间；不足则 realloc（翻倍+1，均摊 O(1)）
sds sdsMakeRoomFor(sds s, size_t addlen) {
    if (!s) return NULL;

    sdshdr *sh = sds_hdr(s);
    size_t avail = sh->alloc - sh->len;
    if (avail >= addlen) return s;      // 空间足够，什么都不做

    // 需要扩容：新容量 = 当前 len + addlen，至少翻倍到 len*2+1
    size_t new_alloc = sh->len + addlen;
    if (new_alloc < sh->len * 2) new_alloc = sh->len * 2;
    new_alloc++;                        // 多留 1 字节给结尾 '\0'

    sdshdr *new_sh = realloc(sh, sizeof(sdshdr) + new_alloc);
    if (!new_sh) return NULL;           // 扩容失败，原 sds 仍有效（realloc 保持原块）

    new_sh->alloc = new_alloc - 1;      // alloc 不含结尾 '\0'
    // 数据区起始处补 '\0'（len 不变，不破坏已有数据）
    ((char *)(new_sh + 1))[new_sh->len] = '\0';

    return (sds)(new_sh + 1);
}

// ==================== 追加 ====================

sds sdscatlen(sds s, const void *t, size_t addlen) {
    if (!s) return NULL;

    sds new_s = sdsMakeRoomFor(s, addlen);
    if (!new_s) return NULL;

    sdshdr *sh = sds_hdr(new_s);
    memcpy(new_s + sh->len, t, addlen);
    sh->len += addlen;
    new_s[sh->len] = '\0';              // 结尾补 '\0'

    return new_s;
}

sds sdscat(sds s, const char *t) {
    if (!t) return s;
    return sdscatlen(s, t, strlen(t));
}

// ==================== 比较 ====================

int sdscmp(const sds s1, const sds s2) {
    size_t l1 = sdslen(s1);
    size_t l2 = sdslen(s2);
    size_t minlen = (l1 < l2) ? l1 : l2;
    int cmp = memcmp(s1, s2, minlen);
    if (cmp != 0) return cmp;
    // 前缀相同，比较长度
    return (l1 > l2) ? 1 : (l1 < l2) ? -1 : 0;
}
