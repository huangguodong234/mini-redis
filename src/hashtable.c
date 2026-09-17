// ============================================================
// hashtable.c —— mini-redis 哈希表的具体实现
// ============================================================
// 采用"链地址法"解决哈希冲突：
// - 每个桶是一个单向链表
// - 插入使用"头插法"把新节点插在链表头部
// - 查找和删除需要遍历对应桶的链表
//
// ★ 纯 sds 世界：key/value 一律以 sds 传入、以 sds 存储。
//   - 接口只收 sds，内部不做任何 C 字符串 <-> sds 转换
//   - 转换（C 字符串 -> sds 的拷贝）一律由调用方（storage 层）完成
//   - 传入的 key/value 由哈希表接管所有权并在 hashtable_free/del 释放

#include <stdio.h>
#include <stdlib.h>
#include "hashtable.h"
#include "sds.h"

#define LOAD_FACTOR_THRESHOLD 0.75

// ==================== djb2 哈希函数 ====================
// 按 sds 的 len 逐字节哈希，二进制安全：
// 关键：走 sdslen 而非 '\0'，key 里即使含 '\0' 也不会提前终止
// 算法：hash = 5381;  hash = hash * 33 + 下一个字符
//      因为 33 = 32 + 1 = (2^5 + 1)，所以 hash*33 = (hash<<5) + hash
static unsigned long hash_djb2_sds(const sds str)
{
    unsigned long hash = 5381;
    size_t len = sdslen(str);
    for (size_t i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + (unsigned char)str[i];
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
    if(!new_buckets){
        fprintf(stderr,"内存分配失败，扩容终止\n");
        return;
    }

    // 2. 遍历所有旧桶和链表，重新哈希每个节点（key 是 sds，二进制安全哈希）
    for(int i=0;i<old_capacity;i++){
        HashNode *curr=ht->buckets[i];
        while(curr){
            HashNode *next =curr->next;  // 暂存下一个节点

            unsigned long hash=hash_djb2_sds(curr->key);
            int new_index = hash% new_capacity;

            // 头插法插入到新桶
            curr ->next =new_buckets[new_index];
            new_buckets[new_index]=curr;

            curr=next;
        }
    }

    // 3. 释放旧桶数组，更新为新的
    free(ht ->buckets);
    ht ->buckets =new_buckets;
    ht ->capacity =new_capacity;
}

// ========== 插入/更新 ==========
// 参数：ht 是哈希表指针，key 和 value 是 sds（由调用方建好，本函数接管所有权）
// 行为：如果 key 已存在，则更新它的 value；不存在则在桶链表头部插入新节点
// 注意：传入的 key/value 在插入或更新成功后都由本表持有，最终在
//       hashtable_free / hashtable_del 释放。调用方不要再去 sdsfree 它们。

void hashtable_set(HashTable *ht,sds key,sds value){
    // 检查是否需要扩容（在新增元素之前）-检查 负载因子=元素数量/容器容量
    double load_factor =(double)ht->size /ht->capacity;
    if(load_factor >=LOAD_FACTOR_THRESHOLD){
         hashtable_resize(ht);
    }

    // 计算桶下标
    unsigned long hash=hash_djb2_sds(key);
    int index =hash % ht ->capacity;

    // 检查链表中是否已存在该 key
    HashNode *curr=ht ->buckets[index];
    while (curr){
        if(sdscmp(curr->key , key)==0){
            // 已存在，更新 value：释放旧 value，接管新 value。
            // key 不重复持有（已有），这里必须释放调用方传入的多余 key，
            // 否则泄漏。
            sdsfree(curr->value);
            curr->value =value;
            sdsfree(key);   // 查询用的 key 与存储 key 重复，释放这一份
            return;
        }
        curr =curr->next;
    }

    // 不存在，头插法插入新节点
    HashNode *new_node =malloc(sizeof(HashNode));
    if(!new_node){
        fprintf(stderr, "hashtable_set: malloc HashNode 失败\n");
        sdsfree(key);
        sdsfree(value);
        return;
    }
    new_node ->key =key;                  // 直接持有传入的 sds（已拷贝）
    new_node ->value=value;               // 直接持有传入的 sds（已拷贝）
    new_node ->next =ht ->buckets[index];     // 新节点指向原头节点
    ht ->buckets[index]=new_node;             // 桶指向新节点
    ht ->size++;
}

// ========== 查找 ==========
// 参数：ht 是哈希表指针，key 是 sds 查询键（只读，不接管）
// 返回：如果找到，返回内部 value 的 sds（本质是 char*，外部不要 free）
//       如果找不到，返回 NULL
char *hashtable_get(HashTable *ht,sds key){
    unsigned long hash =hash_djb2_sds(key);
    int index =hash % ht->capacity;

    HashNode *curr =ht ->buckets[index];
    while(curr){
        if(sdscmp(curr ->key,key)==0){
            return curr->value;  // 返回内部指针，外部不要 free
        }
        curr =curr ->next;
    }
    return NULL;
}

// ========== 删除 ==========
// 参数：ht 是哈希表指针，key 是 sds 查询键（只读，不接管）
// 返回：1 表示删除成功，0 表示 key 不存在
int hashtable_del(HashTable *ht,sds key){
    unsigned long hash=hash_djb2_sds(key);
    int index =hash % ht->capacity;

    HashNode *curr =ht->buckets[index];
    HashNode *prev =NULL;

    while(curr){
        if(sdscmp(curr ->key,key)==0){
            // 从链表中摘除节点
            if(prev ==NULL){
                ht ->buckets[index] =curr->next;// 删除头节点
            }else{
                prev->next=curr->next;     // 删除中间节点
            }
            sdsfree(curr->key);            // 释放 key 的 sds
            sdsfree(curr->value);          // 释放 value 的 sds
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
// 行为：释放哈希表占用的所有内存（sds 键值、节点、桶数组、表本身）
void hashtable_free(HashTable *ht){
    if(!ht)return;
    for(int i=0;i<ht->capacity;i++){
        HashNode*curr=ht->buckets[i];
        while(curr){
            HashNode *next=curr->next;
            sdsfree(curr->key);
            sdsfree(curr->value);
            free(curr);
            curr=next;
        }
    }
    free(ht->buckets);
    free(ht);
}
