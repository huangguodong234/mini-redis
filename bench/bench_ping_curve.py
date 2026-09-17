#!/usr/bin/env python3
"""PING 流水线深度曲线：在不同管道深度下测 QPS。用法: bench_ping_curve.py <binary> <out.csv> [tag]"""
import socket, struct, time, sys, subprocess, os

BIN, OUT, TAG = sys.argv[1], sys.argv[2], (sys.argv[3] if len(sys.argv) > 3 else BIN)
PORT = 6400 + (os.getpid() % 100)
DEPTHS = [1, 10, 50, 200, 500, 1000]
REQS = 40000

def start():
    subprocess.run(["pkill", "-f", f"mini-redis {PORT}"], capture_output=True)
    time.sleep(0.2)
    p = subprocess.Popen([BIN, str(PORT)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(120):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=0.3); s.close(); return p
        except OSError: time.sleep(0.05)
    p.kill(); raise RuntimeError("not ready")

def recv_exact(s, n):
    buf = b''; s.settimeout(20)
    while len(buf) < n:
        d = s.recv(1 << 20)
        if not d: break
        buf += d
    return buf

def ping_qps(pipe, n):
    cmd = b"*1\r\n$4\r\nPING\r\n"
    batch = cmd * pipe
    resp = b"+PONG\r\n"
    s = socket.create_connection(("127.0.0.1", PORT), timeout=20)
    s.sendall(batch); recv_exact(s, len(resp) * pipe); s.close()
    time.sleep(0.1)
    s = socket.create_connection(("127.0.0.1", PORT), timeout=30)
    got = 0; t0 = time.perf_counter()
    while got < n:
        k = pipe if (n - got) >= pipe else (n - got)
        s.sendall(cmd * k)
        recv_exact(s, len(resp) * k)
        got += k
    dt = time.perf_counter() - t0
    s.close()
    return got / dt

def main():
    srv = start()
    rows = []
    try:
        for pipe in DEPTHS:
            try:
                q = ping_qps(pipe, REQS)
                rows.append((TAG, "PING", pipe, "%.0f" % q))
                print(f"{TAG} PING pipe={pipe:4d} qps={q:8.0f}")
            except Exception as e:
                rows.append((TAG, "PING", pipe, "CRASH"))
                print(f"{TAG} PING pipe={pipe:4d} -> CRASH ({e.__class__.__name__})")
    finally:
        try: srv.kill()
        except Exception: pass
    with open(OUT, "w") as f:
        f.write("version,cmd,param,qps\n")
        for r in rows: f.write(",".join(str(x) for x in r) + "\n")
    print("-> wrote", OUT)

if __name__ == "__main__":
    main()
