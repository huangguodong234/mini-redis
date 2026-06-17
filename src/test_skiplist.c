#include <stdio.h>
#include <stdlib.h>
#include "skiplist.h"

int main() {
    printf("=== 跳表插入测试 ===\n\n");

    Skiplist *sl = skiplist_create();

    // 插入几个元素（故意打乱顺序）
    skiplist_add(sl, "apple", 10);
    skiplist_add(sl, "banana", 5);
    skiplist_add(sl, "cherry", 20);
    skiplist_add(sl, "date", 15);

    printf("插入 4 个元素后,size = %d, max_level = %d\n\n", sl->size, sl->max_level);

    // 测试查找
    printf("查找测试:\n");
    SkipNode *node = skiplist_find(sl, "banana");
    if (node) {
        printf("  找到 banana, score = %.0f\n", node->score);
    } else {
        printf("  未找到 banana\n");
    }

    node = skiplist_find(sl, "grape");
    if (node) {
        printf("  找到 grape, score = %.0f\n", node->score);
    } else {
        printf("  未找到 grape\n");
    }

    // 测试更新
    printf("\n更新 apple 的 score 为 100\n");
    skiplist_add(sl, "apple", 100);
    node = skiplist_find(sl, "apple");
    if (node) {
        printf("  apple 的新 score = %.0f\n", node->score);
    }

    skiplist_free(sl);
    printf("\n=== 测试完成 ===\n");
    return 0;
}
