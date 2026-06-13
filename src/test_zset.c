#include <stdio.h>
#include <stdlib.h>
#include "zset.h"

int main() {
    printf("=== 排序链表 ZSET 测试 ===\n\n");

    ZSet *zset = zset_create();

     // 插入几个元素（注意顺序故意打乱）
    zset_add(zset, "apple", 10);
    zset_add(zset, "banana", 5);
    zset_add(zset, "cherry", 20);
    zset_add(zset, "date", 15);

    printf("插入 4 个元素后，size = %d\n\n", zset->size);

     // 范围查询：获取全部元素（按 score 从小到大
    char **result = zset_range(zset, 0, -1);
    printf("ZRANGE 0 -1（全部，按 score 排序）:\n");
    for (int i = 0; result[i]; i++) {
        printf("  %s\n", result[i]);
        free(result[i]);// 释放每个字符串
    }
    free(result);// 释放数组本身

    zset_free(zset);
    printf("\n=== 测试完成 ===\n");
    return 0;
}
