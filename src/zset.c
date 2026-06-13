#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zset.h"

// 如果系统没有 strdup，加上这行
#define _POSIX_C_SOURCE 200809L

// ==================== 创建有序集合 ====================
ZSet *zset_create(void) {
    ZSet *zset = malloc(sizeof(ZSet));
    zset->head = NULL;                         // 空链表
    zset->size = 0;
    return zset;
}

// ==================== 添加元素 ====================
void zset_add(ZSet *zset, const char *member, double score) {
      // 1. 检查 member 是否已存在，如果存在则更新 score
    ZSetNode *curr = zset->head;
    while (curr) {
        if (strcmp(curr->member, member) == 0) {
                // 找到了，更新分数
            curr->score = score;
                 // 注意：真实 Redis 会重新排序，但我们先简化处理
            return;
        }
        curr = curr->next;
    }

     // 2. member 不存在，创建新节点
    ZSetNode *new_node = malloc(sizeof(ZSetNode));
    new_node->member = strdup(member);            // 复制字符串
    new_node->score = score;

     // 3. 找到正确的位置插入（保持按 score 从小到大排序）
    if (zset->head == NULL || zset->head->score > score) {
          // 插入到链表头部
        new_node->next = zset->head;
        zset->head = new_node;
    } else {
          // 遍历找到插入点：prev 之后插入
        ZSetNode *prev = zset->head;
        while (prev->next && prev->next->score <= score) {
            prev = prev->next;
        }
        new_node->next = prev->next;
        prev->next = new_node;
    }

    zset->size++;         // 元素个数 +1
}

// ==================== 范围查询 ====================
char **zset_range(ZSet *zset, int start, int stop) {
     // 处理负数索引
    if (start < 0) start = 0;
    if (stop < 0 || stop >= zset->size) stop = zset->size - 1;

     // 无效范围
    if (start > stop || start >= zset->size) {
        char **result = malloc(sizeof(char *));
        result[0] = NULL;
        return result;
    }

    int count = stop - start + 1;
    char **result = malloc(sizeof(char *) * (count + 1));    // +1 放 NULL 结尾

    ZSetNode *curr = zset->head;
     // 跳过前 start 个元素
    for (int i = 0; i < start && curr; i++) {    //curr 确保指针 curr 不是空指针
        curr = curr->next;
    }

     // 收集 count 个元素
    for (int i = 0; i < count && curr; i++) {
        result[i] = strdup(curr->member);
        curr = curr->next;
    }
    result[count] = NULL;

    return result;
}

// ==================== 释放有序集合 ====================
void zset_free(ZSet *zset) {
    ZSetNode *curr = zset->head;
    while (curr) {
        ZSetNode *next = curr->next;
        free(curr->member);
        free(curr);
        curr = next;
    }
    free(zset);
}
