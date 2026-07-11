#define _POSIX_C_SOURCE 200809L 
//strdup-字符串复制函数 是 POSIX 标准函数，在某些编译环境下可能不可用。
//通过加宏来启用标准 strdup 

// ============================================================
// hashtable.c —— mini-redis 哈希表的具体实现
// ============================================================
// 采用"链地址法"解决哈希冲突：
// - 每个桶是一个单向链表
// - 插入使用"头插法"把新节点插在链表头部
// - 查找和删除需要遍历对应桶的链表

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hashtable.h"

#define LOAD_FACTOR_THRESHOLD 0.75                             

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
HashTable *hashtable_create(int initial_capacity){
    HashTable *ht =malloc(sizeof(HashTable));
    if(!ht) {
        fprintf(stderr, "hashtable_create: malloc HashTable 失败\n");
        return NULL;
    }
    ht ->capacity =initial_capacity;
    ht ->size =0;

    // calloc 分配并清零，所有桶初始为 NULL
    // 这里用来创建桶数组，每个元素初始为 NULL（链表为空）
    ht ->buckets =calloc(initial_capacity,sizeof(HashNode *));
    if(!ht->buckets){
        fprintf(stderr, "hashtable_create: calloc buckets 失败\n");
        free(ht);
        return NULL;
    }
    return ht;
}

// ==================== 扩容函数 ====================
static void hashtable_resize(HashTable *ht){
    int old_capacity =ht->capacity;
    int new_capacity =old_capacity *2;

    printf("[扩容] %d -> %d (size=%d, 负载因子=%.2f)\n",
    old_capacity,new_capacity,ht->size,
    (double)ht->size/old_capacity);

    // 1. 创建新桶数组（全部初始化为 NULL）
    HashNode **new_buckets =calloc(new_capacity,sizeof(HashNode *));
    //可能失败返回 NULL，但未检查。
    if(!new_buckets){
        fprintf(stderr,"内存分配失败，扩容终止\n");
        return;
    }

    // 2. 遍历所有旧桶和链表，重新哈希每个节点
    for(int i=0;i<old_capacity;i++){
        HashNode *curr=ht->buckets[i];
        while(curr){
            HashNode *next =curr->next;  // 暂存下一个节点

            // 重新计算哈希值
            unsigned long hash=hash_djb2(curr->key);
            int new_index = hash% new_capacity;

            // 头插法插入到新桶
            curr ->next =new_buckets[new_index];
            new_buckets[new_index]=curr;

            // 继续处理下一个
            curr=next;
        }
    }

    // 3. 释放旧桶数组，更新为新的
    free(ht ->buckets);
    ht ->buckets =new_buckets;
    ht ->capacity =new_capacity;
}

// ========== 插入/更新 ==========
// 参数：ht 是哈希表指针，key 和 value 是需要存入的字符串
// 行为：如果 key 已存在，则更新它的 value
//       如果 key 不存在，则在对应桶的链表头部插入新节点

void hashtable_set(HashTable *ht,const char *key,const char *value){
    // 检查是否需要扩容（在新增元素之前）-检查 负载因子=元素数量/容器容量
    double load_factor =(double)ht->size /ht->capacity;
    if(load_factor >=LOAD_FACTOR_THRESHOLD){
         //LOAD_FACTOR_THRESHOLD -负载因子阈值
         hashtable_resize(ht);
    }

    // 计算桶下标
    unsigned long hash=hash_djb2(key);
    int index =hash % ht ->capacity;

    // 检查链表中是否已存在该 key
    HashNode *curr=ht ->buckets[index];
    while (curr){
        if(strcmp(curr->key , key)==0){
            // 已存在，更新 value（先 strdup 再 free，避免 strdup 失败丢数据）
            char *new_val =strdup(value);
            if(!new_val){
                fprintf(stderr, "hashtable_set: strdup value 失败\n");
                return;   // 旧 value 保留，程序继续跑
            }
            free(curr->value);
            curr->value =new_val;
            return;
        }
        curr =curr->next;
    }

    // 不存在，头插法插入新节点
    HashNode *new_node =malloc(sizeof(HashNode));
    if(!new_node){
        fprintf(stderr, "hashtable_set: malloc HashNode 失败\n");
        return;
    }
    new_node ->key =strdup(key);
    if(!new_node->key){
        fprintf(stderr, "hashtable_set: strdup key 失败\n");
        free(new_node);
        return;
    }
    new_node ->value=strdup(value);
    if(!new_node->value){
        fprintf(stderr, "hashtable_set: strdup value 失败\n");
        free(new_node->key);
        free(new_node);
        return;
    }
    new_node ->next =ht ->buckets[index];     // 新节点指向原头节点
    ht ->buckets[index]=new_node;             // 桶指向新节点
    ht ->size++;
}

// ========== 查找 ==========
// 参数：ht 是哈希表指针，key 是要查找的键
// 返回：如果找到，返回 value 字符串的指针（不要 free 它！）
//       如果找不到，返回 NULL

char *hashtable_get(HashTable *ht,const char *key){
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


