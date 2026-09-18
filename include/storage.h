#ifndef STORAGE_H
#define STORAGE_H

#include "hashtable.h"    //因为要用 HashTable

// 动态数组存储结构变哈希表，时间复杂度从O（n）变o(1) 
typedef struct {
    HashTable *ht; // 底层存储变成哈希表
} Storage;

// ★ 本层是存储边界（不再做 C 字符串 <-> sds 转换）：
//   接口对外直接收 sds（server/commands 层传进来的就是 sds，二进制安全）；
//   SET 走"所有权转移"（storage_set_steal），key/value 的 sds 所有权移交给
//   哈希表；get/del 的查询 sds 只读不接管。

// 创建并初始化存储引擎
Storage *storage_init();

// 设置键值对（如果 key 已存在则更新）-SET命令（加和改数据）
// ★ 所有权转移路径：key/value 为 sds，本层不深拷贝，直接把所有权移交给
//   哈希表（省掉两次 sdsdup）。调用方（commands 层）必须把自己手里对应的
//   argv[i] 置 NULL，避免误 free；哈希表在 duplicate-key / OOM 路径自行释放
//   传入的 sds，故不泄漏。
void storage_set_steal(Storage *s, sds key, sds value);

// 获取键对应的值，不存在返回 NULL  -GET命令（读和查数据）
// key 为 sds，只读不接管；返回内部 value 的 sds，调用方不要 free
sds storage_get(Storage *s, sds key);

// 删除键值对，成功返回 1，失败返回 0  -DEL命令（删数据）
// key 为 sds，只读不接管
int storage_del(Storage *s, sds key);

// 释放整个存储引擎的内存
void storage_free(Storage *s);


#endif
