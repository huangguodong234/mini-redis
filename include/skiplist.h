#ifndef SKIPLIST_H
#define SKIPLIST_H

#define SKIPLIST_MAX_LEVEL 32  // 跳表最大层数

// 跳表节点
typedef struct SkipNode{
    char *member;                  //成员名
    double score;                 //跳表中所有节点按 score 从小到大排列。  
    struct SkipNode **forward;     //forward[i] 的含义：在第 i 层上，
                                   //指向下一个 score 大于本节点的节点。
    int level;                     //这个节点有多少层
}SkipNode;

// 跳表结构体
typedef struct{
    SkipNode *header;  //不存储任何数据，只作为查找的起点。
    int max_level;     //当前跳表中，除了 header 以外，存在的最高层级。
    int size;          //跳表中实际存储的元素个数（不包括 header）。
}Skiplist;

// 创建空跳表，初始化表头、层数等基础信息
// 返回值：成功返回跳表结构体指针，失败返回NULL
Skiplist *skiplist_create(void);

// 向跳表中插入/更新节点
// sl: 跳表结构体指针
// member: 节点成员名（唯一标识）
// score: 节点排序分数，跳表按score升序排列
void skiplist_add(Skiplist *sl,const char *member,double score);

// 根据成员名删除跳表中的节点
// sl: 跳表结构体指针
// member: 待删除的成员名
// 返回值：删除成功返回1，节点不存在/失败返回0
int skiplist_del(Skiplist *sl,const char *member);

// 范围查询，按顺序截取区间内所有成员名
// sl: 跳表结构体指针
// start: 起始下标（从0开始）
// stop: 结束下标
// 返回值：字符串数组，存放区间内所有member；需调用方手动释放内存
char **skiplist_range(Skiplist *sl,int start,int stop);

// 释放整个跳表所有节点及内存，防止内存泄漏
// sl: 待销毁的跳表结构体指针
void skiplist_free(Skiplist *sl);

#endif
