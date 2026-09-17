#include <stdio.h>
#include <stdlib.h>
#include "storage.h"
#include "sds.h"


// ============================================================
// storage.c —— mini-redis 存储引擎
// ============================================================
// ★ 这里就是 C 字符串 <-> sds 的转换边界：
//   - server / commands 层传进来的是 const char *（C 字符串）
//   - 本层把它们拷贝成 sds，再交给纯 sds 世界的哈希表
//   - 这样转换点集中、一眼可见；哈希表内部不再做任何转换
//
//   生命周期约定：
//   - set：本层建好的 k/v sds 在调用 hashtable_set 后，所有权
//           移交给哈希表（由 hashtable_free / hashtable_del 释放），
//           本层此后不得再 sdsfree 它们。
//   - get/del：查询用 sds 是临时量，用完后在本层就地 sdsfree。

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
// ★ C 字符串 -> sds 的转换（拷贝）就发生在这里
void storage_set(Storage *s, const char *key, const char *value) {
    // 参数检查：避免意外传入 NULL 导致崩溃
    if(!s || !key ||!value) return;

    // 把 C 字符串转成 sds（这是持久数据，必须拷贝一份，
    // 因为 server 层传进来的内存生命周期是函数级的）
    sds k = sdsnew(key);
    sds v = sdsnew(value);
    if(!k || !v){
        sdsfree(k);   // 释放已成功的那一个
        sdsfree(v);
        fprintf(stderr, "storage_set: sdsnew 失败\n");
        return;
    }

    // 拷贝已完成，把 k/v 的所有权交给哈希表（本函数不负责释放）
    hashtable_set(s->ht,k,v);
}

// GET 命令的实现
char *storage_get(Storage *s, const char *key) {
    if(!s || !key) return NULL;

    // 临时查询 sds，用完就地释放
    sds k = sdsnew(key);
    if(!k) return NULL;

    char *val = hashtable_get(s->ht,k);
    sdsfree(k);          // 临时 sds 用完释放
    return val;
}

// DEL 命令的实现
int storage_del(Storage *s, const char *key) {
    if(!s || !key) return 0;

    sds k = sdsnew(key);
    if(!k) return 0;

    int result = hashtable_del(s->ht,k);
    sdsfree(k);          // 临时 sds 用完释放
    return result;
}

// 释放所有内存
void storage_free(Storage *s) {
    if(!s) return;
    hashtable_free(s->ht);    // 释放哈希表内部所有节点（含 sds 键值）
    free(s);                  // 释放 Storage 结构体
}
