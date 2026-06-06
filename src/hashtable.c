// ============================================================
// hashtable.c —— mini-redis 哈希表的具体实现
// ============================================================
// 采用“链地址法”解决哈希冲突：
// - 每个桶是一个单向链表
// - 插入使用“头插法”把新节点插在链表头部
// - 查找和删除需要遍历对应桶的链表

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hashtable.h"

// ==================== djb2 哈希函数 ====================
// 功能：把任意字符串转换成一个无符号长整数
// 算法：hash = 5381;  hash = hash * 33 + 下一个字符的ASCII码
//       因为 33 = 32 + 1 = (2^5 + 1)，所以 hash * 33 可以写为：
//       (hash << 5) + hash   （左移5位 = 乘32，再加自己 = 乘33）
// 特点：计算快，分布均匀，Redis 等很多系统都在用
unsigned long hash_djb2(const char *str)
{
    unsigned long hash=5381;
    int c;
    while((c=*str++)){
        // hash * 33 + c
        hash =((hash << 5) + hash)+c;
    }
    return hash;
}

// ========== 创建哈希表 ==========
// 参数：initial_capacity 指定初始有多少个桶
// 返回：指向 HashTable 结构体的指针
HashTable *hashtable_creat(int initial_capacity){
    HashTable *ht =malloc(sizeof(HashTable));
    ht ->capacity =initial_capacity;
    ht ->size =0;

    // calloc 分配并清零，所有桶初始为 NULL
    // 这里用来创建桶数组，每个元素初始为 NULL（链表为空）
    ht ->buckets =calloc(initial_capacity,sizeof(HashNode *));
    return ht;
}

// ========== 插入/更新 ==========
// 参数：ht 是哈希表指针，key 和 value 是需要存入的字符串
// 行为：如果 key 已存在，则更新它的 value
//       如果 key 不存在，则在对应桶的链表头部插入新节点

void hashtable_set(HashTable *ht,const char *key,const char *value){
    // 1. 计算桶下标
    unsigned long hast=hash_djb2(key);
    int index =hast % ht ->capacity;

    // 2. 检查链表中是否已存在该 key
    HashNode *curr=ht ->buckets[index];
    while (curr){
        if(strcmp(curr->key , key)==0){
            // 已存在，更新 value
            free(curr->value);
            curr->value =strdup(value);
            return;
        }
        curr =curr->next;
    }

    // 3. 不存在，头插法插入新节点
    HashNode *nem_node =malloc(sizeof(HashNode));
    nem_node ->key =strdup(key);
    nem_node ->value=strdup(value);
    nem_node ->next =ht ->buckets[index];     // 新节点指向原头节点
    ht ->buckets[index]=nem_node;             // 桶指向新节点
    ht ->size++;
}

// ========== 查找 ==========
// 参数：ht 是哈希表指针，key 是要查找的键
// 返回：如果找到，返回 value 字符串的指针（不要 free 它！）
//       如果找不到，返回 NULL

char *hasttable_get(HashTable *ht,const char *key){
    unsigned long hash =hash_djb2(key);
    int index =hash % ht->capacity;

    HashNode *curr =ht ->buckets[index];
    while(curr){
        if(strcmp(curr ->key,key)==0){
            return curr->value;  // 返回内部指针，外部不要 free
        }
        curr =curr ->next;
    }
    return NULL;
}

// ========== 删除 ==========
// 参数：ht 是哈希表指针，key 是要删除的键
// 返回：1 表示删除成功，0 表示 key 不存在

int hashtable_del(HashTable *ht,const char *key){
    unsigned long hash=hash_djb2(key);
    int index =hash % ht->capacity;

    HashNode *curr =ht->buckets[index];
    HashNode *prev =NULL;

    while(curr){
        if(strcmp(curr ->key,key)==0){
            // 从链表中摘除节点
            if(prev ==NULL){
                ht ->buckets[index] =curr->next;// 删除头节点
            }else{
                prev->next=curr->next;     // 删除中间节点
            }
            free(curr->key);
            free(curr->value);
            free(curr);
            ht->size--;
            return 1;
        }
        prev=curr;
        curr=curr->next;
    }
    return 0;
}

// ========== 销毁 ==========
// 参数：ht 是哈希表指针
// 行为：释放哈希表占用的所有内存（节点、桶数组、表本身）

void hashtable_free(HashTable *ht){
    if(!ht)return;
    for(int i=0;i<ht->capacity;i++){
        HashNode*curr=ht->buckets[i];
        while(curr){
            HashNode *next=curr->next;
            free(curr->key);
            free(curr->value);
            free(curr);
            curr=next;
        }
    }
    free(ht->buckets);
    free(ht);
}


