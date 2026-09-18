#ifndef ZSET_H
#define ZSET_H
#include "skiplist.h"   // 引入跳表定义
#include <stdbool.h>

// 有序集合节点(排序链表版本加入跳表）
typedef struct{
    Skiplist *sl;   // 跳表指针
}ZSet;

// 创建有序集合
ZSet *zset_create(void);

// 添加元素（如果 member 已存在，则更新 score）-ZADD命令
// member 为 sds（二进制安全）；本层直接接管所有权移交跳表（不深拷贝）。
// ⚠ 调用方必须把自己手里对应的 argv[i] 置 NULL，避免误 free；
//   跳表在 duplicate/同分/OOM 路径自行释放传入的 sds，故不泄漏。
void zset_add(ZSet *zset, sds member, double score);

// 删除指定成员，成功返回1，不存在返回0-ZREM命令
// member 为 sds，只读不接管
int zset_rem(ZSet *zset, sds member); 

//指定成员 member 对应的分数（score）。-ZSCORE命令
// member 为 sds，只读不接管
double zset_score(ZSet *zset,sds member,bool *found);

// 范围查询：返回 score 排名在 [start, stop] 之间的 member 列表-ZRANGE命令
// stop 为 -1 表示到最后一个元素
// ★ 返回的是本层深拷贝出的 sds 数组（每个元素可拥有），最后以 NULL 结尾
//   （需逐个 sdsfree + free 数组）；OOM 时返回 NULL
sds *zset_range(ZSet *zset, int start, int stop);

// 释放有序集合
void zset_free(ZSet *zset);

#endif
