#!/usr/bin/env python3
"""聚焦基准：在固定流水线深度下测 SET / GET / 大值SET 的 QPS。稳健、用于 sds vs no-sds 对比。
用法: python3 bench_focus.py <binary> <out.csv> [tag]
"""
import socket, time, sys, subprocess, random

BIN, OUT, TAG = sys.argv[1], sys.argv[2], (sys.argv[3] if len(sys.argv) > 3 else BIN)
PORT = 6700 + (__import__("os").getpid() % 100)
PIPE = 200
REQS = 30000

def start():
    subprocess.run(["pkill", "-f", f"mini-redis {PORT}"], capture_output=True)
    time.sleep(0.2)
    p = subprocess.Popen([BIN, str(PORT)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(120):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=0.3); s.close(); return p
        except OSError: time.sleep(0.05)
    p.kill(); raise RuntimeError("not ready")

def rx_n(s, n):
    buf = b''; s.settimeout(20)
    while len(buf) < n:
        d = s.recv(1 << 20)
        if not d: break
        buf += d
    return buf

def run(payload, resp_len, n, pipe):
    batch = payload * pipe
    # warmup on a throwaway connection
    s = socket.create_connection(("127.0.0.1", PORT), timeout=20)
    s.sendall(batch); rx_n(s, resp_len * pipe)
    s.close()
    time.sleep(0.1)
    s = socket.create_connection(("127.0.0.1", PORT), timeout=30)
    # 预先把 GET 需要的 key 或数据准备好（如果 GET 需要）
    got = 0
    t0 = time.perf_counter()
    while got < n:
        b = batch if (n - got) >= pipe else (payload * (n - got))
        send_cnt = pipe if (n - got) >= pipe else (n - got)
        s.sendall(b)
        rx_n(s, resp_len * send_cnt)
        got += send_cnt
    dt = time.perf_counter() - t0
    s.close()
    return got / dt

def bench_set(pipe, v):
    cmd = b"*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$%d\r\n" % len(v) + v + b"\r\n"
    return run(cmd, len(b"+OK\r\n"), REQS, pipe)

def bench_get(pipe):
    # 先在服务器上 set 一个固定值
    s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
    s.sendall(b"*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$9\r\nsomevalue\r\n")
    s.settimeout(5); s.recv(100); s.close()
    cmd = b"*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n"
    return run(cmd, len(b"$9\r\nsomevalue\r\n"), REQS, pipe)

def main():
    srv = None
    rows = []
    def ensure_srv():
        nonlocal srv
        if srv is not None:
            try: srv.kill()
            except Exception: pass
        srv = start()
    def safe_bench(tag, fn, param, label):
        ensure_srv()
        try:
            q = fn(); r = "%.0f"%q
            print(f"{TAG} {label} qps={q:.0f}")
        except Exception as e:
            r = "CRASH"; print(f"{TAG} {label} -> CRASH ({e.__class__.__name__})")
        rows.append((TAG, tag, str(param), r))
    def set_small(): return bench_set(PIPE, b"v"*5)
    try:
        safe_bench("SET", set_small, PIPE, f"SET pipe={PIPE}")
        safe_bench("GET", lambda: bench_get(PIPE), PIPE, f"GET pipe={PIPE}")
        for vlen in [1024, 65536]:
            safe_bench("BIGSET", (lambda v=vlen: bench_set(PIPE, b"v"*v)), vlen, f"BIGSET {vlen}B")
    finally:
        if srv is not None:
            try: srv.kill()
            except Exception: pass
    with open(OUT,"w") as f:
        f.write("version,cmd,param,qps\n")
        for r in rows: f.write(",".join(r)+"\n")
    print("-> wrote", OUT)

if __name__ == "__main__":
    main()
