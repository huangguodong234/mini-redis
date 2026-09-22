#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "skiplist.h"

// ============================================================================
// 一、内部哈希索引（member → SkipNode，O(1) 查找，承担 Redis dict 辅助表角色）
//     key / node 都是【借用引用】：key 指向所属 SkipNode 的 member，node 指向
//     所属 SkipNode。索引本身不持有、不释放这两者，生命周期完全由跳表管理，
//     因此 skiplist_del / skiplist_free 必须「先摘索引条目，再释放节点」。
// ============================================================================

#define SKIPLIST_INDEX_INIT_CAP 16
#define SKIPLIST_INDEX_LOAD_FACTOR 0.75

// djb2 哈希，按 sdslen 逐字节，二进制安全
static unsigned long index_hash_sds(const sds str){
    unsigned long hash =5381;
    size_t len =sdslen(str);
    for(size_t i=0;i<len;i++){
        hash =((hash<<5)+hash)+(unsigned char)str[i];
    }
    return hash;
}

// 扩容并重哈希（失败则维持原样继续，正确性不变）
static void index_resize(Skiplist *sl){
    int new_cap=sl->index_cap *2;
    IndexNode **nb =calloc((size_t)new_cap,sizeof(IndexNode *));
    if(!nb) return;   // OOM：维持原桶数（链变长而已）

    for(int i=0;i<sl->index_cap;i++){
        IndexNode *cur =sl->buckets[i];
        while(cur){
            IndexNode *next=cur->next;
            int idx = (int)(index_hash_sds(cur->key) % (unsigned long)new_cap);
            cur->next =nb[idx];
            nb[idx]=cur;
            cur=next;
        }
    }
    free(sl->buckets);
    sl->buckets=nb;
    sl->index_cap=new_cap;
}

// 插入/更新索引：key 借用 node->member，node 借用节点指针
static void index_insert(Skiplist *sl, sds key, SkipNode *node){
    int idx =(int)(index_hash_sds(key) % (unsigned long)sl->index_cap);
    for(IndexNode *cur=sl->buckets[idx];cur;cur=cur->next){
        if(sdscmp(cur->key,key)==0){
            cur->node =node;   // 已存在 → 更新指向
            return;
        }
    }
    // 不存在，头插
    IndexNode *n =malloc(sizeof(IndexNode));
    if(!n) return;   // OOM 时索引缺失:find 退化为找不到，跳表正确性不受影响
    n->key=key;
    n->node=node;
    n->next=sl->buckets[idx];
    sl->buckets[idx]=n;
    sl->index_size++;

    if((double)sl->index_size / sl->index_cap >= SKIPLIST_INDEX_LOAD_FACTOR){
        index_resize(sl);
    }
}

// 删除索引条目；只释放 IndexNode，不释放 key（key 是借用引用，归节点所有）
// 返回 1 删到；0 不存在
static int index_remove(Skiplist *sl, sds key){
    int idx =(int)(index_hash_sds(key) % (unsigned long)sl->index_cap);
    IndexNode *cur =sl->buckets[idx];
    IndexNode *prev =NULL;
    while(cur){
        if(sdscmp(cur->key,key)==0){
            if(prev) prev->next=cur->next;
            else sl->buckets[idx]=cur->next;
            free(cur);          // 只释放桶节点，key 借用不释放
            sl->index_size--;
            return 1;
        }
        prev=cur;
        cur=cur->next;
    }
    return 0;
}

// 按 key 直查节点；找不到返回 NULL（O(1) 平均）
static SkipNode *index_get(Skiplist *sl, sds key){
    if(!sl->buckets) return NULL;
    int idx =(int)(index_hash_sds(key) % (unsigned long)sl->index_cap);
    for(IndexNode *cur=sl->buckets[idx];cur;cur=cur->next){
        if(sdscmp(cur->key,key)==0) return cur->node;
    }
    return NULL;
}

// 释放索引全部桶节点（key 借用，不释放）
static void index_free(Skiplist *sl){
    if(!sl->buckets) return;
    for(int i=0;i<sl->index_cap;i++){
        IndexNode *cur=sl->buckets[i];
        while(cur){
            IndexNode *next=cur->next;
            free(cur);
            cur=next;
        }
    }
    free(sl->buckets);
    sl->buckets=NULL;
    sl->index_size=0;
}

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

    // ★ 初始化 member→node 哈希索引
    sl->buckets =calloc((size_t)SKIPLIST_INDEX_INIT_CAP,sizeof(IndexNode *));
    if(!sl->buckets){
        fprintf(stderr, "skiplist_create: malloc buckets 失败\n");
        free(sl->header->lvl);
        free(sl->header);
        free(sl);
        return NULL;
    }
    sl->index_size=0;
    sl->index_cap=SKIPLIST_INDEX_INIT_CAP;
    return sl;
}

// ============================================================================
// 四、查找节点 —— ★ O(1) 哈希直查（原为 O(N) 遍历第 0 层）
// ============================================================================
SkipNode *skiplist_find(Skiplist *sl, sds member) {
    if(!sl || !member) return NULL;
    return index_get(sl, member);
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

    // ★ 维护 member→node 哈希索引
    index_insert(sl, new_node->member, new_node);
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
    index_remove(sl, member);

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
    SkipNode *nd =index_get(sl, member);
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
    // 先释放索引（key 借用，只 free 桶节点），避免之后读到已释放节点的 member
    index_free(sl);

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
