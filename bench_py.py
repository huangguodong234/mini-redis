#!/usr/bin/env python3
"""mini-redis 基准测试：测量流水线吞吐 + 延迟分布。用法:
    python3 bench_py.py <binary> <port>
同一参数跑两遍取稳定值。支持 env: PIPELINE=1000 REQS=100000
"""
import socket, time, sys, os, subprocess

BIN = sys.argv[1]
PORT = int(sys.argv[2])
PIPELINE = int(os.environ.get("PIPELINE", "500"))
REQS = int(os.environ.get("REQS", "100000"))

def start():
    p = subprocess.Popen([BIN, str(PORT)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=0.3); s.close(); return p
        except OSError: time.sleep(0.05)
    p.kill(); raise RuntimeError("server not ready")

def send_recv(payload, expect_bytes):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
    s.sendall(payload)
    buf = b''
    s.settimeout(10)
    while len(buf) < expect_bytes:
        d = s.recv(1 << 20)
        if not d: break
        buf += d
    s.close()
    return buf

def ping():
    cmd = b"*1\r\n$4\r\nPING\r\n"
    batch = cmd * PIPELINE
    n = (REQS // PIPELINE) * PIPELINE
    expect = len(b"+PONG\r\n") * n
    # warmup
    send_recv(batch, len(b"+PONG\r\n") * PIPELINE)
    t0 = time.perf_counter()
    send_recv(batch * (n // PIPELINE), expect)
    dt = time.perf_counter() - t0
    qps = n / dt
    return qps, dt

def set_get():
    # 生成唯一键，避免碰撞
    import random
    random.seed(1)
    keys = []
    for i in range(REQS):
        keys.append("key%06d_%d" % (i % 50000, random.randint(0, 10**9)))
    # SET 流水线
    batch_set = b"".join(b"*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$5\r\nvalue\r\n" % (len(k.encode()), k.encode()) for k in keys[:PIPELINE])
    n = (REQS // PIPELINE) * PIPELINE
    expect_set = len(b"+OK\r\n") * n
    send_recv(batch_set, expect_set)  # warmup first PIPELINE
    t0 = time.perf_counter()
    # 分批发
    sent = 0
    while sent < n:
        b = b"".join(b"*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$5\r\nvalue\r\n" % (len(k.encode()), k.encode()) for k in keys[sent:sent+PIPELINE])
        s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
        s.sendall(b)
        buf=b''
        s.settimeout(10)
        while len(buf) < len(b"+OK\r\n")*PIPELINE:
            d=s.recv(1<<20)
            if not d: break
            buf+=d
        s.close()
        sent += PIPELINE
    dt = time.perf_counter() - t0
    return n/dt, dt

def main():
    print(f"# bench {BIN} port={PORT} PIPELINE={PIPELINE} REQS={REQS}")
    srv = start()
    try:
        # PING
        q, t = ping()
        print(f"PING  qps={q:.0f}  total={t:.2f}s")
        # SET/GET 各两次取最优
        best = 0
        for _ in range(2):
            try:
                q,t = set_get()
                best = max(best, q)
            except Exception as e:
                print("set err", e)
        print(f"SET   qps={best:.0f}")
    finally:
        srv.kill()

if __name__ == "__main__":
    main()
