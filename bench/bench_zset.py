#!/usr/bin/env python3
"""跳表 zset 专项压测 —— 反映 span + O(1) find 改造后的真实吞吐。

用法: python3 bench/bench_zset.py <binary> <port> [members]

测量（分批流水线 + 批尾 PING 屏障，QPS，取 5 次中位数）：
  * zadd_fresh  : 持续插入全新成员（表增长）—— O(1) 查重 + O(log n) 插入
  * zadd_update : 对已存在成员反复 ZADD 更新分数 —— O(1) find + O(log n) 插入
  * zscore      : 对已灌满的大表做 ZSCORE 点查 —— O(1)
  * zrange      : 对已灌满的大表做 ZRANGE 0 99 —— O(log n) span 跳转 + 输出 100 个

服务器单连接串行处理。每个命令批（≤PIPE 条）末尾跟一个 PING，服务端按序
回复；客户端「sendall 该批 → 读到该批的 +PONG」，即可精确判定整批处理完，
同时小批发送不会因发送缓冲填满而死锁。跨 recv 边界的 PONG 由累积 buf 保证
能被识别。
"""
import socket, sys, time, statistics, subprocess, random

BIN  = sys.argv[1]
PORT = int(sys.argv[2])
N    = int(sys.argv[3]) if len(sys.argv) > 3 else 200_000
PIPE = 2000
REQS = 20_000          # 每项测 2 万次（成员池随机，样本足够稳定）
WARM_W = 50_000

PING  = b"*1\r\n$4\r\nPING\r\n"
PONG  = b"+PONG\r\n"

def bulk(s_bytes):
    return b"$" + str(len(s_bytes)).encode() + b"\r\n" + s_bytes + b"\r\n"

def zadd_cmd(score, m):
    sb, b = str(score).encode(), m.encode()
    return b"*4\r\n" + bulk(b"ZADD") + bulk(b"k") + bulk(sb) + bulk(b)
def zscore_cmd(m):
    b = m.encode()
    return b"*3\r\n" + bulk(b"ZSCORE") + bulk(b"k") + bulk(b)
def zrange_cmd(a, b2):
    return b"*4\r\n" + bulk(b"ZRANGE") + bulk(b"k") + bulk(str(a).encode()) + \
           bulk(str(b2).encode())

def main():
    sub = subprocess.Popen([BIN, str(PORT)], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    s = None
    try:
        for _ in range(300):
            try:
                s = socket.create_connection(("127.0.0.1", PORT), timeout=0.3); break
            except OSError:
                time.sleep(0.03)
        if s is None: raise RuntimeError("server not ready")
        s.settimeout(120)

        # 把命令列表切成每 PIPE 条一批，每批末尾补 1 个 PING 作屏障
        def make_batches(cmdlist):
            batches = []
            for s0 in range(0, len(cmdlist), PIPE):
                seg = cmdlist[s0:s0 + PIPE]
                # 批尾只加 1 个 PING，服务端回复顺序固定为该批回应 + PONG
                batches.append((b"".join(seg) + PING, len(seg)))
            return batches

        # 发一个批，读到该批的 +PONG 即整批处理完（累积 buf 保证跨边界 PONG 也不漏）
        def round_trip(blob):
            s.sendall(blob)
            buf = b""
            while PONG not in buf:
                d = s.recv(1 << 20)
                if not d: raise RuntimeError("connection closed mid-batch")
                buf += d

        def ping_probe():
            s.sendall(PING); s.recv(len(PONG))

        for _ in range(3): ping_probe()

        # ---- 预热：灌满 N 个成员 ----
        print(f"预热灌入 {N} 个成员 ...", flush=True)
        t0 = time.perf_counter()
        for base in range(0, N, WARM_W):
            chunk = [zadd_cmd(1.0, f"m{j}") for j in range(base, min(N, base + WARM_W))]
            for blob, _ in make_batches(chunk):
                round_trip(blob)
        print(f"  预热耗时 {time.perf_counter()-t0:.1f}s, size={N}", flush=True)

        rng = random.Random(12345)
        pool = [f"m{rng.randrange(N)}" for _ in range(REQS)]

        def bench(tag, cmdlist):
            batches = make_batches(cmdlist)
            qs = []
            for _ in range(5):
                t0 = time.perf_counter()
                for blob, _ in batches:
                    round_trip(blob)
                dt = time.perf_counter() - t0
                qs.append(len(cmdlist) / dt)
            q = statistics.median(qs)
            print(f"{tag:12s} : {q:9,.0f} QPS", flush=True)
            return q

        q_score = bench("zscore",      [zscore_cmd(pool[i]) for i in range(REQS)])
        q_range = bench("zrange0-99",  [zrange_cmd(0, 99)   for _ in range(REQS)])
        q_upd   = bench("zadd_update", [zadd_cmd(i, pool[i]) for i in range(REQS)])
        q_fresh = bench("zadd_fresh",  [zadd_cmd(1.0, f"x{i}") for i in range(REQS)])

        print(f"\n== 结果 (members={N}, pipeline={PIPE}, 5次中位数) ==")
        print(f"zscore      : {q_score:,.0f} QPS   <- O(1) 哈希直查")
        print(f"zrange0-99  : {q_range:,.0f} QPS   <- O(log n) span 跳转 + 输出100")
        print(f"zadd_update : {q_upd:,.0f} QPS   <- O(1) find + O(log n) 插入")
        print(f"zadd_fresh  : {q_fresh:,.0f} QPS   <- O(log n) 插入（表增长）")

    finally:
        if s: s.close()
        if sub:
            try: sub.kill()
            except Exception: pass

if __name__ == "__main__":
    main()
