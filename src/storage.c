#include <stdio.h>
#include <stdlib.h>
#include "storage.h"
#include "sds.h"


// ============================================================
// storage.c —— mini-redis 存储引擎
// ============================================================
// ★ 存储边界：接口直接收 sds（server/commands 层传进来的就是 sds，
//   二进制安全）。
//
//   生命周期约定：
//   - set：走"所有权转移"（storage_set_steal），本层不深拷贝，直接把
//           k/v 的 sds 所有权移交给哈希表（由 hashtable_free /
//           hashtable_del / duplicate-key 覆盖释放），调用方须把对应
//           argv[i] 置 NULL，本层此后不得再 sdsfree 它们。
//           ※ 置 NULL 后 server.c 的无脑释放循环会因 sdsfree 内部的
//             `if(!s) return;` 判空而安全跳过该槽，故 server.c 释放循环
//             无需额外判空；重点是"每次 steal 必须成对置 NULL"这条硬契约。
//   - get：快照拷贝路径，返回可拥有的深拷贝，调用方负责 sdsfree。
//   - del：查询用 sds 是调用方传入的临时量，本层只读不接管、不释放
//          （调用方自己负责）。

// 初始化存储引擎
Storage *storage_init() {
    Storage *s=malloc(sizeof(Storage));
    if(!s){
        fprintf(stderr, "storage_init: malloc Storage 失败\n");
        return NULL;
    }
    // 创建初始容量为 16 的哈希表（容量会自动扩容）
    s->ht = hashtable_create(16);
    if(!s->ht){
        fprintf(stderr, "storage_init: hashtable_create 失败\n");
        free(s);
        return NULL;
    }
    return s;
}

// SET 命令的“所有权转移”路径（argv 不释放给哈希）
// ★ 唯一的 set 路径：直接接管调用方解析出的 key/value sds 的所有权移交给
//   哈希表（不深拷贝；省掉每 SET 两次 sdsdup 的 malloc+memcpy，也省掉
//   server 层后续对这两个元素的两次 sdsfree）。
//   ⚠ 调用方（commands 层）必须把自己手里对应的 argv[i] 置 NULL，
//   避免 server.c 误 free（哈希表现在拥有它们）；置 NULL 后 server.c 的
//   无脑释放循环因 sdsfree 内部 `if(!s) return;` 判空而安全跳过该槽，
//   【无需】再给 server.c 加判空；哈希表在 duplicate-key / OOM 拒绝路径
//   自行释放传入的 sds，故不泄漏。
void storage_set_steal(Storage *s, sds key, sds value) {
    if(!s || !key || !value) return;
    // 所有权直接移交，不再拷贝；hashtable_set 会负责持有，重复 key 时自释放
    hashtable_set(s->ht, key, value);
}

// GET 命令的实现（key 只读不接管）
// ★ 快照拷贝路径：底层 hashtable_get 返回哈希表内部 value 的活引用（所有权归
//   哈希表），本层不把活引用直接交给调用方，而是 sdsdup 深拷贝一份"可拥有"的
//   快照返回。这样调用方拿到的是与哈希表完全无关的独立副本，可自由 sdsfree，
//   即使随后有 SET/DEL 改动哈希表该键也不影响它——返回值自给自足，不依赖
//   "发送窗口内表不被改"这个时序前提。
//   ⚠ 前提：hashtable_get 期间哈希表必须稳定（本层内部一次查完即拷贝）。
//   ⚠ 调用方负责 sdsfree 返回值（NULL 表示键不存在，无需释放）。
sds storage_get(Storage *s, sds key) {
    if(!s || !key) return NULL;
    char *internal = hashtable_get(s->ht,key);
    if(!internal) return NULL;
    return sdsdup((sds)internal);  // 返回可拥有的深拷贝
}

// DEL 命令的实现（key 只读不接管）
int storage_del(Storage *s, sds key) {
    if(!s || !key) return 0;
    return hashtable_del(s->ht,key);
}

// 释放所有内存
void storage_free(Storage *s) {
    if(!s) return;
    hashtable_free(s->ht);    // 释放哈希表内部所有节点（含 sds 键值）
    free(s);                  // 释放 Storage 结构体
}
