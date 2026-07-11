#ifndef HASHTABLE_H
#define HASHTABLE_H

//一个动态数组+一个链表

// 哈希表节点（链表节点）
typedef struct HashNode{
    char *key;
    char *value;
    struct HashNode *next;    // 指向下一个节点
}HashNode;

//哈希表结构体
typedef struct{
    HashNode **buckets;  // 桶数组（指针数组）
    int size;            // 当前存储的键值对个数
    int capacity;         // 桶的数量
}HashTable;

// djb2 哈希函数
unsigned long hash_djb2(const char *str);

// 创建哈希表，initial_capacity 是初始桶数
HashTable *hashtable_create(int initial_capacity);

//插入或更新键值对-SET命令
void hashtable_set(HashTable *ht,const char*key,const char *value);

//查找 key，返回 value 指针（找不到返回 NULL）-GET命令
char *hashtable_get(HashTable *ht,const char *key);

// 删除 key，返回 1 成功，0 不存在-DEL命令
int hashtable_del(HashTable *ht,const char *key);

//销毁哈希表，释放所有内存
void hashtable_free(HashTable*ht);

#endif
