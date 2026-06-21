#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "skiplist.h"

// ==================== 随机层数生成 ====================
// 返回 1 到 SKIPLIST_MAX_LEVEL 之间的随机整数
// 算法：从 1 开始，每次以 50% 概率加一层
static int random_level(void){
    int level =1;
    // rand() 返回 0 到 RAND_MAX，如果小于 RAND_MAX/2 就加一层
    //rand() & 1  数字是奇数, 层数 +1,为偶数，退出循环
    while((rand() & 1) && level <SKIPLIST_MAX_LEVEL){
        level++;
    }
    return level;
}

// ==================== 创建跳表 ====================
Skiplist *skiplist_create(void){
    Skiplist *sl =malloc(sizeof(Skiplist));
    sl->size=0;
    sl->max_level =1;    // 初始只有第0层

    // 创建头节点：不存数据，拥有最大层数
    sl->header =malloc(sizeof(SkipNode));
    sl->header->member =NULL;
    sl->header->score = 0.0;
    sl->header->level =SKIPLIST_MAX_LEVEL;

    // 分配头节点的 forward 数组（SKIPLIST_MAX_LEVEL 个指针）
    sl->header->forward=malloc(sizeof(SkipNode *) *SKIPLIST_MAX_LEVEL);
    for(int i=0;i<SKIPLIST_MAX_LEVEL;i++){
        sl->header->forward[i]=NULL;  // 初始全指向 NULL
    }
    return sl;
}

// ==================== 查找节点 ==================== 现在是用循环的方法来查找，O(n),待优化
// 根据 member 查找节点，返回节点指针，找不到返回 NULL
// 对：返回节点指针 SkipNode*
SkipNode *skiplist_find(Skiplist *sl, const char *member) {
    if(!sl || !member) return NULL;
    // 在第0层顺序查找（按 member 精确匹配）
    SkipNode *curr=sl->header->forward[0];
    while(curr){
        if(strcmp(curr->member,member)==0){
            return curr;
        }
        curr=curr->forward[0];
    }
    return NULL;
}

// ==================== 插入/更新 ====================
void skiplist_add(Skiplist *sl,const char*member,double score){
    //先检查member是否存在，再记录前驱指针
    //防止删除节点时删掉的节点刚好是前驱指针
    //避免插入时前驱指针指向已删除的内存

    // 1. 检查 member 是否已存在（用 skiplist_find 按 member 查找）
    SkipNode *existing=skiplist_find(sl,member);
    if(existing){
        if(existing->score==score) return;
        else{
            skiplist_del(sl,member); //删除旧节点（重新排序）
        }
    }

    // update[i] 记录在第 i 层，新节点应该插在谁后面
    SkipNode *update[SKIPLIST_MAX_LEVEL];
    SkipNode *curr =sl->header;

    // 2. 从最高层往下查找，记录每层的前驱节点
    for(int i=sl->max_level -1;i>=0;i--){          // 修复：i-- 而不是 i++
        while(curr ->forward[i] && curr->forward[i]->score <score){
            curr=curr->forward[i];
        }
        update[i]=curr;  // 该层的前驱指针
    }
   
    // 3. member 不存在（或被删除了），创建新节点
    int new_level =random_level();
    // 如果新节点的层数超过当前最大层，更新 max_level
    if(new_level >sl->max_level){
        for(int i=sl->max_level;i<new_level;i++){
            update[i]=sl->header;// 高层的前驱就是头节点
        }
        sl->max_level=new_level;
    }

    // 创建新节点
    SkipNode *new_node =malloc(sizeof(SkipNode));
    new_node ->member=strdup(member);
    new_node ->score=score;
    new_node ->level=new_level;
    new_node ->forward=malloc(sizeof(SkipNode *)*new_level);

    // 4. 更新各层指针（链表插入操作）
    for(int i=0;i<new_level;i++){ 
        new_node->forward[i]=update[i]->forward[i];  // 新节点的下一个
        update[i]->forward[i]=new_node;              // 前驱指向新节点
    }
    sl->size++;
}

// ==================== 删除节点 ====================
// 根据 member 删除跳表中的节点
// 成功返回 1，节点不存在返回 0
int skiplist_del(Skiplist *sl, const char *member) {
    if(!sl || !member) return 0;

    // 先查找目标节点，获取它的 score
    SkipNode *target =skiplist_find(sl,member);
    if(!target) return 0;  // 不存在，直接返回
    double score =target->score;

    SkipNode *update[SKIPLIST_MAX_LEVEL];
    SkipNode *curr=sl->header;

    // 1. 从最高层向下，记录每层的前驱节点
    for(int i=sl->max_level-1;i>=0;i--){
        // 前进条件：下一个节点存在，
        //并且 (score更小) 或 (score相同但member字符串更小)
        while(curr->forward[i] && (curr->forward[i]->score < score || 
            (curr->forward[i]->score==0 && 
            strcmp(curr->forward[i]->member ,member)<0))){
                curr=curr->forward[i];
        }
        update[i]=curr; // 记录该层前驱
    }

    // 2. 定位到第0层的目标节点（现在 update[0]->forward[0] 应该就是 target）
    curr=curr->forward[0];
    if(curr !=target){
        return 0;       // 理论上不会发生，防御性检查
    } 

    // 3. 从各层链表中摘除该节点
    for(int i=0;i<sl->max_level;i++){
        if(update[i]->forward[i]==curr){
            update[i]->forward[i]=curr->forward[i];
        }
    }

    // 4. 释放节点内存
    free(curr->member);
    free(curr->forward);
    free(curr);
    sl->size--;

    // 5. 调整 max_level（如果高层的前驱指针全变成 NULL 了）
    while(sl->max_level>1 && sl->header->forward[sl->max_level-1]==NULL){
        sl->max_level--;
    }
    return 1;
}

// ==================== 范围查询 ====================
// 按排名从 start 到 stop 返回 member 字符串数组
// start 和 stop 是索引（从0开始），stop 可以为 -1 表示最后一个
// 返回的数组以 NULL 结尾，调用者需要释放数组中每个字符串以及数组本身
//有更快的方法，在每个节点额外存储 span 信息，但我们先简化
char **skiplist_range(Skiplist *sl, int start, int stop) {
    if(!sl){
        char **result=malloc(sizeof(char*));
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
        char **result=malloc(sizeof(char *));
        result[0]=NULL;
        return result;
    }

    //修正 stop 超出上界
    if (stop >= sl->size) stop = sl->size - 1;

    int count=stop-start+1;
    char **result=malloc(sizeof(char*)*(count +1)); // +1 放 NULL

    // 从头节点第0层走到第一个
    SkipNode *curr=sl->header->forward[0];
    for(int i=0;i<start && curr;i++){
        curr=curr->forward[0];
    }

    // 收集 count 个元素
    for(int i=0 ;i<count && curr;i++){
        result[i]=strdup(curr->member);
        curr=curr->forward[0];
    }
    result[count]=NULL;
    return result;
}

// ==================== 释放跳表 ====================
void skiplist_free(Skiplist *sl){
    if(! sl) return;
    SkipNode *curr=sl->header->forward[0];
    // 1. 释放所有业务节点
    while(curr){
        SkipNode *next=curr->forward[0];
        free(curr->member);
        free(curr->forward);
        free(curr);
        curr=next;
    }
    // 2. 释放头节点
    free(sl->header->member);
    free(sl->header->forward);
    free(sl->header);

    // 3. 释放跳表主体结构
    free(sl);
}
