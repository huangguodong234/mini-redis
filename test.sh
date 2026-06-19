#!/bin/bash                 
# 告诉系统：用 bash 执行这个脚本
HOST="127.0.0.1"            # 服务器地址：本机
PORT=6379                   # 端口：Redis 默认端口 6379
PASS=0                      # 测试通过的数量，初始 0
FAIL=0                      # 测试失败的数量，初始 0

echo "=== mini-redis 自动化测试 ==="  # 屏幕打印标题
echo ""                               # 打印空行，更美观

# 启动服务器
./server > /tmp/server.log 2>&1 &
# ./server            运行你写的 Redis 服务
# > /tmp/server.log   把日志输出到文件，不占屏幕
# 2>&1                把错误信息也一起输出到文件
# &                   后台运行（不卡住脚本继续往下走）

SERVER_PID=$!           # 记住刚才启动的服务进程号（方便后面关掉）
sleep 0.5               # 等待 0.5 秒，让服务完全启动

trap "kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null" EXIT
# trap ... EXIT
# 意思：脚本不管是正常结束、还是崩溃、还是按 Ctrl+C
# 都会自动执行后面的命令：杀掉服务进程
# 作用：**保证测试完一定关掉服务，不残留进程**

# ---------- 基本命令 ----------
echo "--- 1. 基本命令 ---"  # 打印：第一组测试开始

# ==================== SET 测试 ====================
R=$(redis-cli -h $HOST -p $PORT SET name zhangsan 2>/dev/null)
# R=$(...)           把命令执行结果存到变量 R 里
# redis-cli         官方 Redis 客户端工具
# -h $HOST          连接本机
# -p $PORT          连接 6379 端口
# SET name zhangsan 发送存储命令
# 2>/dev/null       屏蔽错误输出（只看结果）

[ "$R" = "ok" ] && { echo "  [PASS] SET"; PASS=$((PASS+1)); } || { echo "  [FAIL] SET (got: '$R')"; FAIL=$((FAIL+1)); }
# 这一行是简写的 判断 + 输出 + 计数
# 如果 R 等于 "OK" → 打印 PASS，通过数+1
# 否则 → 打印 FAIL，失败数+1

# ==================== GET 测试 ====================
R=$(redis-cli -h $HOST -p $PORT GET name 2>/dev/null)
[ "$R" = "zhangsan" ] && { echo "  [PASS] GET"; PASS=$((PASS+1)); } || { echo "  [FAIL] GET (got: '$R')"; FAIL=$((FAIL+1)); }
# 测试：获取刚才存的 name，期望得到 zhangsan

# ==================== GET 不存在的 key ====================
R=$(redis-cli -h $HOST -p $PORT GET nokey 2>/dev/null)
[ -z "$R" ] && { echo "  [PASS] GET miss"; PASS=$((PASS+1)); } || { echo "  [FAIL] GET miss (got: '$R')"; FAIL=$((FAIL+1)); }
# [ -z "$R" ] 意思是：R 为空字符串
# 期望：获取不存在的 key → 返回空

# ==================== DEL 删除 ====================
R=$(redis-cli -h $HOST -p $PORT DEL name 2>/dev/null)
[ "$R" = "1" ] && { echo "  [PASS] DEL"; PASS=$((PASS+1)); } || { echo "  [FAIL] DEL (got: '$R')"; FAIL=$((FAIL+1)); }
# 删除成功 → 返回 1

# ==================== DEL 再次删除 ====================
R=$(redis-cli -h $HOST -p $PORT DEL name 2>/dev/null)
[ "$R" = "0" ] && { echo "  [PASS] DEL again"; PASS=$((PASS+1)); } || { echo "  [FAIL] DEL again (got: '$R')"; FAIL=$((FAIL+1)); }
# 删一个已经不存在的 key → 返回 0

echo ""  # 空行

# ---------- 错误处理 ----------
echo "--- 2. 错误处理 ---"

# ==================== SET 参数错误 ====================
R=$(redis-cli -h $HOST -p $PORT SET a 2>/dev/null)
echo "$R" | grep -q "wrong number" && { echo "  [PASS] SET err"; PASS=$((PASS+1)); } || { echo "  [FAIL] SET err (got: '$R')"; FAIL=$((FAIL+1)); }
# 故意少传参数：SET a
# 期望返回错误包含 wrong number
# grep -q "xxx"  检查字符串里是否包含 xxx

# ==================== GET 参数错误 ====================
R=$(redis-cli -h $HOST -p $PORT GET 2>/dev/null)
echo "$R" | grep -q "wrong number" && { echo "  [PASS] GET err"; PASS=$((PASS+1)); } || { echo "  [FAIL] GET err (got: '$R')"; FAIL=$((FAIL+1)); }
# 只写 GET，不写 key → 期望报错

# ==================== 未知命令 ====================
R=$(redis-cli -h $HOST -p $PORT UNKNOWN 2>/dev/null)
echo "$R" | grep -qi "unknown" && { echo "  [PASS] UNKNOWN"; PASS=$((PASS+1)); } || { echo "  [FAIL] UNKNOWN (got: '$R')"; FAIL=$((FAIL+1)); }
# 发送一个不存在的命令 UNKNOWN
# 期望报错包含 unknown

# ---------- 有序集合 ----------
echo "--- 3. 有序集合 (ZADD / ZRANGE) ---"

# ==================== ZADD 第一个元素 ====================
R=$(redis-cli -h $HOST -p $PORT ZADD myzset 10 apple 2>/dev/null)
if [ "$R" = "OK" ] || [ "$R" = "1" ]; then
    echo "  [PASS] ZADD apple"; PASS=$((PASS+1))
else
    echo "  [FAIL] ZADD apple (got: '$R')"; FAIL=$((FAIL+1))
fi

# ==================== ZADD 第二个元素 ====================
R=$(redis-cli -h $HOST -p $PORT ZADD myzset 5 banana 2>/dev/null)
if [ "$R" = "OK" ] || [ "$R" = "1" ]; then
    echo "  [PASS] ZADD banana"; PASS=$((PASS+1))
else
    echo "  [FAIL] ZADD banana (got: '$R')"; FAIL=$((FAIL+1))
fi

# ==================== ZRANGE 查询全部 ====================
R=$(redis-cli -h $HOST -p $PORT ZRANGE myzset 0 -1 2>/dev/null)
if echo "$R" | grep -q "banana" && echo "$R" | grep -q "apple"; then
    echo "  [PASS] ZRANGE all"; PASS=$((PASS+1))
else
    echo "  [FAIL] ZRANGE all (got: '$R')"; FAIL=$((FAIL+1))
fi

# ==================== ZRANGE 索引范围 ====================
# 只查第一个元素（score最小的），期望只有 banana，不包含 apple
R=$(redis-cli -h $HOST -p $PORT ZRANGE myzset 0 0 2>/dev/null)
if echo "$R" | grep -q "banana" && ! echo "$R" | grep -q "apple"; then
    echo "  [PASS] ZRANGE 0 0"; PASS=$((PASS+1))
else
    echo "  [FAIL] ZRANGE 0 0 (got: '$R')"; FAIL=$((FAIL+1))
fi

# ==================== ZREM 删除 ====================
R=$(redis-cli -h $HOST -p $PORT ZREM myzset banana 2>/dev/null)
if [ "$R" = "1" ]; then
    echo "  [PASS] ZREM banana"; PASS=$((PASS+1))
else
    echo "  [FAIL] ZREM banana (got: '$R')"; FAIL=$((FAIL+1))
fi

# ==================== ZREM 删除不存在的 ====================
R=$(redis-cli -h $HOST -p $PORT ZREM myzset banana 2>/dev/null)
if [ "$R" = "0" ]; then
    echo "  [PASS] ZREM banana again"; PASS=$((PASS+1))
else
    echo "  [FAIL] ZREM banana again (got: '$R')"; FAIL=$((FAIL+1))
fi

echo ""

# ==================== 最终结果统计 ====================
TOTAL=$((PASS+FAIL))            # 总用例数 = 通过 + 失败
echo "=== 结果: $PASS/$TOTAL 通过 ==="  # 打印成绩

if [ $FAIL -eq 0 ]; then        # 如果失败数为 0
    echo "全部通过! ✅"
    exit 0                      # 脚本返回成功
else
    echo "存在失败 ❌"
    exit 1                      # 脚本返回失败
fi
