// ============================================================
// test_hashtable.c —— 哈希表单元测试
// 测试场景：
//   1. 插入并查询
//   2. 更新已存在的 key
//   3. 删除存在的 key
//   4. 删除不存在的 key
// ============================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hashtable.h"

int main()
{
    printf("=== 哈希表单元测试 ===\n");

    // 创建一个容量为 4 的小哈希表（故意很小，便于测试链表行为）
    HashTable *ht =hashtable_creat(4);

    // ---------- 测试 1：插入并查询 ----------
    printf("\n[测试1] 插入 3 个 key\n");
    hashtable_set(ht ,"name","alice");
    hashtable_set(ht,"age","25");
    hashtable_set(ht,"city","beijing");

    printf("  GET name  = %s (期望 Alice)\n", hashtable_get(ht, "name"));
    printf("  GET age   = %s (期望 25)\n", hashtable_get(ht, "age"));
    printf("  GET city  = %s (期望 Beijing)\n", hashtable_get(ht, "city"));
    printf("  GET notexist = %s (期望 (null))\n", hashtable_get(ht, "notexist"));

    // ---------- 测试 2：更新已存在的 key ----------
    printf("\n[测试2] 更新 age 为 30\n");
    hashtable_set(ht,"age","30");
    printf("  GET age   = %s (期望 30)\n", hashtable_get(ht, "age"));

    // ---------- 测试 3：删除存在的 key ----------
    printf("\n[测试3] 删除 city\n");
    int ret=hashtable_del(ht,"city");
    printf("  删除 city 返回值 = %d (期望 1)\n", ret);   
    printf("  GET city  = %s (期望 (null))\n", hashtable_get(ht, "city"));

    // ---------- 测试 4：删除不存在的 key ----------
    printf("\n[测试4] 再次删除 city\n");
    ret=hashtable_del(ht,"city");
    printf("删除 city 返回值 = %d (期望 0)\n",ret);

    // ---------- 清理 ----------
    hashtable_free(ht);
    printf("\n=== 所有测试完成 ===\n");
    return 0;
}
