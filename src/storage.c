#include <stdio.h>
#include <stdlib.h>
#include "storage.h"
#include "sds.h"


// ============================================================
// storage.c —— mini-redis 存储引擎
// ============================================================
// ★ 存储边界：接口直接收 sds（server/commands 层传进来的就是 sds，
//   二进制安全）。
//
//   生命周期约定：
//   - set：走"所有权转移"（storage_set_steal），本层不深拷贝，直接把
//           k/v 的 sds 所有权移交给哈希表（由 hashtable_free /
//           hashtable_del / duplicate-key 覆盖释放），调用方须把对应
//           argv[i] 置 NULL，本层此后不得再 sdsfree 它们。
//           ※ 置 NULL 后 server.c 的无脑释放循环会因 sdsfree 内部的
//             `if(!s) return;` 判空而安全跳过该槽，故 server.c 释放循环
//             无需额外判空；重点是"每次 steal 必须成对置 NULL"这条硬契约。
//   - get：快照拷贝路径，返回可拥有的深拷贝，调用方负责 sdsfree。
//   - del：查询用 sds 是调用方传入的临时量，本层只读不接管、不释放
//          （调用方自己负责）。

// 初始化存储引擎
Storage *storage_init() {
    Storage *s=malloc(sizeof(Storage));
    if(!s){
        fprintf(stderr, "storage_init: malloc Storage 失败\n");
        return NULL;
    }
    // 创建通用哈希表，挂上 sdsDictType（拥有 key/value，销毁即 sdsfree）。
    // 容量、负载因子、扩容都在 dict 内部自管（初始 16 桶）。
    s->ht = dictCreate(&sdsDictType, NULL);
    if(!s->ht){
        fprintf(stderr, "storage_init: dictCreate 失败\n");
        free(s);
        return NULL;
    }
    return s;
}

// SET 命令的“所有权转移”路径（argv 不释放给哈希）
// ★ 唯一的 set 路径：直接接管调用方解析出的 key/value sds 的所有权移交给
//   哈希表（不深拷贝；省掉每 SET 两次 sdsdup 的 malloc+memcpy，也省掉
//   server 层后续对这两个元素的两次 sdsfree）。
//   ⚠ 调用方（commands 层）必须把自己手里对应的 argv[i] 置 NULL，
//   避免 server.c 误 free（哈希表现在拥有它们）；置 NULL 后 server.c 的
//   无脑释放循环因 sdsfree 内部 `if(!s) return;` 判空而安全跳过该槽，
//   【无需】再给 server.c 加判空；哈希表在 duplicate-key / OOM 拒绝路径
//   自行释放传入的 sds，故不泄漏。
int storage_set_steal(Storage *s, sds key, sds value) {
    // 无法入库的路径也要就地归还，保证"无论成败所有权都已转移"这条契约成立，
    // 调用方（commands 层）才能无条件把 argv 槽置 NULL 而不泄漏。
    if(!s || !key || !value){
        sdsfree(key);
        sdsfree(value);
        return 0;
    }
    // ★ 所有权直接移交，不再拷贝。
    //   这里的语义与原 hashtable_set 完全一致，只是换了实现：
    //     - 成功：dict 持有这两个 sds，最终在 dictDelete/dictRelease 时
    //       走 sdsDictType 的 destructor（sdsfree）释放；
    //     - key 已存在：dictAdd 会把新 value 覆盖进去并返回 1，
    //       旧 value 不再被引用，故需在此处主动释放（见下方补丁）；
    //     - OOM：dictAdd 返回 0，且【不会替我们释放】key/value
    //       （通用表不知道它们归谁），所以必须在本层就地归还。
    //
    // ★ 一个已有 key 的更新路径：先探一下旧 value。
    //   为什么不能让 dict 自己去释放旧 value：因为通用表不知道
    //   旧 value 归谁（storage 的需要 sdsfree，跳表索引的只是个 SkipNode*
    //   不该动）——这正是“销毁权已外包”的代价：负责人变成调用方。
    //   代价很小：一次 dictFind（O(1)），而且这个分支本来就只在“重复 SET”时走。
    void *old = dictFind(s->ht, key);
    if (old) sdsfree((sds)old);          // 释放旧 value（旧 key 就是新 key，不用动）

    if (!dictAdd(s->ht, key, value)) {
        // 失败：本次没写入，就地归还 key/value，
        // 保持“无论成败所有权都已转移”这条硬契约成立。
        sdsfree(key);
        sdsfree(value);
        return 0;
    }
    return 1;
}

// GET 命令的实现（key 只读不接管）
// ★ 快照拷贝路径：底层 hashtable_get 返回哈希表内部 value 的活引用（所有权归
//   哈希表），本层不把活引用直接交给调用方，而是 sdsdup 深拷贝一份"可拥有"的
//   快照返回。这样调用方拿到的是与哈希表完全无关的独立副本，可自由 sdsfree，
//   即使随后有 SET/DEL 改动哈希表该键也不影响它——返回值自给自足，不依赖
//   "发送窗口内表不被改"这个时序前提。
//   ⚠ 前提：hashtable_get 期间哈希表必须稳定（本层内部一次查完即拷贝）。
//   ⚠ 调用方负责 sdsfree 返回值（NULL 表示键不存在，无需释放）。
sds storage_get(Storage *s, sds key) {
    if(!s || !key) return NULL;
    char *internal = (char *)dictFind(s->ht, key);
    if(!internal) return NULL;
    return sdsdup((sds)internal);  // 返回可拥有的深拷贝
}

// DEL 命令的实现（key 只读不接管）
int storage_del(Storage *s, sds key) {
    if(!s || !key) return 0;
    // ★ 这里就是“通用”最关键的一处体现：同一个 dictDelete，
    //   在 storage 这张表（sdsDictType）上会真的 sdsfree 掉 key/value；
    //   在跳表索引那张表（skipIndexDictType）上则什么都不做。
    return dictDelete(s->ht, key);
}

// 释放所有内存
void storage_free(Storage *s) {
    if(!s) return;
    dictRelease(s->ht);       // 释放哈希表内部所有节点（按 sdsDictType 释放 sds 键值）
    free(s);                  // 释放 Storage 结构体
}
