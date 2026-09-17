#ifndef STORAGE_H
#define STORAGE_H

#include "hashtable.h"    //因为要用 HashTable

// 动态数组存储结构变哈希表，时间复杂度从O（n）变o(1) 
typedef struct {
    HashTable *ht; // 底层存储变成哈希表
} Storage;

// ★ 本层是 C 字符串 <-> sds 的转换边界：
//   接口对外仍收 const char *（server/commands 层不变）；
//   内部把 key/value 拷贝成 sds 再交给纯 sds 的哈希表。set 时
//   所有权移交给哈希表，get/del 的查询 sds 用后在本层释放。

// 创建并初始化存储引擎
Storage *storage_init();

// 设置键值对（如果 key 已存在则更新）-SET命令（加和改数据）
void storage_set(Storage *s, const char *key, const char *value);

// 获取键对应的值，不存在返回 NULL  -GET命令（读和查数据）
char *storage_get(Storage *s, const char *key);

// 删除键值对，成功返回 1，失败返回 0  -DEL命令（删数据）
int storage_del(Storage *s, const char *key);

// 释放整个存储引擎的内存
void storage_free(Storage *s);


#endif
