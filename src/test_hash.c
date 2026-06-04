#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "hashtable.h"

#define NUM_KEYS 10000
#define CAPACITY 1000

int main() {
    int bucket_count[CAPACITY];
    for (int i = 0; i < CAPACITY; i++) {
        bucket_count[i] = 0;
    }

    char key[32];
    for (int i = 0; i < NUM_KEYS; i++) {
        snprintf(key, sizeof(key), "key_%d", i);
        unsigned long hash = hash_djb2(key);
        int bucket = hash % CAPACITY;
        bucket_count[bucket]++;
    }

    int max_len = 0;
    int empty_buckets = 0;
    long total_sq = 0;   // 统一用 total_sq

    for (int i = 0; i < CAPACITY; i++) {
        int len = bucket_count[i];
        if (len > max_len) max_len = len;
        if (len == 0) empty_buckets++;
        total_sq += (long)len * len;   // 这里累加的是 total_sq
    }

    double avg = (double)NUM_KEYS / CAPACITY;
    double variance = (double)total_sq / CAPACITY - avg * avg;

    printf("=== djb2 哈希分布测试 ===\n");
    printf("桶容量: %d, 总 key 数: %d\n", CAPACITY, NUM_KEYS);
    printf("平均每个桶: %.2f\n", avg);
    printf("最长链表长度: %d\n", max_len);
    printf("空桶数: %d / %d (%.1f%%)\n", empty_buckets, CAPACITY,
           100.0 * empty_buckets / CAPACITY);
    printf("方差: %.4f, 标准差: %.4f\n", variance, sqrt(variance));

    if (variance >= 0 && max_len <= 2 * avg) {
        printf("结论：哈希分布均匀，djb2 表现优秀！\n");
    } else {
        printf("结论：分布可能不够均匀，请检查。\n");
    }

    return 0;
}
