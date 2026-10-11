#!/usr/bin/env bash
#
# mini-redis 自动化回归测试
#
# 用法：
#   ./test.sh                 # 用默认 127.0.0.1:6379 测试
#   HOST=127.0.0.1 PORT=6390 ./test.sh   # 覆盖地址/端口
#   SKIP_BUILD=1 ./test.sh    # 跳过重新编译（默认启动前先 make）
#
# 特性：
#   * 启动服务器后用 PING 轮询等待就绪（不再用固定 sleep）
#   * 失败时打印服务器日志尾部，便于定位
#   * HOST/PORT 可用环境变量覆盖
#   * 统一的断言辅助函数，用例直观
#   * 退出码：全部通过返回 0，有失败返回 1

set -u                      # 未定义变量报错（set -e 不用，避免个别命令非零退出中断脚本）

# ============ 配置（环境变量可覆盖） ============
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-6379}"
LOG_FILE="/tmp/mini-redis-server-${PORT}-${UID:-$(id -u)}.log"   # 加 UID，避免与别的用户/进程的同名日志撞上 → 触发 Permission denied
STARTUP_TIMEOUT="${STARTUP_TIMEOUT:-5}"         # 等待服务器就绪的最长秒数

# ============ 统计 ============
PASS=0
FAIL=0

# ============ 断言辅助 ============
# 每个断言都回写统计，TOKEN 是输出颜色/前缀
GREEN="\033[0;32m"; RED="\033[0;31m"; RESET="\033[0m"

_record() {  # $1=通过与否  $2=描述  $3=额外信息(可选)
    local ok="$1" desc="$2" extra="${3:-}"
    if [ "$ok" = "1" ]; then
        PASS=$((PASS+1))
        printf "  ${GREEN}[PASS]${RESET} %s\n" "$desc"
    else
        FAIL=$((FAIL+1))
        printf "  ${RED}[FAIL]${RESET} %s%s\n" "$desc" "$([ -n "$extra" ] && printf " (%s)" "$extra")"
    fi
}

assert_eq() {  # $1=描述  $2=期望  $3=实际
    if [ "$2" = "$3" ]; then
        _record 1 "$1"
    else
        _record 0 "$1" "want='$2' got='$3'"
    fi
}

assert_contains() {  # $1=描述  $2=子串  $3=完整文本
    if printf '%s' "$3" | grep -qF "$2"; then
        _record 1 "$1"
    else
        _record 0 "$1" "missing '$2' in: '$3'"
    fi
}

assert_empty() {  # $1=描述  $2=待判值
    if [ -z "$2" ]; then
        _record 1 "$1"
    else
        _record 0 "$1" "want=<empty> got='$2'"
    fi
}

# redis-cli 封装：去掉 stderr，结果赋值
rcli() {
    redis-cli -h "$HOST" -p "$PORT" "$@"
}

# ============ 服务器生命周期 ============
start_server() {
    # 兜底：清掉上一次测试残留的 mini-redis（端口冲突会让本次 bind 直接失败）。
    # -x 精确匹配进程名（不会误伤 redis-server）；-u 限定为当前用户（非 root
    # 也只能 signal 自己的进程，加 -u 让这层意图显式，避免 silent 失败）。
    # pkill 没匹配到时返回 1，但 ; sleep 0.3 保证继续；SO_REUSEADDR 已设，
    # 这里 sleep 只是给 TIME_WAIT 一个短暂的兜底。
    pkill -x -u "${UID:-$(id -u)}" mini-redis 2>/dev/null; sleep 0.3

    if [ "${SKIP_BUILD:-0}" != "1" ]; then
        echo "==> 编译..."
        if ! make >/dev/null 2>&1; then
            echo "编译失败，中止。" >&2
            exit 2
        fi
    fi

    if [ ! -x ./mini-redis ]; then
        echo "错误：找不到可执行文件 ./mini-redis（先 make）" >&2
        exit 2
    fi

    echo "==> 启动服务器 ${HOST}:${PORT} ..."
    ./mini-redis >"$LOG_FILE" 2>&1 &
    SERVER_PID=$!
    # 兜底关停：只 kill 不 wait。wait 的退出状态可能被 bash 当作脚本最终退出码，
    # 在某些场景下把"测试失败应该是 1"变成 2（也可能在 trap 里与已 reaped 的 PID
    # 返回 127）。trap 的唯一目的是关停进程，不该影响脚本语义。
    # 末尾用 "|| true" 强制 trap 整体返回 0，避免任何意外覆盖显式 exit N。
    trap "kill $SERVER_PID 2>/dev/null || true" EXIT

    # 用 PING 轮询等待就绪（而非固定 sleep），最多等 STARTUP_TIMEOUT 秒
    local waited=0
    while ! rcli PING >/dev/null 2>&1; do
        sleep 0.1
        waited=$((waited + 1))
        if [ $waited -gt $((STARTUP_TIMEOUT * 10)) ]; then
            echo "错误：服务器 ${waited}0ms 内未就绪，退出。日志如下：" >&2
            tail -n 30 "$LOG_FILE" >&2
            exit 2
        fi
    done
    echo "==> 服务器已就绪（PID=$SERVER_PID）"
}

# ============ 裸 TCP 辅助（/dev/tcp，不依赖 nc） ============
# raw_send: 开一条裸 TCP 连接，原样发送 $1（可用 \r\n 转义），读回 1 秒内所有响应行并去掉 \r
raw_send() {
    timeout 3 bash -c '
        exec 3<>/dev/tcp/'"$HOST"'/'"$PORT"' || exit 1
        printf "%b" "$1" >&3
        out=""
        while IFS= read -r -t 1 line <&3; do out="$out$line
"; done
        printf "%s" "$out"
    ' _ "$1" 2>/dev/null | tr -d '\r'
}

# raw_test: $1=描述  $2=发送数据  $3=期望回复（已去 \r，可多行）
raw_test() {
    local desc="$1" payload="$2" expect="$3" got
    got=$(raw_send "$payload")
    assert_eq "$desc" "$expect" "$got"
}

# raw_send_split: 先发 $1，等 $3 秒，再发 $2——模拟半包（一条命令拆成两个 TCP 包到达）
raw_send_split() {
    timeout 5 bash -c '
        exec 3<>/dev/tcp/'"$HOST"'/'"$PORT"' || exit 1
        printf "%b" "$1" >&3
        sleep "$3"
        printf "%b" "$2" >&3
        out=""
        while IFS= read -r -t 1 line <&3; do out="$out$line
"; done
        printf "%s" "$out"
    ' _ "$1" "$2" "$3" 2>/dev/null | tr -d '\r'
}

# raw_test_split: $1=描述  $2=前半截  $3=后半截  $4=期望回复
raw_test_split() {
    local desc="$1" first="$2" second="$3" expect="$4"
    assert_eq "$desc" "$expect" "$(raw_send_split "$first" "$second" 0.4)"
}

# ============ 测试开始 ============
echo "=== mini-redis 自动化测试 (${HOST}:${PORT}) ==="
echo ""
start_server

# ---------- 0. 连接测试 ----------
echo "--- 0. 连接测试 ---"
assert_eq "PING" "PONG" "$(rcli PING 2>/dev/null)"
echo ""

# ---------- 1. 基本命令 ----------
echo "--- 1. 基本命令 ---"
assert_eq "SET" "OK" "$(rcli SET name zhangsan 2>/dev/null)"
assert_eq "GET" "zhangsan" "$(rcli GET name 2>/dev/null)"
assert_empty "GET miss" "$(rcli GET nokey 2>/dev/null)"
assert_eq "DEL" "1" "$(rcli DEL name 2>/dev/null)"
assert_eq "DEL again" "0" "$(rcli DEL name 2>/dev/null)"
echo ""

# ---------- 2. 错误处理 ----------
echo "--- 2. 错误处理 ---"
assert_contains "SET err (wrong number)" "wrong number" "$(rcli SET a 2>&1)"
assert_contains "GET err (wrong number)" "wrong number" "$(rcli GET 2>&1)"
assert_contains "UNKNOWN command" "unknown" "$(rcli UNKNOWN 2>&1)"
echo ""

# ---------- 3. 有序集合 ----------
echo "--- 3. 有序集合 ---"
R=$(rcli ZADD myzset 10 apple 2>/dev/null)
[ "$R" = "OK" ] || [ "$R" = "1" ] && _record 1 "ZADD apple" || _record 0 "ZADD apple" "want=OK/1 got='$R'"

R=$(rcli ZADD myzset 5 banana 2>/dev/null)
[ "$R" = "OK" ] || [ "$R" = "1" ] && _record 1 "ZADD banana" || _record 0 "ZADD banana" "want=OK/1 got='$R'"

R=$(rcli ZADD myzset 20 apple 2>/dev/null)
[ "$R" = "OK" ] || [ "$R" = "0" ] && _record 1 "ZADD update apple" || _record 0 "ZADD update apple" "want=OK/0 got='$R'"

assert_eq "ZSCORE banana" "5" "$(rcli ZSCORE myzset banana 2>/dev/null)"
assert_eq "ZSCORE apple" "20" "$(rcli ZSCORE myzset apple 2>/dev/null)"
assert_empty "ZSCORE miss" "$(rcli ZSCORE myzset cherry 2>/dev/null)"

# ---- 分数格式化回归（对齐真实 Redis d2string 的最短往返表示）----
# 回归点：曾用 "%.17g" 导致常见值出现浮点噪声（0.1 → 0.10000000000000001）。
# 期望值直接取自真实 Redis 的 ZSCORE 输出。
#
# ⚠ 注意：本项目 zset 的 key 参数当前被忽略（commands.c 里 `(void)key`），
#   所有 ZADD/ZRANGE/ZSCORE 共享同一个全局 zset（README 已列为已知限制）。
#   因此这里必须复用 myzset 这一把"全局表"，不能另开 key 名——
#   否则会污染后续 ZRANGE myzset 的次序断言。
rcli ZADD myzset 0.1  s01 >/dev/null 2>&1
assert_eq "ZSCORE 0.1 不带浮点噪声" "0.1" "$(rcli --raw ZSCORE myzset s01 2>/dev/null)"
rcli ZADD myzset 0.3  s03 >/dev/null 2>&1
assert_eq "ZSCORE 0.3 不带浮点噪声" "0.3" "$(rcli --raw ZSCORE myzset s03 2>/dev/null)"
rcli ZADD myzset 1e-7 s07 >/dev/null 2>&1
assert_eq "ZSCORE 1e-7 用指数形式" "1e-07" "$(rcli --raw ZSCORE myzset s07 2>/dev/null)"
rcli ZADD myzset 3.0  s30 >/dev/null 2>&1
assert_eq "ZSCORE 3.0 去掉多余小数" "3" "$(rcli --raw ZSCORE myzset s30 2>/dev/null)"
rcli ZADD myzset 1e10 s10 >/dev/null 2>&1
assert_eq "ZSCORE 1e10 不用指数(Redis 风格)" "10000000000" "$(rcli --raw ZSCORE myzset s10 2>/dev/null)"
rcli ZADD myzset -0   snz >/dev/null 2>&1
assert_eq "ZSCORE -0 保留负零" "-0" "$(rcli --raw ZSCORE myzset snz 2>/dev/null)"
rcli ZADD myzset inf  sinf >/dev/null 2>&1
assert_eq "ZSCORE inf 拼写固定" "inf" "$(rcli --raw ZSCORE myzset sinf 2>/dev/null)"
rcli ZADD myzset -inf sninf >/dev/null 2>&1
assert_eq "ZSCORE -inf 拼写固定" "-inf" "$(rcli --raw ZSCORE myzset sninf 2>/dev/null)"
# 关键：不得为了"好看"而丢失精度 —— 这个值必须原样保留 17 位
rcli ZADD myzset 0.30000000000000004 sprec >/dev/null 2>&1
assert_eq "ZSCORE 保留必要精度(0.1+0.2)" "0.30000000000000004" "$(rcli --raw ZSCORE myzset sprec 2>/dev/null)"
# 清掉本组新增成员，避免影响后续 ZRANGE myzset 的次序断言
for m in s01 s03 s07 s30 s10 snz sinf sninf sprec; do
    rcli ZREM myzset "$m" >/dev/null 2>&1
done

R=$(rcli ZRANGE myzset 0 -1 2>/dev/null)
{ printf '%s' "$R" | grep -q "banana" && printf '%s' "$R" | grep -q "apple"; } \
    && _record 1 "ZRANGE all" || _record 0 "ZRANGE all" "got='$R'"

assert_eq "ZRANGE 0 0" "banana" "$(rcli --raw ZRANGE myzset 0 0 2>/dev/null)"

assert_eq "ZREM banana" "1" "$(rcli ZREM myzset banana 2>/dev/null)"
assert_eq "ZREM banana again" "0" "$(rcli ZREM myzset banana 2>/dev/null)"
echo ""

# ---------- 4. 边界测试 ----------
echo "--- 4. 边界测试 ---"
R=$(rcli ZADD myzset 20 apple 2>/dev/null)
[ "$R" = "OK" ] || [ "$R" = "0" ] && _record 1 "ZADD same member same score" || _record 0 "ZADD same member same score" "got='$R'"
assert_eq "ZSCORE apple still 20" "20" "$(rcli ZSCORE myzset apple 2>/dev/null)"

rcli ZADD myzset 30 cherry  >/dev/null 2>&1
rcli ZADD myzset 40 date   >/dev/null 2>&1
R=$(rcli ZRANGE myzset -2 -1 2>/dev/null)
{ printf '%s' "$R" | grep -q "cherry" && printf '%s' "$R" | grep -q "date"; } \
    && _record 1 "ZRANGE -2 -1" || _record 0 "ZRANGE -2 -1" "got='$R'"

R=$(rcli ZRANGE myzset 10 20 2>/dev/null)
if ! printf '%s' "$R" | grep -q "apple"; then
    _record 1 "ZRANGE out of range (empty)"
else
    _record 0 "ZRANGE out of range (empty)" "got='$R'"
fi
echo ""

# ---------- 5. B3/B9 回归 + inline 命令 ----------
echo "--- 5. B3 参数上限 / B9 参数校验 / inline 命令 ---"
# 1. B3：12 参数命令（修复前直接 protocol error；上限已提至 1024）
assert_eq "12-arg PING (B3)" "PONG" "$(rcli PING a b c d e f g h i j k l 2>/dev/null)"

# 2. B3：*2000 头（超 1024 上限，应报协议错误并断开）
raw_test "*2000 header rejected" '*2000\r\n' '-ERR protocol error'

# 3. B9：ZADD 垃圾 score（修复前 atof("abc") 按 0 处理）
assert_contains "ZADD invalid score (B9)" "value is not a valid float" "$(rcli ZADD myzset abc m1 2>&1)"

# 4. B9：ZADD NaN score（修复前 NaN 破坏跳表排序）
assert_contains "ZADD NaN score (B9)" "value is not a valid float" "$(rcli ZADD myzset nan m2 2>&1)"

# 5. B9：ZRANGE 垃圾索引（修复前 atoi 不报错）
assert_contains "ZRANGE invalid index (B9)" "value is not an integer or out of range" "$(rcli ZRANGE myzset a 1 2>&1)"

# 6. inline 命令：经典 echo "PING" | nc（不走 RESP 数组协议）
raw_test "inline PING" 'PING\r\n' '+PONG'

# 7. inline SET
raw_test "inline SET" 'SET inline_k inline_v\r\n' '+OK'

# 8. inline GET（RESP 两行：长度行 + 内容行；inline_v 长 8）
raw_test "inline GET" 'GET inline_k\r\n' "$(printf '$8\ninline_v')"

# 9. inline 空行：应被跳过而不是协议错误
raw_test "inline blank line ignored" '\r\nPING\r\n' '+PONG'

# ---------- 半包（split packet）回归 ----------
# 10. 头部 *1 单独一个包，参数在第二个包
raw_test_split "split: header then args" '*1\r\n' '$4\r\nPING\r\n' '+PONG'

# 11. SET 命令拆在参数中间
raw_test_split "split: SET mid-args" '*3\r\n$3\r\nSET\r\n' '$3\r\nspk\r\n$3\r\nspv\r\n' '+OK'

# 12. bulk 内容 "spk" 被拆成 "s"+"pk"（内容跨包）
raw_test_split "split: bulk content split" '*2\r\n$3\r\nGET\r\n$3\r\ns' 'pk\r\n' "$(printf '$3\nspv')"

# 清理半包 / inline 测试的键
rcli DEL spk      >/dev/null 2>&1
rcli DEL inline_k >/dev/null 2>&1
echo ""

# ---------- 6. 二进制安全（SDS 核心能力，C 字符串版做不到） ----------
# 说明：raw_send 走 "逐行 read + 去 \r"，遇到 \0 / 多行 bulk 内容会失真，
#       所以二进制与超长值用例改用 py_roundtrip（Python 直接 socket，二进制精确读写）。
echo "--- 6. 二进制安全 ---"
py_roundtrip() {  # $1=描述  $2=python 代码(定义 send()/expect() 返回 bytes)
    local desc="$1" code="$2" rc
    local out
    out=$(HOST="$HOST" PORT="$PORT" python3 - "$code" <<'PYEOF'
import os, sys, socket
code = sys.argv[1]
HOST, PORT = os.environ['HOST'], int(os.environ['PORT'])
s = socket.create_connection((HOST, PORT), timeout=3)
ns = {}
exec(code, ns)
s.sendall(ns['send']())
s.settimeout(2)
buf = b''
try:
    while True:
        d = s.recv(65536)
        if not d: break
        buf += d
except socket.timeout:
    pass
s.close()
expect = ns['expect']()
if buf == expect:
    sys.stdout.write("OK")
else:
    sys.stdout.write("MISMATCH want_len=%d got_len=%d want_head=%r got_head=%r" %
                     (len(expect), len(buf), expect[:20], buf[:20]))
PYEOF
)
    assert_eq "$desc" "OK" "$out"
}

# 1. 值中间含 \0：SET bin1 "A\x00xy"（长度4），回读必须原样还原 4 字节
py_roundtrip "binary value SET/GET (含\\0)" \
'send=lambda: b"*3\r\n$3\r\nSET\r\n$4\r\nbin1\r\n$4\r\nA\x00xy\r\n*2\r\n$3\r\nGET\r\n$4\r\nbin1\r\n"
expect=lambda: b"+OK\r\n$4\r\nA\x00xy\r\n"'

# 2. ZADD 二进制 member（m\x00k）
py_roundtrip "binary member ZADD+ZSCORE" \
'send=lambda: b"*4\r\n$4\r\nZADD\r\n$4\r\nbinz\r\n$4\r\n42.5\r\n$3\r\nm\x00k\r\n*3\r\n$6\r\nZSCORE\r\n$4\r\nbinz\r\n$3\r\nm\x00k\r\n"
expect=lambda: b"+OK\r\n$4\r\n42.5\r\n"'

# ---------- 7. 大值（>1KB，旧版固定 1KB 缓冲区装不下，SDS 动态扩容） ----------
echo "--- 7. 大值/超 1KB 缓冲 ---"
# 2000 字节值：旧版固定 char[1024] 在 read 到 1KB 就会截断，SDS 可整段拼接还原
py_roundtrip "SET/GET 2000B value (超 1KB)" \
'send=lambda: b"*3\r\n$3\r\nSET\r\n$7\r\nbigkey1\r\n$2000\r\n"+b"x"*2000+b"\r\n*2\r\n$3\r\nGET\r\n$7\r\nbigkey1\r\n"
expect=lambda: b"+OK\r\n$2000\r\n"+b"x"*2000+b"\r\n"'

# 8192 字节值：验证动态扩容 + 多段 read 拼接
py_roundtrip "SET/GET 8192B value (动态扩容)" \
'send=lambda: b"*3\r\n$3\r\nSET\r\n$7\r\nbigkey2\r\n$8192\r\n"+b"y"*8192+b"\r\n*2\r\n$3\r\nGET\r\n$7\r\nbigkey2\r\n"
expect=lambda: b"+OK\r\n$8192\r\n"+b"y"*8192+b"\r\n"'

# 320KB 大值（接近 sdsMakeRoomFor 的 1MB 阈值的分段扩容路径）
py_roundtrip "SET/GET 320KB value (阈值扩容)" \
'send=lambda: b"*3\r\n$3\r\nSET\r\n$7\r\nbigkey3\r\n$327680\r\n"+b"z"*327680+b"\r\n*2\r\n$3\r\nGET\r\n$7\r\nbigkey3\r\n"
expect=lambda: b"+OK\r\n$327680\r\n"+b"z"*327680+b"\r\n"'

# 用 redis-cli 清理大键
rcli DEL bigkey1 >/dev/null 2>&1
rcli DEL bigkey2 >/dev/null 2>&1
rcli DEL bigkey3 >/dev/null 2>&1
echo ""

# ---------- 8. 断连 / 并发 / 优雅退出（审查修复回归） ----------
echo "--- 8. 断连 / 并发 / 优雅退出 ---"

# 8.1 SIGPIPE 回归（P0-1）：客户端发完命令立刻强制 RST 关闭，一个回复字节都不读。
#     修复前：服务器随后的 write() 会收到 SIGPIPE，默认动作是【终止整个进程】——
#             一个客户端就能把服务搞挂。修复后：只拿到 EPIPE，继续服务下一个客户端。
py_rst_probe() {
    HOST="$HOST" PORT="$PORT" python3 - <<'PYEOF'
import os, socket, struct, sys
HOST, PORT = os.environ['HOST'], int(os.environ['PORT'])
try:
    # 写一个大值，让服务器回包需要多次 write()，放大触发概率
    s = socket.create_connection((HOST, PORT), timeout=3)
    s.sendall(b"*3\r\n$3\r\nSET\r\n$7\r\nrst_key\r\n$100000\r\n" + b"z" * 100000 + b"\r\n")
    # SO_LINGER(1,0)：close() 立即发 RST，且本端不读任何回复
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
    s.close()
    # 服务器若已被 SIGPIPE 杀死，下面这次连接会直接失败
    t = socket.create_connection((HOST, PORT), timeout=3)
    t.sendall(b"*1\r\n$4\r\nPING\r\n")
    t.settimeout(2)
    data = t.recv(64)
    t.close()
    sys.stdout.write("OK" if b"PONG" in data else "NO_PONG")
except Exception as e:
    sys.stdout.write("DIED(%s)" % e.__class__.__name__)
PYEOF
}
assert_eq "客户端 RST 断连后服务器仍存活 (SIGPIPE 回归)" "OK" "$(py_rst_probe)"
rcli DEL rst_key >/dev/null 2>&1

# 8.2 并发能力（★ epoll 改造后升级为正式断言）
#     改造前是阻塞式单连接串行：第 1 条连接不关，第 2 条就得不到响应。
#     ae 事件循环改造后，多条连接应能【同时】被服务 —— 这里断言：
#       ① 第 1 条连接只建立、不发数据（故意制造"卡住读"的场景）
#       ② 第 2 条连接立刻发 PING，必须收到 PONG
#       ③ 第 3 条也建一条，验证不止能服务两条
#       ④ 通过第 2 条 SET / 第 1 条 GET 交叉，验证共享同一份存储
py_concurrency_probe() {
    HOST="$HOST" PORT="$PORT" python3 - <<'PYEOF'
import os, socket
HOST, PORT = os.environ['HOST'], int(os.environ['PORT'])

def cmd(s, *args):
    out = ("*%d\r\n" % len(args)).encode()
    for a in args:
        a = a.encode() if isinstance(a, str) else a
        out += ("$%d\r\n" % len(a)).encode() + a + b"\r\n"
    s.sendall(out)
    return s.recv(4096)

try:
    # ① 第 1 条：建立后保持打开且【不发任何数据】
    a = socket.create_connection((HOST, PORT), timeout=2)

    # ② 第 2 条：立刻发 PING，必须收到 PONG
    b = socket.create_connection((HOST, PORT), timeout=2)
    r = cmd(b, "PING")
    if b"PONG" not in r:
        print("第 2 条连接未收到 PONG（收到 %r）" % r[:40]); raise SystemExit
    # ③ 第 3 条也连上
    c = socket.create_connection((HOST, PORT), timeout=2)
    if b"PONG" not in cmd(c, "PING"):
        print("第 3 条连接未收到 PONG"); raise SystemExit
    # ④ 交叉验证存储共享：B 写，A 读
    cmd(b, "SET", "conc_key", "shared")
    r = cmd(a, "GET", "conc_key")
    if b"shared" not in r:
        print("存储未共享（A 读到 %r）" % r[:40]); raise SystemExit
    if b"PONG" not in cmd(a, "PING"):
        print("第 1 条连接已失效"); raise SystemExit
    a.close(); b.close(); c.close()
    print("OK")
except Exception as e:
    print("探测失败(%s)" % e.__class__.__name__)
PYEOF
}
assert_eq "并发能力：第 2/3 条连接可同时被服务（epoll 回归）" "OK" "$(py_concurrency_probe)"

# 8.3 定时器 / 空闲超时（阶段 4）：
#     ★ 用一个【独立端口 + CLIENT_TIMEOUT=1】的服务器实例来做，
#       因为主实例按默认配置（不超时）跑，而这一项要验证「超时确实会踢人」。
#     验证两件事：
#       a) 连上后完全不发数据的连接会被踢掉（防「只占 fd 不发数据」）
#       b) 持续活动的连接不会被误踢（超时逻辑不能误伤）
#     ⚠ 必须放在优雅退出（8.4）之前 —— 那一项会把主实例关掉。
py_idle_timeout_probe() {
    python3 - <<'PYEOF'
import os, socket, subprocess, time
host = os.environ.get('HOST', '127.0.0.1')
port = int(os.environ.get('IDLE_PORT', '6399'))

srv = subprocess.Popen(['./mini-redis', str(port)],
                       env={**os.environ, 'CLIENT_TIMEOUT': '1'},
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    # 等就绪（不用固定 sleep，避免慢机器上偶发失败）
    for _ in range(50):
        try:
            t = socket.create_connection((host, port), 0.2); t.close(); break
        except OSError:
            time.sleep(0.1)
    else:
        print("SERVER_NOT_UP"); raise SystemExit

    idle = socket.create_connection((host, port), 3)   # 连上后彻底不动
    act  = socket.create_connection((host, port), 3)   # 持续活动

    ok_active = 0
    for _ in range(5):        # 约 2 秒，远超 1 秒的超时阈值
        time.sleep(0.4)
        try:
            act.sendall(b"*1\r\n$4\r\nPING\r\n")
            if act.recv(64) == b"+PONG\r\n": ok_active += 1
        except OSError:
            break

    # 空闲连接现在应该已被服务器关闭
    idle.settimeout(1.0)
    try:
        idle.sendall(b"*1\r\n$4\r\nPING\r\n")
        idle_closed = (idle.recv(64) == b"")
    except OSError:
        idle_closed = True

    idle.close(); act.close()
    print("OK" if (idle_closed and ok_active == 5) else
          "idle_closed=%s active=%d/5" % (idle_closed, ok_active))
finally:
    srv.terminate()
    try: srv.wait(timeout=5)
    except subprocess.TimeoutExpired: srv.kill()
PYEOF
}
assert_eq "空闲超时：空闲连接被踢、活跃连接不受影响（定时器回归）" "OK" "$(py_idle_timeout_probe)"

# 8.3b 分批游标不能持有 Client*（阶段 4 修复回归）：
#      serverCron 的分批检查如果「保存上次扫描到的 Client* 当游标」，
#      那个节点可能在两次 cron 之间被 freeClient 释放 → 下一轮解引用即
#      heap-use-after-free。这个 bug 真实发生过（ASan 抓到 serverCron
#      读取已释放的 Client），修复是改成不持有任何对象地址。
#      这里用「反复建立/断开一批连接」来踩它：连接被删得越频繁，
#      游标指向已释放节点的概率越高。
#      ⚠ 这一项在普通构建下可能【侥幸通过】（释放的内存往往还能读）——
#        它真正的检出能力在 `make asan` 之后跑。但仍然值得保留：
#        崩溃/异常退出在普通构建下也会被抓到。
py_cron_uaf_probe() {
    python3 - <<'PYEOF'
import os, socket, subprocess, time
host = os.environ.get('HOST', '127.0.0.1')
port = int(os.environ.get('UAF_PORT', '6398'))

# CLIENT_TIMEOUT 用一个大值：我们不想踢连接，只想让分批扫描持续运行
srv = subprocess.Popen(['./mini-redis', str(port)],
                       env={**os.environ, 'CLIENT_TIMEOUT': '60'},
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(50):
        try:
            t = socket.create_connection((host, port), 0.2); t.close(); break
        except OSError:
            time.sleep(0.1)
    else:
        print("SERVER_NOT_UP"); raise SystemExit

    rounds = 0
    for _ in range(8):
        conns = [socket.create_connection((host, port), 3) for _ in range(8)]
        for c in conns:
            c.sendall(b"*1\r\n$4\r\nPING\r\n")
        for c in conns:
            c.recv(64)
        time.sleep(0.35)      # 期间 serverCron 会跑几轮（分批扫描进行中）
        for c in conns:
            c.close()         # 全部断开 —— 游标若存指针就会指向已释放节点
        time.sleep(0.4)
        rounds += 1

    # 服务器必须仍然存活、仍能服务
    alive = False
    try:
        s = socket.create_connection((host, port), 3)
        s.sendall(b"*1\r\n$4\r\nPING\r\n")
        alive = (s.recv(64) == b"+PONG\r\n")
        s.close()
    except OSError:
        pass

    rc = srv.poll()           # 非 None 表示服务器已经自己崩了
    print("OK" if (alive and rc is None) else
          "alive=%s exitcode=%s rounds=%d" % (alive, rc, rounds))
finally:
    srv.terminate()
    try: srv.wait(timeout=5)
    except subprocess.TimeoutExpired: srv.kill()
PYEOF
}
assert_eq "定时器分批扫描：频繁断开不导致悬垂游标（阶段4 UAF 回归）" "OK" "$(py_cron_uaf_probe)"

# 8.4 优雅退出回归（P0-2）：有客户端连接着时发 SIGINT，服务器必须能退出。
#     修复前：readQueryFromClient 的 EINTR 分支无条件 continue，
#             只要还有客户端连着，Ctrl+C 就永远退不出去（只能等客户端断开）。
#     ⚠ 这一项会终止服务器，必须放在所有需要服务器的用例之后。
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    _record 0 "有客户端连接时 SIGINT 能优雅退出" "服务器进程已不在，前置用例可能已把它搞挂"
else
    python3 - "$HOST" "$PORT" <<'PYEOF' &
import sys, socket, time
s = socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=10)
time.sleep(8)      # 保持连接不放，期间外部给服务器发 SIGINT
s.close()
PYEOF
    HOLD_PID=$!
    sleep 0.5
    kill -INT "$SERVER_PID" 2>/dev/null
    waited=0
    while kill -0 "$SERVER_PID" 2>/dev/null && [ "$waited" -lt 50 ]; do
        sleep 0.1
        waited=$((waited + 1))
    done
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        _record 0 "有客户端连接时 SIGINT 能优雅退出" "进程 $SERVER_PID 在 5s 内未退出"
        kill -9 "$SERVER_PID" 2>/dev/null
    else
        _record 1 "有客户端连接时 SIGINT 能优雅退出"
    fi
    # 保持连接的那个 python 还停在 sleep 里，直接结束掉，别让整个套件白等好几秒
    kill "$HOLD_PID" 2>/dev/null
    wait "$HOLD_PID" 2>/dev/null
fi
echo ""

# ---------- 最终结果统计 ----------
# 清理可能残留的边界测试键，避免下次运行受影响
rcli ZREM myzset cherry >/dev/null 2>&1
rcli ZREM myzset date   >/dev/null 2>&1

TOTAL=$((PASS+FAIL))
echo "=== 结果: $PASS/$TOTAL 通过 ==="
if [ "$FAIL" -eq 0 ]; then
    echo "全部通过! ✅"
    exit 0
else
    echo "存在失败 ❌"
    echo ">>> 服务器日志末尾（$LOG_FILE）："
    tail -n 20 "$LOG_FILE"
    exit 1
fi
