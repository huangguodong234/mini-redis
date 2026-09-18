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
//   哈希表；GET 走"快照拷贝"（storage_get 返回与哈希表无关的深拷贝，调用方可
//   自由释放）；del 的查询 sds 只读不接管。

// 创建并初始化存储引擎
Storage *storage_init();

// 设置键值对（如果 key 已存在则更新）-SET命令（加和改数据）
// ★ 所有权转移路径：key/value 为 sds，本层不深拷贝，直接把所有权移交给
//   哈希表（省掉两次 sdsdup）。调用方（commands 层）必须把自己手里对应的
//   argv[i] 置 NULL：
//     - 置 NULL 后，server.c 的无脑释放循环 sdsfree(argv[i]) 会因
//       sdsfree 内部的 `if (!s) return;` 判空而安全跳过该槽（NULL 是 no-op），
//       因此 server.c 的释放循环【无需】再额外判空——sdsfree 已兜底。
//     - 反过来，若某个 steal 忘记置 NULL，该槽仍是原地址（已被哈希表持有），
//       server.c 的核心释放会对它 double-free。故"每次 steal 都必须成对置 NULL"
//       是本方案的硬性契约，而不是靠释放循环判空兜底。
//   哈希表在 duplicate-key / OOM 路径自行释放传入的 sds，故不泄漏。
void storage_set_steal(Storage *s, sds key, sds value);

// 获取键对应的值，不存在返回 NULL  -GET命令（读和查数据）
// ★ 快照拷贝路径：key 为 sds 只读不接管；返回【可拥有的深拷贝】（与哈希表无关
//   的快照），调用方负责 sdsfree。这样即使随后有 SET/DEL 改动该键也不影响返回
//   值，不依赖"发送窗口内表不被改"的时序前提。NULL 表示键不存在，无需释放。
sds storage_get(Storage *s, sds key);

// 删除键值对，成功返回 1，失败返回 0  -DEL命令（删数据）
// key 为 sds，只读不接管
int storage_del(Storage *s, sds key);

// 释放整个存储引擎的内存
void storage_free(Storage *s);


#endif
