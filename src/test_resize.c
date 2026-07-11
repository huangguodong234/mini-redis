#include <stdio.h>
#include <stdlib.h>
#include "hashtable.h"

int main()
{
    printf("=== 哈希表自动扩容测试 ===\n\n");

    HashTable *ht =hashtable_create(4);
    printf("初始容量: %d\n\n", ht->capacity);

    //插入key
    for(int i=0;i<10;i++){
        char key[32];
        snprintf(key,sizeof(key),"key_%d",i);
        hashtable_set(ht,key,"value");
        printf("插入 '%s' 后: size=%d, capacity=%d\n", key, ht->size, ht->capacity);
    }

    printf("\n验证所有 key:\n");
    for(int i=0;i<10;i++){
        char key[32];
        snprintf(key,sizeof(key),"key_%d",i);
        char *val =hashtable_get(ht,key);
        printf("  GET %s = %s\n", key, val ? val : "(null)");
    }
    hashtable_free(ht);
    printf("\n=== 测试完成 ===\n");
    return 0;
}
