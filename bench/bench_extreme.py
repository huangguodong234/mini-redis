#!/usr/bin/env python3
"""极限吞吐 + 延迟 + 跳表(zset) 综合压测：sds默认 / sds-steal(argv不释放) / 无sds。

用法: python3 bench_extreme.py <binary> <out.csv> [tag]

对每个被测二进制依次测量：
  ① 小命令极限吞吐（流水线深度 200）: SET 8B、GET 8B
  ② 串行每命令延迟分布（min/avg/p50/p99/max, 微秒）: SET 8B、GET 8B
  ③ 跳表(zset) 吞吐 + 延迟: ZADD(8B member)、ZSCORE

输出 CSV：version,metric,command,value (value 为 qps 或 微秒等)
崩溃容错：每档位独立服务器，崩溃自动重启继续。
"""
import socket, time, sys, subprocess, os, statistics

BIN = sys.argv[1]
OUT = sys.argv[2]
TAG = sys.argv[3] if len(sys.argv) > 3 else BIN
PORT0 = 6900 + (os.getpid() % 90)   # 起点
_PORT_CTR = [0]                      # 每次 start() 递增，保证每台服务器用不同端口，
                                     # 避免同端口重启撞上 TIME_WAIT 导致 bind 失败
CURR_PORT = [PORT0]                  # 当前服务器端口（start() 里更新）
PIPE = 200
REQS = 50000          # 吞吐采样命令数
NSAMP = 3000          # 延迟采样数
WARM = 200            # 延迟预热


def start():
    _PORT_CTR[0] += 1
    port = PORT0 + _PORT_CTR[0]
    CURR_PORT[0] = port
    subprocess.run(["pkill", "-f", f"mini-redis {port}"], capture_output=True)
    time.sleep(0.1)
    p = subprocess.Popen([BIN, str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(200):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=0.3); s.close(); return p
        except OSError:
            time.sleep(0.03)
    p.kill(); raise RuntimeError("server not ready on port " + str(port))


def rx_n(s, n):
    buf = b''; s.settimeout(60)
    while len(buf) < n:
        d = s.recv(1 << 20)
        if not d:
            break
        buf += d
    return buf


# ---------- 吞吐（流水线） ----------
def tput(cmd_payload, resp_each, n, pipe):
    batch = cmd_payload * pipe
    expect = len(resp_each)
    expect_batch = expect * pipe
    # warmup（一次性连接）
    s = socket.create_connection(("127.0.0.1", CURR_PORT[0]), timeout=60)
    s.sendall(batch); rx_n(s, expect_batch)
    s.close()
    time.sleep(0.1)
    s = socket.create_connection(("127.0.0.1", CURR_PORT[0]), timeout=120)
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


# ---------- 吞吐：逐命令变化的流水线（用于 ZADD 唯一成员，压真正插入/查找成本） ----------
def tput_stream(make_payload, resp_each, n, pipe):
    """make_payload(i) 生成第 i 条命令；每条成员唯一，实测跳表在表增长下的吞吐。
    resp_each 为每条回复的字节长度，流水发 pipe 条收 pipe 份。"""
    expect = len(resp_each)
    s = socket.create_connection(("127.0.0.1", CURR_PORT[0]), timeout=120)
    pos = 0
    t0 = time.perf_counter()
    while pos < n:
        cnt = min(pipe, n - pos)
        batch = b"".join(make_payload(pos + j) for j in range(cnt))
        s.sendall(batch)
        rx_n(s, expect * cnt)
        pos += cnt
    dt = time.perf_counter() - t0
    s.close()
    return pos / dt


# ---------- 串行延迟 ----------
def lat_serial(cmd_payload, resp_bytes, n):
    """resp_bytes：该命令的完整期望回复（用其字节数做 rx_n 长度，避免人工数错）。"""
    resp_n = len(resp_bytes)
    s = socket.create_connection(("127.0.0.1", CURR_PORT[0]), timeout=120)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    for _ in range(WARM):
        s.sendall(cmd_payload); rx_n(s, resp_n)
    sam = []
    for _ in range(n):
        t0 = time.perf_counter()
        s.sendall(cmd_payload); rx_n(s, resp_n)
        sam.append((time.perf_counter() - t0) * 1e6)
    s.close()
    sam.sort()
    return {
        "min": min(sam), "avg": statistics.mean(sam),
        "p50": sam[int(len(sam) * 0.50)], "p99": sam[int(len(sam) * 0.99)],
        "max": max(sam),
    }


def main():
    print(f"== {TAG} 极限吞吐/延迟/跳表 综合压测 ==")
    srv = None; srv_alive = False
    rows = []

    def ensure_srv():
        nonlocal srv, srv_alive
        if srv_alive:
            try: srv.kill()
            except Exception: pass
        srv = start(); srv_alive = True

    def crash(tag):
        nonlocal srv_alive
        srv_alive = False
        print(f"  [CRASH] {tag}")
        rows.append((TAG, tag, "CRASH"))

    # 命令模板
    set8 = b"*3\r\n$3\r\nSET\r\n$8\r\nbenchkey\r\n$8\r\nbenchval\r\n"
    get8 = b"*2\r\n$3\r\nGET\r\n$8\r\nbenchkey\r\n"
    ZKEY = b"benchkey"
    def zadd_payload(i):
        mem = b"m%08d" % i                      # 每个命令唯一成员（5GB 内不重复）
        return b"*4\r\n$4\r\nZADD\r\n$%d\r\n%s\r\n$2\r\n10\r\n$%d\r\n%s\r\n" % (len(ZKEY), ZKEY, len(mem), mem)
    def zscore_payload(i):
        return b"*3\r\n$6\r\nZSCORE\r\n$%d\r\n%s\r\n$9\r\nm00000001\r\n" % (len(ZKEY), ZKEY)

    try:
        # ========== ① 小命令极限吞吐 ==========
        ensure_srv()
        try:
            q = tput(set8, b"+OK\r\n", REQS, PIPE)
            rows.append((TAG, "tput_set8", "%.0f" % q))
            print(f"  [吞吐 SET8B] {q:9.0f} QPS")
        except Exception:
            crash("tput_set8")
        if srv_alive:
            try:
                s = socket.create_connection(("127.0.0.1", CURR_PORT[0]), timeout=30)
                s.sendall(set8); rx_n(s, 5); s.close()
                q = tput(get8, b"$8\r\nbenchval\r\n", REQS, PIPE)
                rows.append((TAG, "tput_get8", "%.0f" % q))
                print(f"  [吞吐 GET8B] {q:9.0f} QPS")
            except Exception:
                crash("tput_get8")

        # ========== ② 串行延迟 ==========
        ensure_srv()
        for name, cmd, respb in [("lat_set8", set8, b"+OK\r\n"), ("lat_get8", get8, b"$8\r\nbenchval\r\n")]:
            if not srv_alive: break
            try:
                if name == "lat_get8":
                    s = socket.create_connection(("127.0.0.1", CURR_PORT[0]), timeout=30)
                    s.sendall(set8); rx_n(s, 5); s.close()
                d = lat_serial(cmd, respb, NSAMP)
                rows.append((TAG, name, "%0.1f" % d["min"], "%0.1f" % d["avg"], "%0.1f" % d["p50"], "%0.1f" % d["p99"], "%0.1f" % d["max"]))
                print(f"  [延迟 {name}] avg={d['avg']:.1f}us p50={d['p50']:.1f} p99={d['p99']:.1f}")
            except Exception as e:
                crash(name, e)

        # ========== ③ 跳表 zset 吞吐 + 延迟 ==========
        # 注意：当前 skiplist_find 是 O(n) 线性扫（非 O(log n)），ZADD 每次先 O(n)
        # 查重再插入，故表越大单操越慢——这正是“跳表区别”要测的。为控制时长，
        # 跳表档用较小的规模（预填 scale + REQS_Z 万量级），避免 O(n²) 拖死压测。
        ensure_srv()
        PREFILL, REQS_Z = 2000, 5000
        try:
            s = socket.create_connection(("127.0.0.1", CURR_PORT[0]), timeout=120)
            # 预填 PREFILL 个成员，让查找有真实规模可扫
            batch = b"".join(zadd_payload(b) for b in range(PREFILL))
            s.sendall(batch); rx_n(s, 5 * PREFILL)
            s.close()
        except Exception:
            pass

        for name, fn, resp in [
            ("tput_zadd_n", "unique_member_growing", b"+OK\r\n"),
            ("tput_zscore_n", "fixed_hit", b"$2\r\n10\r\n"),
        ]:
            if not srv_alive: break
            try:
                if fn == "unique_member_growing":
                    q = tput_stream(zadd_payload, b"+OK\r\n", REQS_Z, PIPE)
                else:
                    q = tput(zscore_payload(0), b"$2\r\n10\r\n", REQS_Z, PIPE)
                rows.append((TAG, name, "%.0f" % q))
                print(f"  [吞吐 {name}] {q:9.0f} QPS ({fn})")
            except Exception as e:
                crash(name, e)
        for name, cmd, respb in [
            ("lat_zadd", zadd_payload(PREFILL + 90000), b"+OK\r\n"),
            ("lat_zscore", zscore_payload(0), b"$2\r\n10\r\n"),
        ]:
            if not srv_alive: break
            try:
                d = lat_serial(cmd, respb, NSAMP)
                rows.append((TAG, name, "%0.1f" % d["min"], "%0.1f" % d["avg"], "%0.1f" % d["p50"], "%0.1f" % d["p99"], "%0.1f" % d["max"]))
                print(f"  [延迟 {name}] avg={d['avg']:.1f}us p50={d['p50']:.1f} p99={d['p99']:.1f}")
            except Exception as e:
                crash(name, e)
    finally:
        if srv_alive:
            try: srv.kill()
            except Exception: pass

    with open(OUT, "w") as f:
        f.write("version,metric,command,value,extra1,extra2,extra3,extra4\n")
        for r in rows:
            f.write(",".join(str(x) for x in r) + "\n")
    print("-> wrote", OUT)


if __name__ == "__main__":
    main()
