#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zset.h"

// 创建有序集合（内部创建跳表）
ZSet *zset_create(void) {
    ZSet *zset = malloc(sizeof(ZSet));
    if(!zset){
        fprintf(stderr, "zset_create: malloc ZSet 失败\n");
        return NULL;
    }
    zset->sl=skiplist_create();
    if(!zset->sl){
        fprintf(stderr, "zset_create: skiplist_create 失败\n");
        free(zset);
        return NULL;
    }
    return zset;
}

// 添加元素（委托给跳表）
void zset_add(ZSet *zset, const char *member, double score) {
    if (!zset || !member) return;
    skiplist_add(zset->sl, member, score);
}

// 删除成员（委托给跳表）
int zset_rem(ZSet *zset, const char *member) {
    if (!zset || !member) return 0;
    return skiplist_del(zset->sl, member);
}

//指定成员 member 对应的分数（score）。
double zset_score(ZSet *zset,const char *member,bool *found){
    if(!zset || !member) {
        *found=false; //起标记的作用，用于判断成员是否存在
        return 0.0;
    }
    SkipNode *node = skiplist_find(zset->sl, member);
    if(node){
        *found=true;
        return node->score;
    }
    *found=false;
    return 0.0;
}

// 范围查询（跳表版本）
char **zset_range(ZSet *zset, int start, int stop) {
    if(!zset){
        char **result =malloc(sizeof(char*));
        if(!result) return NULL;
        result[0]=NULL;
        return result;
    }

    return skiplist_range(zset->sl,start ,stop);
}



// ==================== 释放有序集合 ====================
void zset_free(ZSet *zset) {
    if(!zset) return;
    skiplist_free(zset->sl);
    free(zset);
}
