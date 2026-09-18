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

// 添加元素（member 为命令层传来的 sds，"所有权转移"路径）
// ★ 存储边界：本层不再 sdsdup 深拷贝，直接把 member 的 sds 所有权移交给跳表
//   （skiplist_add 直接接管，失败/同分路径自行 sdsfree）。调用方（commands 层
//   ZADD 分支）必须把自己手里对应的 argv[i] 置 NULL，避免 server.c 误 free。
void zset_add(ZSet *zset, sds member, double score) {
    if (!zset || !member) return;
    skiplist_add(zset->sl, member, score);   // 所有权移交跳表（不再拷贝）
}

// 删除成员（查询用 member 只读不接管）
int zset_rem(ZSet *zset, sds member) {
    if (!zset || !member) return 0;
    return skiplist_del(zset->sl, member);
}

//指定成员 member 对应的分数（score）。
double zset_score(ZSet *zset,sds member,bool *found){
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

// 范围查询（对外返回可拥有的 sds 副本；拷贝统一在本层做，跳表只给借用引用）
sds *zset_range(ZSet *zset, int start, int stop) {
    if(!zset){
        sds *result =malloc(sizeof(sds));
        if(!result) return NULL;
        result[0]=NULL;
        return result;
    }

    // 从跳表拿到借用的 member 引用数组（NULL 结尾，元素不 free）
    sds *borrowed = skiplist_range(zset->sl, start, stop);
    if(!borrowed) return NULL;

    // 逐个深拷贝成可拥有的副本（所有权归调用方，需逐个 sdsfree + free 数组）
    int i = 0;
    while (borrowed[i]) {
        sds owned = sdsdup(borrowed[i]);
        if(!owned){
            // B8 修复：OOM 时不能把 NULL/未初始化值留在结果数组里
            //（上层 while(member[count]) 会读到垃圾指针崩溃）。
            // 释放已拷贝部分，归还数组，返回 NULL 走 OOM 断开路径
            for(int j=0; j<i; j++) sdsfree(borrowed[j]);
            free(borrowed);
            return NULL;
        }
        borrowed[i] = owned;   // 用副本覆盖借用引用
        i++;
    }
    return borrowed;
}



// ==================== 释放有序集合 ====================
void zset_free(ZSet *zset) {
    if(!zset) return;
    skiplist_free(zset->sl);
    free(zset);
}
