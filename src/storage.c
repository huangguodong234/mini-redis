#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage.h"

#define INIT_CAPACITY 10  // 初始容量

// 自定义的 strdup 函数（复制字符串到新分配的内存）
static char *my_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *new = malloc(len + 1);
    if (new) {
        strcpy(new, s);
    }
    return new;
}

// 初始化存储引擎
Storage *storage_init() {
    Storage *s = malloc(sizeof(Storage));           // 分配 Storage 结构体
    s->entries = malloc(sizeof(Entry) * INIT_CAPACITY); // 分配底层数组
    s->count = 0;                                   // 初始元素个数为 0
    s->capacity = INIT_CAPACITY;                    // 容量 = 10
    return s;
}

// 查找 key 的下标，找不到返回 -1
static int find_index(Storage *s, const char *key) {
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0)    // 字符串比较
            return i;
    }
    return -1;
}

// SET 命令的实现
void storage_set(Storage *s, const char *key, const char *value) {
    int idx = find_index(s, key);
    if (idx != -1) {                               // key 已存在，更新 value
        free(s->entries[idx].value);               // 释放旧值
        s->entries[idx].value = my_strdup(value);  // 复制新值
        return;
    }
    // 扩容检查
    if (s->count >= s->capacity) {
        s->capacity *= 2;                          // 容量翻倍
        s->entries = realloc(s->entries, sizeof(Entry) * s->capacity);
    }
    // 在数组末尾添加新项
    s->entries[s->count].key = my_strdup(key);
    s->entries[s->count].value = my_strdup(value);
    s->count++;
}

// GET 命令的实现
char *storage_get(Storage *s, const char *key) {
    int idx = find_index(s, key);
    if (idx == -1) return NULL;          // 没找到
    return s->entries[idx].value;        // 返回内部指针（外部不要 free）
}

// DEL 命令的实现
int storage_del(Storage *s, const char *key) {
    int idx = find_index(s, key);
    if (idx == -1) return 0;            // 没找到，返回 0
    free(s->entries[idx].key);
    free(s->entries[idx].value);
    // 把最后一个元素移到被删除的位置，保持数组紧凑
    s->entries[idx] = s->entries[s->count - 1];
    s->count--;
    return 1;
}

// 释放所有内存
void storage_free(Storage *s) {
    for (int i = 0; i < s->count; i++) {
        free(s->entries[i].key);
        free(s->entries[i].value);
    }
    free(s->entries);
    free(s);
}

