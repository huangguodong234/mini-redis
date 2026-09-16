/*
 * SDS 简单动态字符串实现（分规格版）
 * 按长度在 5 种头部(5/8/16/32/64)里选一种，短字符串省内存。
 * 布局：malloc 整块 = [ hdr{...,flags} ][ 数据 ][ '\0' ]，sds 指向数据区。
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include "sds.h"

static inline size_t sdslen_type(const sds s, char type) {
    switch (type & SDS_TYPE_MASK) {
        case SDS_TYPE_5:  return (size_t)(s[-1] >> SDS_TYPE_BITS);
        case SDS_TYPE_8:  return sds_hdr8(s)->len;
        case SDS_TYPE_16: return sds_hdr16(s)->len;
        case SDS_TYPE_32: return sds_hdr32(s)->len;
        case SDS_TYPE_64: return sds_hdr64(s)->len;
    }
    return 0;
}

static inline size_t sdsalloc_type(const sds s, char type) {
    switch (type & SDS_TYPE_MASK) {
        case SDS_TYPE_5:  return sdslen_type(s, type);
        case SDS_TYPE_8:  return sds_hdr8(s)->alloc;
        case SDS_TYPE_16: return sds_hdr16(s)->alloc;
        case SDS_TYPE_32: return sds_hdr32(s)->alloc;
        case SDS_TYPE_64: return sds_hdr64(s)->alloc;
    }
    return 0;
}

sds sdsnewlen(const void *init, size_t initlen) {
    char type = sdsReqType(initlen);
    /* 防御：TYPE_5 的高 5 位最多表示 31（SDS_TYPE_5_LEN）。
     * 当前 sdsReqType 用 `len < 1<<5` 判断，触不到此分支，
     * 但保留可在边界判据变动时避免写到 5 位放不下的长度。 */
    if (type == SDS_TYPE_5 && initlen > SDS_TYPE_5_LEN) {
        type = SDS_TYPE_8;
    }
    size_t hdrlen = sdsHdrSize(type);
    sds s = malloc(hdrlen + initlen + 1);
    if (s == NULL) return NULL;
    if (type == SDS_TYPE_5) {
        /* 头部只有 1 字节（flags），s 是 malloc 基址，flags 在 s[0] */
        s[0] = (char)((initlen << SDS_TYPE_BITS) | SDS_TYPE_5);
    } else {
        switch (type & SDS_TYPE_MASK) {
            case SDS_TYPE_8:
                ((struct sdshdr8 *)s)->len = initlen;
                ((struct sdshdr8 *)s)->alloc = initlen;
                ((struct sdshdr8 *)s)->flags = SDS_TYPE_8;
                break;
            case SDS_TYPE_16:
                ((struct sdshdr16 *)s)->len = initlen;
                ((struct sdshdr16 *)s)->alloc = initlen;
                ((struct sdshdr16 *)s)->flags = SDS_TYPE_16;
                break;
            case SDS_TYPE_32:
                ((struct sdshdr32 *)s)->len = initlen;
                ((struct sdshdr32 *)s)->alloc = initlen;
                ((struct sdshdr32 *)s)->flags = SDS_TYPE_32;
                break;
            case SDS_TYPE_64:
                ((struct sdshdr64 *)s)->len = initlen;
                ((struct sdshdr64 *)s)->alloc = initlen;
                ((struct sdshdr64 *)s)->flags = SDS_TYPE_64;
                break;
        }
    }
    char *data = s + hdrlen;
    if (initlen && init) memcpy(data, init, initlen);
    data[initlen] = '\0';
    return data;
}

sds sdsnew(const char *init) {
    if (!init) return sdsempty();
    return sdsnewlen(init, strlen(init));
}

sds sdsempty(void) {
    return sdsnewlen("", 0);
}

sds sdsdup(const sds s) {
    if (!s) return NULL;
    return sdsnewlen(s, sdslen(s));
}

void sdsfree(sds s) {
    if (!s) return;
    char type = sds_type(s);
    free(s - sdsHdrSize(type));
}

void sdsclear(sds s) {
    char type = sds_type(s);
    switch (type & SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            s[-1] = (char)SDS_TYPE_5;
            break;
        case SDS_TYPE_8:  sds_hdr8(s)->len = 0;  break;
        case SDS_TYPE_16: sds_hdr16(s)->len = 0; break;
        case SDS_TYPE_32: sds_hdr32(s)->len = 0; break;
        case SDS_TYPE_64: sds_hdr64(s)->len = 0; break;
    }
    s[0] = '\0';
}

size_t sdslen(const sds s) {
    if (!s) return 0;
    return sdslen_type(s, sds_type(s));
}

size_t sdsavail(const sds s) {
    if (!s) return 0;
    char type = sds_type(s);
    return sdsalloc_type(s, type) - sdslen_type(s, type);
}

sds sdsMakeRoomFor(sds s, size_t addlen) {
    /* 空间已够则直接返回 */
    if (!s) return NULL;
    char oldtype = sds_type(s);
    size_t len = sdslen_type(s, oldtype);
    size_t avail = sdsalloc_type(s, oldtype) - len;
    if (avail >= addlen) return s;

    /* 需要扩容：新长度 = 旧 len + addlen */
    size_t newlen = len + addlen;
    size_t newalloc;
    if (newlen < 1024 * 1024) {
        newalloc = newlen * 2 + 1;
    } else {
        newalloc = newlen + 1024 * 1024;
    }

    /* 选能容纳 newlen 和 newalloc 的头部类型；TYPE_5 无法再增长，至少 TYPE_8 */
    char newtype = sdsReqType(newalloc < newlen ? newlen : newalloc);
    if (newtype == SDS_TYPE_5) newtype = SDS_TYPE_8;
    size_t newhdr = sdsHdrSize(newtype);

    if (newtype != oldtype) {
        /* 头部类型变化：新分配一块 + 拷贝数据 + 释放旧块。
         * 新块大小与旧块显著不同，malloc 不会与旧块同址。 */
        char *new_s = malloc(newhdr + newalloc + 1);
        if (new_s == NULL) return NULL;
        memcpy(new_s + newhdr, s, len);
        free(s - sdsHdrSize(oldtype));
        s = new_s + newhdr;
    } else {
        /* 头部类型不变：realloc 原地/就近扩容，安全处理地址复用。
         * realloc 保留旧头部与旧数据，且新旧头部大小相同，
         * 所以数据仍从 newhdr 处开始，无需搬移。 */
        void *newbase = realloc(s - sdsHdrSize(oldtype), newhdr + newalloc + 1);
        if (newbase == NULL) return NULL;
        s = (char *)newbase + newhdr;
    }

    /* 写新头部（len 不变，alloc=newalloc；TYPE_5 的 len 存 flags 高 5 位） */
    if (newtype == SDS_TYPE_5) {
        s[-1] = (char)((len << SDS_TYPE_BITS) | SDS_TYPE_5);
    } else {
        switch (newtype & SDS_TYPE_MASK) {
            case SDS_TYPE_8:
                sds_hdr8(s)->len = len;
                sds_hdr8(s)->alloc = newalloc;
                sds_hdr8(s)->flags = SDS_TYPE_8;
                break;
            case SDS_TYPE_16:
                sds_hdr16(s)->len = len;
                sds_hdr16(s)->alloc = newalloc;
                sds_hdr16(s)->flags = SDS_TYPE_16;
                break;
            case SDS_TYPE_32:
                sds_hdr32(s)->len = len;
                sds_hdr32(s)->alloc = newalloc;
                sds_hdr32(s)->flags = SDS_TYPE_32;
                break;
            case SDS_TYPE_64:
                sds_hdr64(s)->len = len;
                sds_hdr64(s)->alloc = newalloc;
                sds_hdr64(s)->flags = SDS_TYPE_64;
                break;
        }
    }
    s[len] = '\0';
    return s;
}

sds sdscatlen(sds s, const void *t, size_t addlen) {
    if (!s) return NULL;
    sds new_s = sdsMakeRoomFor(s, addlen);
    if (new_s == NULL) return NULL;
    size_t len = sdslen(new_s);
    memcpy(new_s + len, t, addlen);
    /* 按类型累加 len（TYPE_5 的 len 在 flags 高 5 位） */
    {
        char type = sds_type(new_s);
        switch (type & SDS_TYPE_MASK) {
            case SDS_TYPE_5:
                new_s[-1] = (char)(((len + addlen) << SDS_TYPE_BITS) | SDS_TYPE_5);
                break;
            case SDS_TYPE_8:  sds_hdr8(new_s)->len += addlen;  break;
            case SDS_TYPE_16: sds_hdr16(new_s)->len += addlen; break;
            case SDS_TYPE_32: sds_hdr32(new_s)->len += addlen; break;
            case SDS_TYPE_64: sds_hdr64(new_s)->len += addlen; break;
        }
    }
    new_s[len + addlen] = '\0';
    return new_s;
}

sds sdscat(sds s, const char *t) {
    if (!t) return s;
    return sdscatlen(s, t, strlen(t));
}

int sdscmp(const sds s1, const sds s2) {
    size_t l1 = sdslen(s1);
    size_t l2 = sdslen(s2);
    size_t minlen = (l1 < l2) ? l1 : l2;
    int cmp = memcmp(s1, s2, minlen);
    if (cmp != 0) return cmp;
    return (l1 > l2) ? 1 : (l1 < l2) ? -1 : 0;
}
