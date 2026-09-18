#!/usr/bin/env python3
"""大数据吞吐基准：多档位大值下 SET / GET 的流水线 QPS（sds vs 无sds）。

用法: python3 bench_throughput.py <binary> <out.csv> [tag]

对每个 value 大小档位（512B ~ 128KB）在固定流水线深度下测量 SET 与 GET 的
吞吐（QPS / 每秒请求数），并把吞吐折算成「值载荷吞吐」（MB/s = QPS × 值大小），
以便跨档位统一比较“一秒钟能读/写多少数据”（对 SET 是写入值字节、对 GET 是读出值字节）。

关键点：
- 服务器是阻塞单线程，流水线并发使吞吐达到服务器处理上限，反映真实最高容量。
- 与 bench_latency.py（串行延迟）互补：这里看“单位时间能处理多少个大值”。
- 无 sds 版在 >=1KB 档位会崩溃，正好体现 sds 在大值吞吐上的能力边界。
- 崩溃容错：每个档位用独立服务器进程，崩溃后自动重启继续后续档位。
"""
import socket, time, sys, subprocess, os

BIN = sys.argv[1]
OUT = sys.argv[2]
TAG = sys.argv[3] if len(sys.argv) > 3 else BIN
PORT = 6600 + (os.getpid() % 100)
SIZES = [512, 1024, 4096, 32768, 131072]   # 512B, 1KB, 4KB, 32KB, 128KB
PIPE = 200
REQS = 20000
WARM_PIPE = 30


def start():
    subprocess.run(["pkill", "-f", f"mini-redis {PORT}"], capture_output=True)
    time.sleep(0.2)
    p = subprocess.Popen([BIN, str(PORT)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(150):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=0.3); s.close(); return p
        except OSError:
            time.sleep(0.05)
    p.kill(); raise RuntimeError("not ready")


def rx_n(s, n):
    buf = b''; s.settimeout(60)
    while len(buf) < n:
        d = s.recv(1 << 20)
        if not d:
            break
        buf += d
    return buf


def tput(cmd_payload, resp_each, n, pipe):
    """以 pipe 深度流水线执行 n 条命令，返回 QPS。"""
    batch = cmd_payload * pipe
    expect = len(resp_each)
    expect_batch = expect * pipe
    # warmup 在一次性连接上进行
    s = socket.create_connection(("127.0.0.1", PORT), timeout=60)
    s.sendall(batch); rx_n(s, expect_batch)
    s.close()
    time.sleep(0.1)
    s = socket.create_connection(("127.0.0.1", PORT), timeout=120)
    got = 0
    t0 = time.perf_counter()
    while got < n:
        cnt = pipe if (n - got) >= pipe else (n - got)
        s.sendall(batch if cnt == pipe else cmd_payload * cnt)
        rx_n(s, expect * cnt)
        got += cnt
    dt = time.perf_counter() - t0
    s.close()
    return got / dt


def main():
    print(f"== {TAG} 大数据吞吐 (pipe={PIPE}, QPS + MB/s) ==")
    srv = None
    srv_alive = False
    rows = []

    def ensure_srv():
        nonlocal srv, srv_alive
        if srv_alive:
            try: srv.kill()
            except Exception: pass
        srv = start(); srv_alive = True

    def mark_crash(tag):
        print(f"  [CRASH] {tag} — 服务器崩溃/无法应答")
        rows.append((TAG, tag, "CRASH", "CRASH", "CRASH", "CRASH"))

    try:
        for vsize in SIZES:
            v = b"v" * vsize
            set_cmd = b"*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$%d\r\n" % len(v) + v + b"\r\n"
            get_cmd = b"*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n"
            get_resp = b"$%d\r\n" % len(v) + v + b"\r\n"   # GET 回包 = 值本身
            val_mb = vsize / (1024 * 1024)                  # 单条命令承载的值字节(MB)

            # ---- SET 吞吐 ----
            ensure_srv()
            tag_set = f"SET val={vsize}B"
            try:
                qps = tput(set_cmd, b"+OK\r\n", REQS, PIPE)
                pay = qps * val_mb
                rows.append((TAG, tag_set, "%.0f" % qps, "%.2f" % pay))
                print(f"  [SET {vsize:>7}B] qps={qps:9.0f}  {pay:9.2f} MB/s(写)")
            except Exception as e:
                srv_alive = False
                mark_crash(tag_set + f" ({e.__class__.__name__})")

            # ---- GET 吞吐（需要先写入档位值；若上面 SET 崩溃则跳过）----
            tag_get = f"GET val={vsize}B"
            if srv_alive:
                try:
                    # 先在服务器存入该档位值，保证 GET 命中并回包该大值
                    s = socket.create_connection(("127.0.0.1", PORT), timeout=30)
                    s.sendall(set_cmd); s.settimeout(30); s.recv(100); s.close()
                    qps = tput(get_cmd, get_resp, REQS, PIPE)
                    pay = qps * val_mb
                    rows.append((TAG, tag_get, "%.0f" % qps, "%.2f" % pay))
                    print(f"  [GET {vsize:>7}B] qps={qps:9.0f}  {pay:9.2f} MB/s(读)")
                except Exception as e:
                    srv_alive = False
                    mark_crash(tag_get + f" ({e.__class__.__name__})")
    finally:
        if srv_alive:
            try: srv.kill()
            except Exception: pass

    with open(OUT, "w") as f:
        f.write("version,cmd,qps,payload_mbs\n")
        for r in rows:
            f.write(",".join(str(x) for x in r) + "\n")
    print("-> wrote", OUT)


if __name__ == "__main__":
    main()
