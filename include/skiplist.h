#ifndef SKIPLIST_H
#define SKIPLIST_H

#include "sds.h"

/*我的跳表和Redis的跳表的区别：
*   1.跳表结构体的不同：
*      （1）我的跳表结构体是一个单向链表，而 Redis 跳表在第 0 层有 backward 后退指针，所以支持从尾向头遍历。但高层只有 forward，所以它本质是"双向跳表 + 第 0 层双向"。
*      （2）我是每层都单独记前进指针和层数，而Redis是创建一个level结构体，把前进指针和span字段（节点跨度）放进去。
*           
*        优点：
*        (1) redis用双向链表，可以处理正序和逆序访问。
*        (2) 我的没有记录span，导致我在做范围查询时，我要通过for循环先找到开始读取的数，才可以输出
*        而Redis可以在记每个节点的前进指针时，记一下每层此节点的排名，在范围查询和问排名时，可以通过跳表跳转时，读取排名，这样不用在通过for循环现在找到开始节点，再收集数据。
*        加一个span可以使得在跳表在存了大量的数据时，在要范围查询/问排名时，不用for循环遍历链表，而是可以用跳表跳转时，读取排名的方法，使得在范围查询和问排名时也可以发挥跳表的优势
*        使得执行速度从for循环的O(n)--> 跳表的O(log n)。
*
*    2.从member查询score的方式不同：
*      我的是通过for循环，在跳表的第0层的链表里通过遍历的方式来查找score，
*      而Redis是通过查找哈希表来查找score的。使得查找效率从O(n)-->O(1)
*/

/*   核心差异：
*        Redis 有 span → 范围查询 O(log N)，我的 O(N)
*        Redis 用哈希表辅助 member→score 映射 O(1)，我的 O(N)
*        Redis 有 backward 指针支持逆序遍历
*        Redis 删除时支持内存所有权移交
*
*   ★ 本次升级（对照上面两条差异来打的补丁）：
*        (A) 引入 SkipListLevel {forward, span}：给每个前进指针记录 span（该层
*            一跳跨越的节点数，含目标），于是跳表跳转时能顺带累加出"第几个"，
*            范围查询 / 问排名从 O(N) 变 O(log N)。
*        (B) 引入 member→SkipNode 的哈希索引（桶数组直接在 Skiplist 里）：让
*            skiplist_find 从 O(N) 遍历第 0 层 变成 O(1) 哈希直查，同 Redis 的
*            dict 辅助表。member→score 查找（zset_score）也随之变 O(1)。
*        (C) 第 0 层仍是单向（member 查询无需 back 遍历），本轮不引入 backward。
*/

#define SKIPLIST_MAX_LEVEL 32  // 跳表最大层数

// 跳表的一层：前进指针 + 跨度 span
// span 语义（同 Redis）：从"本节点"沿该层 forward 跳到"目标节点"，中间跨越的
// 节点总数（把目标自己也算 1 个）。等价于：目标在链表第 0 层里的排名 − 本节点
// 排名。header 每一层的 span 从下标 1 起标，累计"从链表头沿该层一跳能跳过的节点数"。
typedef struct SkipListLevel{
    struct SkipNode *forward;   // 该层指向的下一个 score 更大的节点
    unsigned long span;         // 本节点 → forward 这一跳的步长：跨过几个节点（含 forward 自己）；
}SkipListLevel;

// 跳表节点
typedef struct SkipNode{
    sds member;                    //成员名（sds，二进制安全）
    double score;                 //跳表中所有节点按 score 从小到大排列。
    int level;                     //这个节点有多少层（1..SKIPLIST_MAX_LEVEL）
    SkipListLevel *lvl;            //lvl[i]：第 i 层的 {forward, span}。
                                   //旧版是独立的 SkipNode **forward；现在把前进指针
                                   //和 span 一起放进 SkipListLevel，Redis 同款结构。
}SkipNode;

// 内部哈希索引的桶节点（用于 O(1) 的 member → SkipNode 查找，同 Redis 的 dict）
// key 是【借用引用】：指向所属 SkipNode 自己的 member（非持有、不 free）。
// node 也是【借用引用】：指向索引对应的跳表节点（节点的生命周期由跳表管理）。
typedef struct IndexNode{
    sds key;                     // 借用：即 所指向 SkipNode 的 member
    SkipNode *node;              // 借用：索引指向的跳表节点
    struct IndexNode *next;      // 链地址法下一个节点
}IndexNode;

// 跳表结构体
typedef struct{
    SkipNode *header;  //不存储任何数据，只作为查找的起点。
    int max_level;     //当前跳表中，除了 header 以外，存在的最高层级。
    int size;          //跳表中实际存储的元素个数（不包括 header）。
    IndexNode **buckets;  // ★ 新增：member→SkipNode 哈希索引的桶数组（O(1) 查找）
    int index_size;       // 索引中已存键的个数
    int index_cap;        // 索引桶数
}Skiplist;

// 创建空跳表，初始化表头、层数等基础信息
// 返回值：成功返回跳表结构体指针，失败返回NULL
Skiplist *skiplist_create(void);

// 向跳表中插入/更新节点
// sl: 跳表结构体指针
// member: 节点成员名（sds，二进制安全）。★所有权约定：跳表不拷贝，直接接管
//         本次传入的 sds（调用方 zset 需已 sdsdup 深拷贝）；若本函数未存储它
//         （同分提前返回 / 分配失败），会在函数内 sdsfree 归还，调用方不再负责。
// score: 节点排序分数，跳表按score升序排列
// ★ 返回：1 = 成功（含"同分无变化"这种未实际插入的情形）；0 = 失败（OOM，未写入）。
//   ★ 所有权契约（P0-4 修复）：无论返回 1 还是 0，member 的所有权都在返回时
//   转移完毕（成功 → 跳表持有；失败 → 已在本函数内 sdsfree）。调用方一律置 NULL。
int skiplist_add(Skiplist *sl,sds member,double score);

// 根据成员名删除跳表中的节点
// sl: 跳表结构体指针
// member: 待删除的成员名（sds，只读不接管）
// 返回值：删除成功返回1，节点不存在/失败返回0
int skiplist_del(Skiplist *sl,sds member);

// 范围查询，按顺序截取区间内所有成员名
// sl: 跳表结构体指针
// start: 起始下标（从0开始）
// stop: 结束下标
// ★ 借用语义：返回的数组里每个元素是内部节点的 member 引用（非拷贝、不拥有），
//   数组以 NULL 结尾。调用方（zset_range）不得 sdsfree/修改这些元素，
//   需要可拥有的副本时自行 sdsdup；数组本身（malloc 出的指针数组）可 free。
sds *skiplist_range(Skiplist *sl,int start,int stop);

// 根据成员名查找节点（★现在是 O(1) 哈希直查，不再是 O(N) 遍历第 0 层）
// 返回值：成功返回节点指针，找不到返回 NULL
// member: 查询用 sds，只读不接管
SkipNode *skiplist_find(Skiplist *sl,sds member);

// 求成员在跳表中的排名（★ O(log N)，靠 span 累加，不再遍历第 0 层）
// 排名从 1 开始（第 0 层第一个是第 1 名）；找不到返回 0。Redis ZRANK 同语义。
unsigned long skiplist_get_rank(Skiplist *sl,sds member);

// 释放整个跳表所有节点及内存，防止内存泄漏
// sl: 待销毁的跳表结构体指针
void skiplist_free(Skiplist *sl);

#endif
