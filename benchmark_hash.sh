#!/bin/bash
# ==========================================
# mini-redis 哈希表 O(1) 性能测试 (redis-cli)
# ==========================================

PORT=6379
HOST="127.0.0.1"

echo "=========================================="
echo "  哈希表 O(1) 性能测试"
echo "=========================================="
echo ""

echo "=== 1. 插入 10000 个 key（SET） ==="
time for i in $(seq 1 10000); do
    redis-cli -h $HOST -p $PORT SET key_$(printf "%04d" $i) value > /dev/null
done
echo ""

echo "=== 2. 随机查询 10000 次（GET） ==="
time for i in $(seq 1 10000); do
    key_id=$(( RANDOM % 10000 + 1 ))
    redis-cli -h $HOST -p $PORT GET key_$(printf "%04d" $key_id) > /dev/null
done
echo ""

echo "=== 测试完成 ==="
echo "记录 real 时间，与动态数组版对比。"
EOF

