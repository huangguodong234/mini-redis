#!/bin/bash
HOST="127.0.0.1"
PORT=6379
PASS=0
FAIL=0

echo "=== mini-redis 自动化测试 ==="
echo ""

# 启动服务器
./server > /tmp/server.log 2>&1 &
SERVER_PID=$!
sleep 0.5
trap "kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null" EXIT

# ---------- 基本命令 ----------
echo "--- 1. 基本命令 ---"

# SET
R=$(redis-cli -h $HOST -p $PORT SET name zhangsan 2>/dev/null)
[ "$R" = "ok" ] && { echo "  [PASS] SET"; PASS=$((PASS+1)); } || { echo "  [FAIL] SET (got: '$R')"; FAIL=$((FAIL+1)); }

# GET
R=$(redis-cli -h $HOST -p $PORT GET name 2>/dev/null)
[ "$R" = "zhangsan" ] && { echo "  [PASS] GET"; PASS=$((PASS+1)); } || { echo "  [FAIL] GET (got: '$R')"; FAIL=$((FAIL+1)); }

# GET miss
R=$(redis-cli -h $HOST -p $PORT GET nokey 2>/dev/null)
[ -z "$R" ] && { echo "  [PASS] GET miss"; PASS=$((PASS+1)); } || { echo "  [FAIL] GET miss (got: '$R')"; FAIL=$((FAIL+1)); }

# DEL
R=$(redis-cli -h $HOST -p $PORT DEL name 2>/dev/null)
[ "$R" = "1" ] && { echo "  [PASS] DEL"; PASS=$((PASS+1)); } || { echo "  [FAIL] DEL (got: '$R')"; FAIL=$((FAIL+1)); }

# DEL again
R=$(redis-cli -h $HOST -p $PORT DEL name 2>/dev/null)
[ "$R" = "0" ] && { echo "  [PASS] DEL again"; PASS=$((PASS+1)); } || { echo "  [FAIL] DEL again (got: '$R')"; FAIL=$((FAIL+1)); }

echo ""

# ---------- 错误处理 ----------
echo "--- 2. 错误处理 ---"

# SET err
R=$(redis-cli -h $HOST -p $PORT SET a 2>/dev/null)
echo "$R" | grep -q "wrong number" && { echo "  [PASS] SET err"; PASS=$((PASS+1)); } || { echo "  [FAIL] SET err (got: '$R')"; FAIL=$((FAIL+1)); }

# GET err
R=$(redis-cli -h $HOST -p $PORT GET 2>/dev/null)
echo "$R" | grep -q "wrong number" && { echo "  [PASS] GET err"; PASS=$((PASS+1)); } || { echo "  [FAIL] GET err (got: '$R')"; FAIL=$((FAIL+1)); }

# UNKNOWN
R=$(redis-cli -h $HOST -p $PORT UNKNOWN 2>/dev/null)
echo "$R" | grep -qi "unknown" && { echo "  [PASS] UNKNOWN"; PASS=$((PASS+1)); } || { echo "  [FAIL] UNKNOWN (got: '$R')"; FAIL=$((FAIL+1)); }

echo ""
TOTAL=$((PASS+FAIL))
echo "=== 结果: $PASS/$TOTAL 通过 ==="
if [ $FAIL -eq 0 ]; then
    echo "全部通过! ✅"
    exit 0
else
    echo "存在失败 ❌"
    exit 1
fi
