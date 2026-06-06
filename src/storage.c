#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage.h"


// 初始化存储引擎
Storage *storage_init() {
    Storage *s=malloc(sizeof(Storage));
    // 创建初始容量为 16 的哈希表（容量会自动扩容）
    s->ht = hashtable_creat(16);
    return s;
}

// SET 命令的实现
void storage_set(Storage *s, const char *key, const char *value) {
    hashtable_set(s->ht,key,value);
}

// GET 命令的实现
char *storage_get(Storage *s, const char *key) {
    return hashtable_get(s->ht,key);
}

// DEL 命令的实现
int storage_del(Storage *s, const char *key) {
    return hashtable_del(s->ht,key);
}

// 释放所有内存
void storage_free(Storage *s) {
    if(!s) return;
    hashtable_free(s->ht);    // 释放哈希表内部所有节点
    free(s);                  // 释放 Storage 结构体
}

