#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zset.h"

// 如果系统没有 strdup，加上这行
#define _POSIX_C_SOURCE 200809L

// 创建有序集合（内部创建跳表）
ZSet *zset_create(void) {
    ZSet *zset = malloc(sizeof(ZSet));
    zset->sl=skiplist_create();
    return zset;
}

// 添加元素（委托给跳表）
void zset_add(ZSet *zset, const char *member, double score) {
    if (!zset || !member) return;
    skiplist_add(zset->sl, member, score);
}

// 范围查询（跳表版本稍后实现，先返回空）
char **zset_range(ZSet *zset, int start, int stop) {
    if(!zset){
        char **result =malloc(sizeof(char*));
        result[0]=NULL;
        return result;
    }
    // 调用跳表的范围查询函数（目前在 skiplist.c 中还是空函数）
    return skiplist_range(zset->sl,start ,stop);
}

// ==================== 释放有序集合 ====================
void zset_free(ZSet *zset) {
    if(!zset) return;
    skiplist_free(zset->sl);
    free(zset);
}
