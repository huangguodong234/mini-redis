#!/usr/bin/env python3
"""mini-redis 基准采集：多流水线深度 / 多命令 / 多负载下的 QPS，输出 CSV。
用法: python3 bench_collect.py <binary> <out.csv> [tag]
"""
import socket, time, sys, subprocess, random, os

BIN, OUT, TAG = sys.argv[1], sys.argv[2], (sys.argv[3] if len(sys.argv) > 3 else BIN)
PORT = 6500
PIPELINE_DEPTHS = [1, 10, 50, 200, 500, 1000]
REQS = 40000

def start():
    p = subprocess.Popen([BIN, str(PORT)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(120):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=0.3); s.close(); return p
        except OSError: time.sleep(0.05)
    p.kill(); raise RuntimeError("not ready")

def recv_exact(s, n):
    buf = b''
    s.settimeout(15)
    while len(buf) < n:
        d = s.recv(1 << 20)
        if not d: break
        buf += d
    return buf

def tput(cmd_payload, resp_bytes_each, n, pipe, keygen=None):
    """send n commands pipelined by pipe depth; return qps."""
    batch = cmd_payload * pipe
    expect_each = len(resp_bytes_each)
    # warmup
    s = socket.create_connection(("127.0.0.1", PORT), timeout=15)
    s.sendall(batch); recv_exact(s, expect_each * pipe)
    s.close()
    batches = n // pipe
    t0 = time.perf_counter()
    s = socket.create_connection(("127.0.0.1", PORT), timeout=30)
    for _ in range(batches):
        s.sendall(batch)
        recv_exact(s, expect_each * pipe)
    s.close()
    dt = time.perf_counter() - t0
    return n / dt

def bench_ping(pipe, n):
    cmd = b"*1\r\n$4\r\nPING\r\n"
    return tput(cmd, b"+PONG\r\n", n, pipe)

def bench_set(pipe, n, vlen):
    v = b"v" * vlen
    vh = b"*3\r\n$3\r\nSET\r\n$4\r\nkey\r\n$%d\r\n" % len(v)
    cmd = vh + v + b"\r\n"
    return tput(cmd, b"+OK\r\n", n, pipe)

def bench_get(pipe, n):
    cmd = b"*2\r\n$3\r\nGET\r\n$4\r\nkey\r\n"
    # 先 set 一个值，保证 GET 命中
    s = socket.create_connection(("127.0.0.1", PORT), timeout=15)
    s.sendall(b"*3\r\n$3\r\nSET\r\n$4\r\nkey\r\n$8\r\nsomevalue\r\n")
    s.settimeout(5); s.recv(100); s.close()
    return tput(cmd, b"$8\r\nsomevalue\r\n", n, pipe)

def main():
    srv = start()
    rows = []
    try:
        for pipe in PIPELINE_DEPTHS:
            q = bench_ping(pipe, REQS)
            rows.append((TAG, "PING", pipe, "%.0f" % q))
            print(f"{TAG} PING  pipe={pipe:4d}  qps={q:8.0f}")
        for pipe in PIPELINE_DEPTHS:
            q = bench_set(pipe, REQS, 5)
            rows.append((TAG, "SET", pipe, "%.0f" % q))
            print(f"{TAG} SET   pipe={pipe:4d}  qps={q:8.0f}")
        for pipe in PIPELINE_DEPTHS:
            q = bench_get(pipe, REQS)
            rows.append((TAG, "GET", pipe, "%.0f" % q))
            print(f"{TAG} GET   pipe={pipe:4d}  qps={q:8.0f}")
        # 大值 SET
        for vlen in [1024, 8192, 65536]:
            sql = bench_set(200, 1000, vlen)
            rows.append((TAG, "BIGSET", vlen, "%.0f" % sql))
            print(f"{TAG} BIGSET vlen={vlen:6d}  qps={sql:8.0f}")
    finally:
        srv.kill()
    with open(OUT, "w") as f:
        f.write("version,cmd,param,qps\n")
        for r in rows:
            f.write(",".join(r) + "\n")
    print("-> wrote", OUT)

if __name__ == "__main__":
    main()
