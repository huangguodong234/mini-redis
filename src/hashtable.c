#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hashtable.h"

// ========== djb2 哈希函数 ==========
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
HashTable *hashtable_creat(int initial_capacity){
    HashTable *ht =malloc(sizeof(HashTable));
    ht ->capacity =initial_capacity;
    ht ->size =0;

    // calloc 分配并清零，所有桶初始为 NULL
    ht ->buckets =calloc(initial_capacity,sizeof(HashNode *));
    return ht;
}

// ========== 插入/更新 ==========
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


