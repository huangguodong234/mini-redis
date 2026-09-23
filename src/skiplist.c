#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "skiplist.h"

// ============================================================================
// 一、索引的 dictType：只借用，不拥有（★ 消重的核心）
// ============================================================================
// 这里曾经是一整块私有哈希表（IndexNode / buckets / index_* 约 100 行）。
// 现在整块删除，换成下面这张函数表 —— 因为通用 dict 已经
// 把“怎么算哈希 / 怎么比较 / 怎么销毁”外包给了调用方。
//
// 跳表对这三件事的答案：哈希/比较用 sds 那套（member 就是 sds），
// 销毁【什么都不做】—— key 和 value 都是借用引用：
//   key   指向 SkipNode->member（归 SkipNode 所有）
//   value 就是 SkipNode*（节点生命周期由跳表管）
// 两者都不归 dict 所有——这正是「先摘索引条目，再释放节点」这条硬约束
// 的来历：dictDelete 不会去碰 member，所以在节点释放前调它是安全的。
//
// ★ 这里【没有一行代码去判断“这是跳表的表”——身份就是这张表本身：
//   dictCreate 把它存进 d->type，之后每次 dictDelete/dictRelease 自动走到这里。

// ★ 为什么显式写空操作函数而不直接传 NULL：dictFreeEntry 已会判空，
//   传 NULL 也能跑；但空函数让“这是借用、故意不释放”在代码里看得见。
//   （对照：sdsDictType 那边传的是 dictSdsDestructor，真释放。）
static void skipIndexKeyDestructor(void *privdata, void *key)
{
    (void)privdata;
    (void)key;   // 借用：member 归 SkipNode 所有，索引不释放它
}

static void skipIndexValDestructor(void *privdata, void *val)
{
    (void)privdata;
    (void)val;   // 借用：SkipNode 归跳表所有，索引不释放它
}

// 跳表索引的操作手册：与 sdsDictType 同样的 hash/compare（member 也是
// sds，直接复用，这就是 dict.h 把它们导出和而非 static 的原因），
// 但 destructor 换成空操作。
//
// ★ 注意这里【没有一行代码去“判断这是跳表的表”——
//   身份就是这张表本身：dictCreate 把它存进 d->type，之后每次
//   dictDelete/dictRelease 自动走到这里。这就是把“分支”数据化成
//   “建表时选了哪份函数表”。
static dictType skipIndexDictType = {
    dictSdsHash,              // hashFunction   : 复用 sds 那套（djb2，二进制安全）
    dictSdsKeyCompare,        // keyCompare     : 复用 sdscmp（相等返回 0）
    NULL,                     // keyFromU64     : 不用（sds key）
    NULL,                     // keyDup         : NULL = 不拷贝（member 归 SkipNode）
    NULL,                     // valDup         : NULL = 不拷贝（指针直接存）
    skipIndexKeyDestructor,   // keyDestructor  : ★ 借用 → 空操作
    skipIndexValDestructor,   // valDestructor  : ★ 借用 → 空操作
};

// ============================================================================
// 二、随机层数生成
// ============================================================================
static int random_level(void){
    int level =1;
    while((rand() & 1) && level <SKIPLIST_MAX_LEVEL){
        level++;
    }
    return level;
}

// ============================================================================
// 三、创建跳表
// ============================================================================
Skiplist *skiplist_create(void){
    Skiplist *sl =malloc(sizeof(Skiplist));
    if(!sl){
        fprintf(stderr, "skiplist_create: malloc Skiplist 失败\n");
        return NULL;
    }
    sl->size=0;
    sl->max_level =1;    // 初始只有第0层

    // 创建头节点：不存数据，拥有最大层数
    sl->header =malloc(sizeof(SkipNode));
    if(!sl->header){
        fprintf(stderr, "skiplist_create: malloc header 失败\n");
        free(sl);
        return NULL;
    }
    sl->header->member =NULL;
    sl->header->score = 0.0;
    sl->header->level =SKIPLIST_MAX_LEVEL;

    // 分配头节点的 lvl 数组（SKIPLIST_MAX_LEVEL 个 {forward, span}）
    sl->header->lvl=malloc(sizeof(SkipListLevel) *SKIPLIST_MAX_LEVEL);
    if(!sl->header->lvl){
        fprintf(stderr, "skiplist_create: malloc header lvl 失败\n");
        free(sl->header);
        free(sl);
        return NULL;
    }
    for(int i=0;i<SKIPLIST_MAX_LEVEL;i++){
        sl->header->lvl[i].forward=NULL;
        sl->header->lvl[i].span=0;
    }

    // ★ member→node 索引：一张通用 dict + 一份「借用语义」的 dictType。
    //   容量/负载因子/扩容全部交给 dict 自己管，跳表不再算桶数。
    sl->index = dictCreate(&skipIndexDictType, NULL);
    if(!sl->index){
        fprintf(stderr, "skiplist_create: dictCreate 失败\n");
        free(sl->header->lvl);
        free(sl->header);
        free(sl);
        return NULL;
    }
    return sl;
}

// ============================================================================
// 四、查找节点 —— ★ O(1) 哈希直查（原为 O(N) 遍历第 0 层）
// ============================================================================
SkipNode *skiplist_find(Skiplist *sl, sds member) {
    if(!sl || !member) return NULL;
    // ★ 一行。value 存的就是 SkipNode*，这里的强制转换是本项目唯一需要写的
    //   —— void* 只能说“我不知道它是什么”，只有调用方知道。
    return (SkipNode *)dictFind(sl->index, member);
}

// ============================================================================
// 五、插入/更新
//    与旧版最大差别：
//      * 维护每层 span（含 header），使「按排名找 / 求排名」能 O(log N) 完成
//      * 同步维护 member→node 哈希索引
// ============================================================================
// 所有权约定：跳表不拷贝 member，直接接管调用方移交的 sds（已在 zset 层
// sdsdup 深拷贝过）。若本次不存储该 sds（同分提前返回 / 分配失败），
// 会在本函数内 sdsfree 归还，保证不泄漏。
int skiplist_add(Skiplist *sl,sds member,double score){
    if(!sl || !member){
        sdsfree(member);
        return 0;
    }

    // 先检查member是否存在（O(1)）
    SkipNode *existing=skiplist_find(sl,member);
    if(existing){
        if(existing->score==score){
            sdsfree(member);      // 同分不存储：归还所有权
            return 1;             // 同分无变化不算失败
        }
        else{
            skiplist_del(sl,member); //删除旧节点（重新排序），同时摘索引
        }
    }

    SkipNode *update[SKIPLIST_MAX_LEVEL];
    unsigned long rank[SKIPLIST_MAX_LEVEL];   // update[i] 当前排名（相对，1-based）
    SkipNode *curr =sl->header;

    // 从最高层往下查找，记录每层前驱与排名
    for(int i=sl->max_level-1;i>=0;i--){
        // rank[i] 起步沿用上一层已累计的排名（相同前缀）
        rank[i] = (i == sl->max_level-1) ? 0 : rank[i+1];
        while(curr->lvl[i].forward && (curr->lvl[i].forward->score < score ||
            (curr->lvl[i].forward->score==score &&
            sdscmp(curr->lvl[i].forward->member ,member)<0))){
                rank[i] += curr->lvl[i].span;   // 每跳一次，累加该层 span
                curr=curr->lvl[i].forward;
        }
        update[i]=curr; // 记录该层前驱
    }

    // 创建新节点，层数随机
    int new_level =random_level();
    if(new_level >sl->max_level){
        // 超出当前最高层：新层的前驱是 header，rank 为 0，
        // 其 span 应覆盖「当前全部元素」（新节点会插到 header 之后，占第 1 位）
        for(int i=sl->max_level;i<new_level;i++){
            rank[i]=0;
            update[i]=sl->header;
            update[i]->lvl[i].span=sl->size;
        }
        sl->max_level=new_level;
    }

    SkipNode *new_node =malloc(sizeof(SkipNode));
    if(!new_node){
        fprintf(stderr, "skiplist_add: malloc SkipNode 失败\n");
        sdsfree(member);
        return 0;
    }
    new_node ->member=member;   
    new_node ->score=score;
    new_node ->level=new_level;
    new_node ->lvl=malloc(sizeof(SkipListLevel) *new_level);
    if(!new_node->lvl){
        fprintf(stderr, "skiplist_add: malloc lvl 失败\n");
        sdsfree(new_node->member);
        free(new_node);
        return 0;
    }

    // 链接到各层，并修正 span（Redis 同款算法）
    for(int i=0;i<new_level;i++){
        new_node->lvl[i].forward = update[i]->lvl[i].forward;
        update[i]->lvl[i].forward =new_node;

        // 新节点插在 update[i] 的「第 (rank[0]-rank[i]) 个后继位置」之前
        new_node->lvl[i].span = update[i]->lvl[i].span - (rank[0] - rank[i]);
        update[i]->lvl[i].span = (rank[0] - rank[i]) + 1;
    }
    // 不在新节点层数里的上层：前驱的 span 也要 +1（因为下面多了一个节点）
    for(int i=new_level;i<sl->max_level;i++){
        update[i]->lvl[i].span++;
    }
    sl->size++;

    // ★ 维护 member→node 索引：直接把 SkipNode* 当 value 存进去，
    //   零额外分配（对比旧方案若硬塞进 sds 则需 sdsnewlen 打包指针）。
    //   失败不影响正确性：dictFind 对缺失的键返回 NULL，
    //   等价于旧的 index_insert 在 OOM 时静默跳过（只是 find 会查不到）。
    (void)dictAdd(sl->index, new_node->member, new_node);
    return 1;
}

// ============================================================================
// 六、删除节点
// ============================================================================
int skiplist_del(Skiplist *sl, sds member) {
    if(!sl || !member) return 0;

    SkipNode *target =skiplist_find(sl,member);   // O(1)
    if(!target) return 0;  // 不存在
    double score=target->score;

    SkipNode *update[SKIPLIST_MAX_LEVEL];
    SkipNode *curr=sl->header;

    // 从最高层向下，记录每层的前驱节点
    for(int i=sl->max_level-1;i>=0;i--){
        while(curr->lvl[i].forward && (curr->lvl[i].forward->score < score ||
            (curr->lvl[i].forward->score==score &&
            sdscmp(curr->lvl[i].forward->member ,member)<0))){
                curr=curr->lvl[i].forward;
        }
        update[i]=curr;
    }

    curr=curr->lvl[0].forward;
    if(curr !=target){
        return 0;       // 防御性检查
    }

    // ★ 摘除索引条目（在回收节点前，key 还是借用 target 的 member）
    //   顺序是硬要求：dictDelete 要调 hashFunction/keyCompare，
    //   两者都会解引用 member；若先释放节点就是读悬垂指针。
    //   （skipIndexDictType 的 destructor 是空操作，所以这里只会释放
    //    dictEntry 本身，不会去动 member 或 SkipNode。）
    (void)dictDelete(sl->index, member);

    // 从各层链表中摘除该节点，并修正 span（Redis 同款算法）。
    // ⚠ 必须遍历【所有】层（i<sl->max_level），而不仅是节点自身的层数：
    //   节点不在的高层里，其前驱的 span 也要 -1（因为整条第 0 层少了一个节点）。
    //   若只走到 curr->level，高层前驱的 span 会多算 1 → 排名/范围查询全偏。
    for(int i=0;i<sl->max_level;i++){
        if(update[i]->lvl[i].forward==curr){
            update[i]->lvl[i].span += curr->lvl[i].span - 1;
            update[i]->lvl[i].forward=curr->lvl[i].forward;
        }else{
            update[i]->lvl[i].span -= 1;
        }
    }

    // 释放节点内存
    sdsfree(curr->member);
    free(curr->lvl);
    free(curr);
    sl->size--;

    // 调整 max_level
    while(sl->max_level>1 && sl->header->lvl[sl->max_level-1].forward==NULL){
        sl->max_level--;
    }
    return 1;
}

// ============================================================================
// 七、按排名取节点（1-based rank）—— ★ O(log N)，供范围查询跳转起点用
//     借助 span，从 header 自上而下跳转，不用遍历第 0 层。
// ============================================================================
static SkipNode *skiplist_node_at_rank(Skiplist *sl, unsigned long rank){
    if(!sl || rank<1 || rank > (unsigned long)sl->size) return NULL;
    SkipNode *x=sl->header;
    unsigned long traversed=0;
    for(int i=sl->max_level-1;i>=0;i--){
        while(x->lvl[i].forward && (traversed + x->lvl[i].span) <= rank){
            traversed += x->lvl[i].span;
            x=x->lvl[i].forward;
        }
        if(traversed==rank) return x;
    }
    return NULL;
}

// 求 member 在跳表中的排名（1-based，链表第 0 层第一个是第 1 名），
// 找不到返回 0。★ O(log N)：顺着 span 累加即可，不用遍历第 0 层。
unsigned long skiplist_get_rank(Skiplist *sl, sds member){
    if(!sl || !member) return 0;
    // 先用 O(1) 索引拿到节点（取 score 用于排序比较），再沿 span 求排名
    SkipNode *nd =(SkipNode *)dictFind(sl->index, member);
    if(!nd) return 0;
    double score=nd->score;

    SkipNode *x=sl->header;
    unsigned long rank=0;
    for(int i=sl->max_level-1;i>=0;i--){
        while(x->lvl[i].forward &&
            (x->lvl[i].forward->score < score ||
             (x->lvl[i].forward->score==score &&
              sdscmp(x->lvl[i].forward->member, member) <= 0))){
            rank += x->lvl[i].span;
            x = x->lvl[i].forward;
        }
        // x 可能是 header，需判 member 非空；到达目标成员即返回累计排名
        if(x->member && sdscmp(x->member,member)==0){
            return rank;
        }
    }
    return 0;   // 找不到
}

// ============================================================================
// 八、范围查询 —— ★ 用 span 直接跳到 start 排名，O(log N) 起步
// ============================================================================
sds *skiplist_range(Skiplist *sl, int start, int stop) {
    if(!sl){
        sds *result=malloc(sizeof(sds));
        if(!result) return NULL;
        result[0]=NULL;
        return result;
    }

    // 将负数索引转换为正数（Redis 风格）
    if(start < 0) start=sl->size + start;
    if(stop < 0) stop=sl->size + stop;

    // 边界修正
    if(start < 0) start = 0;
    if(stop < 0) stop =0;

    // 无效范围
    if(start >stop || start >=sl->size){
        sds *result=malloc(sizeof(sds));
        if(!result) return NULL;
        result[0]=NULL;
        return result;
    }
    if (stop >= sl->size) stop = sl->size - 1;

    int count=stop-start+1;
    sds *result=malloc(sizeof(sds)*(count +1));
    if(!result) return NULL;

    // ★ 旧版：for(i=0;i<start;i++) curr=curr->forward[0]（O(N)）
    //    新版：用 span 直接跳到第 start+1 名（1-based），O(log N)
    SkipNode *curr = skiplist_node_at_rank(sl, (unsigned long)start + 1);
    int collected=0;
    for(int i=0;i<count && curr;i++){
        result[i]=curr->member;   // 借用内部节点的 member
        collected++;
        curr=curr->lvl[0].forward;
    }
    for(int i=collected;i<count;i++) result[i]=NULL;
    result[count]=NULL;
    return result;
}

// ============================================================================
// 九、释放跳表
// ============================================================================
void skiplist_free(Skiplist *sl){
    if(!sl) return;
    // 先释放索引（key/value 都是借用，只 free dictEntry 与桶数组），
    // 避免之后读到已释放节点的 member。
    // ★ dictRelease 走的是 skipIndexDictType：destructor 是空操作，
    //   所以它只释放索引自己的内存，碰都不碰跳表节点。
    dictRelease(sl->index);

    SkipNode *curr=sl->header->lvl[0].forward;
    while(curr){
        SkipNode *next=curr->lvl[0].forward;
        sdsfree(curr->member);
        free(curr->lvl);
        free(curr);
        curr=next;
    }
    sdsfree(sl->header->member);
    free(sl->header->lvl);
    free(sl->header);
    free(sl);
}
