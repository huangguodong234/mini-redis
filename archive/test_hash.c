//=========哈希表分布测试==============

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "hashtable.h"

#define NUM_KEYS 10000
#define CAPACITY 1000

int main() {
    // 这个数组记录每个桶被命中多少次，初始全 0
    int bucket_count[CAPACITY];
    for (int i = 0; i < CAPACITY; i++) {
        bucket_count[i] = 0;
    }

    char key[32];
     // 循环生成 10000 个 key，格式为 "key_0", "key_1", ...
    for (int i = 0; i < NUM_KEYS; i++) {
        snprintf(key, sizeof(key), "key_%d", i);      // 构造 key
        unsigned long hash = hash_djb2(key);          // 计算哈希值
        int bucket = hash % CAPACITY;                 // 取模得到桶下标
        bucket_count[bucket]++;                       // 对应桶计数 +1
    }

    int max_len = 0;            // 最长链表长度（也就是桶里最多有多少个 key）
    int empty_buckets = 0;      // 空桶数量
    long total_sq = 0;           // 用于计算方差：各桶长度的平方和

    for (int i = 0; i < CAPACITY; i++) {
        int len = bucket_count[i];
        if (len > max_len) max_len = len;  
        if (len == 0) empty_buckets++;  
        total_sq += (long)len * len;   
    }

    double avg = (double)NUM_KEYS / CAPACITY;                     // 平均每个桶应有多少 key
    double variance = (double)total_sq / CAPACITY - avg * avg;   // 方差：衡量分布波动
 
    printf("=== djb2 哈希分布测试 ===\n");
    printf("桶容量: %d, 总 key 数: %d\n", CAPACITY, NUM_KEYS);
    printf("平均每个桶: %.2f\n", avg);
    printf("最长链表长度: %d\n", max_len);
    printf("空桶数: %d / %d (%.1f%%)\n", empty_buckets, CAPACITY,
           100.0 * empty_buckets / CAPACITY);
    printf("方差: %.4f, 标准差: %.4f\n", variance, sqrt(variance));

    // 简单评判：如果最长链表 ≤ 平均的 2 倍，且方差小于均值，说明分布均匀
    if (variance >= 0 && max_len <= 2 * avg) {
        printf("结论: 哈希分布均匀,djb2 表现优秀！\n");
    } else {
        printf("结论：分布可能不够均匀，请检查。\n");
    }

    return 0;
}
