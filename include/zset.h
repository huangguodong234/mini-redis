#ifndef ZSET_H
#define ZSET_H
#include "skiplist.h"   // 引入跳表定义

// 有序集合节点(排序链表版本加入跳表）
typedef struct{
    Skiplist *sl;   // 跳表指针
}ZSet;

// 创建有序集合
ZSet *zset_create(void);

// 添加元素（如果 member 已存在，则更新 score）
void zset_add(ZSet *zset, const char *member, double score);

// 范围查询：返回 score 排名在 [start, stop] 之间的 member 列表
// stop 为 -1 表示到最后一个元素
// 返回的是动态分配的字符串数组，最后以 NULL 结尾

char **zset_range(ZSet *zset, int start, int stop);

// 释放有序集合
void zset_free(ZSet *zset);

#endif
