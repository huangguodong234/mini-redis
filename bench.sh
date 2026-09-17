#!/usr/bin/env bash
#
# bench.sh —— mini-redis 压力测试脚本
#
# 用法:
#   ./bench.sh                       # 压测当前 ./mini-redis (sds 版)
#   BIN=/path/to/mini-redis ./bench.sh     # 压测指定二进制
#   PORT=6390 ./bench.sh             # 覆盖端口
#
# 功能:
#   * 用 redis-benchmark --csv 做多命令 / 多并发 / 多数据量压测
#   * 纯本地回环, 避免网络噪声
#   * 结果写入 CSV (final_bench.csv) 便于后续画图
#
# 注意: 服务器是"阻塞单线程"模型, accept→read→parse→handle→write 串行,
#       所以并发提升有限, 基准是"单连接+流水线"下的吞吐。

set -u
BIN="${BIN:-./mini-redis}"
PORT="${PORT:-6390}"
LOG="/tmp/bench-$PORT.log"
OUT="bench_out.csv"
rm -f "$OUT"

echo "==> 压测二进制: $BIN"
[ -x "$BIN" ] || { echo "找不到可执行文件 $BIN"; exit 2; }

"$BIN" "$PORT" >"$LOG" 2>&1 &
SRV=$!
trap "kill -9 $SRV 2>/dev/null" EXIT

# 等待就绪
for i in $(seq 1 50); do
  redis-cli -p "$PORT" PING >/dev/null 2>&1 && break
  sleep 0.1
done

run() {  # $1=描述  $2...=redis-benchmark 参数
  local desc="$1"; shift
  echo -n "" 
  # redis-benchmark --csv 输出一行: "test,throughput,latency"
  local line
  line=$(redis-benchmark -p "$PORT" --csv "$@" 2>/dev/null | tr -d '\r')
  # 只取第一行有效结果(test 可能是 PING_INLINE 等)
  local row
  row=$(printf '%s\n' "$line" | head -1)
  printf '%s,%s\n' "$desc" "$row" >> "$OUT"
  echo "  [$desc] -> $row"
}

echo "==> 开始压测（单连接 + 流水线）"
run "ping_inline_c1"  -t ping -c 1 -n 300000 -q
run "set_c1"          -t set  -c 1 -n 100000 -q
run "get_c1"          -t get  -c 1 -n 100000 -q
run "set_c10"         -t set  -c 10 -n 100000 -q
run "get_c10"         -t get  -c 10 -n 100000 -q
run "set_1KB"         -t set  -c 1 -n 20000  -d 1024 -q
run "set_8KB"         -t set  -c 1 -n 5000   -d 8192 -q

echo "==> 结果已写入 $OUT"
echo "---"; cat "$OUT"
kill -9 $SRV 2>/dev/null
trap - EXIT
