#!/usr/bin/env python3
"""延迟分布基准：sds vs 无sds 的大值分档延迟 + 串行 QPS。

用法: python3 bench_latency.py <binary> <out.csv> [tag]

对每个 value 大小档位测量 SET 与 GET 的每命令延迟分布
(min / avg / p50 / p99 / max，微秒)，并给出等价串行 QPS。
同时测二进制含 \\0 值（体现 sds 的二进制安全优势）。

关键点：
- 服务器是阻塞单线程，串行「发一命令→等一回复」的 RTT 即单命令真实延迟。
- 大值分多档位，无 sds 版在 >=1KB 档位会崩溃/截断，正好体现 sds 优势。
- 崩溃容错：每个档位用独立服务器进程，崩溃后自动重启继续后续档位。
"""
import socket, time, sys, subprocess, os

BIN = sys.argv[1]
OUT = sys.argv[2]
TAG = sys.argv[3] if len(sys.argv) > 3 else BIN
PORT = 6800 + (os.getpid() % 100)
SIZES = [8, 512, 1024, 4096, 32768, 131072]
NSAMP = 3000
WARM = 200


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


def recv_exact(s, n):
    buf = b''; s.settimeout(30)
    while len(buf) < n:
        d = s.recv(1 << 20)
        if not d:
            break
        buf += d
    return buf


def latency(cmd, resp_len, n, warm):
    """串行发一命令等一回复，返回延迟样本(秒)。"""
    s = socket.create_connection(("127.0.0.1", PORT), timeout=30)
    for _ in range(warm):
        s.sendall(cmd); recv_exact(s, resp_len)
    samples = []
    for _ in range(n):
        t0 = time.perf_counter()
        s.sendall(cmd); recv_exact(s, resp_len)
        samples.append(time.perf_counter() - t0)
    s.close()
    return samples


def stats(secs):
    us = [t * 1e6 for t in secs]
    us.sort()
    n = len(us)
    p = lambda q: us[min(n - 1, int(q * n))]
    avg = sum(us) / n
    return dict(min=us[0], avg=avg, p50=p(0.50), p99=p(0.99), mx=us[-1],
                qps=1.0 / (avg * 1e-6))


def fmt(st):
    return (f"min={st['min']:6.1f} avg={st['avg']:7.1f} p50={st['p50']:7.1f} "
            f"p99={st['p99']:8.1f} max={st['mx']:8.1f}  (qps={st['qps']:8.0f})")


def main():
    print(f"== {TAG} 延迟分布 (微秒) ==")
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
        rows.append((TAG, tag, "CRASH", "CRASH", "CRASH", "CRASH", "CRASH", "CRASH"))

    try:
        # ---- 常规分档延迟 ----
        for vsize in SIZES:
            v = b"v" * vsize
            set_cmd = b"*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$%d\r\n" % len(v) + v + b"\r\n"
            get_cmd = b"*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n"
            resp_set = len(b"+OK\r\n")
            ensure_srv()
            # SET 延迟
            tag_set = f"SET val={vsize}B"
            try:
                s = latency(set_cmd, resp_set, NSAMP, WARM)
                st = stats(s)
                rows.append((TAG, tag_set, *["%.1f" % st[k] for k in ("min","avg","p50","p99","mx")], "%.0f" % st["qps"]))
                print(f"  [SET  {vsize:>7}B] " + fmt(st))
            except Exception as e:
                srv_alive = False
                import traceback; traceback.print_exc()
                mark_crash(tag_set + f" ({e.__class__.__name__}: {e})")
            # GET 延迟（同一值档位；若上面 SET 已崩溃则 GET 也会失败）
            tag_get = f"GET val={vsize}B"
            if srv_alive:
                try:
                    s = latency(get_cmd, len(b"$%d\r\n" % len(v)) + len(v) + 2, NSAMP, WARM)
                    st = stats(s)
                    rows.append((TAG, tag_get, *["%.1f" % st[k] for k in ("min","avg","p50","p99","mx")], "%.0f" % st["qps"]))
                    print(f"  [GET  {vsize:>7}B] " + fmt(st))
                except Exception as e:
                    srv_alive = False
                    import traceback; traceback.print_exc()
                    mark_crash(tag_get + f" ({e.__class__.__name__}: {e})")

        # ---- 体现 sds 优势：二进制含 \0 值往返 ----
        bval = b"A\x00xy"  # 4 字节，含 NUL —— 无 sds 会截断成 'A'
        ensure_srv()
        ok = True
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
            s.sendall(b"*3\r\n$3\r\nSET\r\n$4\r\nbink\r\n$4\r\n" + bval + b"\r\n"
                      b"*2\r\n$3\r\nGET\r\n$4\r\nbink\r\n")
            s.settimeout(3)
            chunks = []
            try:
                while True:
                    d = s.recv(4096)
                    if not d:
                        break
                    chunks.append(d)
            except socket.timeout:
                pass
            got = b"".join(chunks)
            s.close()
            ok = (got == b"+OK\r\n$4\r\n" + bval + b"\r\n")
        except Exception:
            ok = False
        rows.append((TAG, "binval(\\0)", "PASS" if ok else "FAIL/T", "PASS" if ok else "FAIL/T",
                     "PASS" if ok else "FAIL/T", "PASS" if ok else "FAIL/T",
                     "PASS" if ok else "FAIL/T", "PASS" if ok else "FAIL/T"))
        print(f"  [BIN \\0 值往返] " + ("PASS — 完整保存" if ok else "FAIL/T — 截断或崩溃"))

    finally:
        if srv_alive:
            try: srv.kill()
            except Exception: pass

    with open(OUT, "w") as f:
        f.write("version,cmd,min_us,avg_us,p50_us,p99_us,max_us,qps\n")
        for r in rows:
            f.write(",".join(str(x) for x in r) + "\n")
    print("-> wrote", OUT)


if __name__ == "__main__":
    main()
