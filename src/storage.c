#include <stdio.h>
#include <stdlib.h>
#include "storage.h"
#include "sds.h"


// ============================================================
// storage.c —— mini-redis 存储引擎
// ============================================================
// ★ 存储边界：接口直接收 sds（server/commands 层传进来的就是 sds，
//   二进制安全）。本层做深拷贝后交给纯 sds 的哈希表。
//
//   生命周期约定：
//   - set：本层把 k/v 用 sdsdup 深拷贝一份，调用 hashtable_set 后
//           所有权移交给哈希表（由 hashtable_free / hashtable_del 释放），
//           本层此后不得再 sdsfree 它们。
//   - get/del：查询用 sds 是调用方传入的临时量，本层只读不接管、
//           不释放（调用方自己负责）。

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

// SET 命令的实现
// ★ key/value 已是 sds（二进制安全），深拷贝后交给哈希表
void storage_set(Storage *s, sds key, sds value) {
    // 参数检查：避免意外传入 NULL 导致崩溃
    if(!s || !key ||!value) return;

    // 深拷贝成 sds（这是持久数据，必须拷贝一份，因为调用方后续会释放自己的）
    sds k = sdsdup(key);
    sds v = sdsdup(value);
    if(!k || !v){
        sdsfree(k);   // 释放已成功的那一个
        sdsfree(v);
        fprintf(stderr, "storage_set: sdsdup 失败\n");
        return;
    }

    // 拷贝已完成，把 k/v 的所有权交给哈希表（本函数不负责释放）
    hashtable_set(s->ht,k,v);
}

// GET 命令的实现（key 只读不接管；返回内部 value 的 sds，调用方不要 free）
sds storage_get(Storage *s, sds key) {
    if(!s || !key) return NULL;
    return hashtable_get(s->ht,key);
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
