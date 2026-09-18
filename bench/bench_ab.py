#!/usr/bin/env python3
"""argv 不释放给哈希（steal）优化 vs 默认深拷贝：SET8 延迟/吞吐交错 A/B 压测。

用法: python3 bench_ab.py <binary_default> <binary_steal> <out.csv> [rounds]

要点：
- 同一脚本轮换启动两个二进制（交错、各跑 rounds 轮），用中位数比，
  抵消机器负载漂移 —— 单次跑不可信（本机吞吐波动 ±30%）。
- 只测受优化影响的 SET8：默认每 SET 做 2 次 sdsdup(malloc+memcpy)+2 次
  sdsfree；steal 直接把解析出的 key/value 所有权移交哈希表，省掉这些。
- 每轮固定新端口，避免 TIME_WAIT 重绑。
"""
import socket, time, sys, subprocess, os, statistics

DEF, STEAL, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
ROUNDS = int(sys.argv[4]) if len(sys.argv) > 4 else 6
_PORT = [7400 + (os.getpid() % 80)]
NSAMP = 300
REQS = 5000
PIPE = 200
set8 = b"*3\r\n$3\r\nSET\r\n$8\r\nbenchkey\r\n$8\r\nbenchval\r\n"


def start(bin):
    _PORT[0] += 1
    p = _PORT[0]
    subprocess.run(["pkill", "-f", f"mini-redis {p}"], capture_output=True)
    time.sleep(0.1)
    proc = subprocess.Popen([bin, str(p)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(200):
        try:
            s = socket.create_connection(("127.0.0.1", p), timeout=0.3); s.close(); return proc
        except OSError:
            time.sleep(0.03)
    proc.kill(); raise RuntimeError("not ready")


def rx_n(s, n):
    b = b''; s.settimeout(60)
    while len(b) < n:
        d = s.recv(1 << 20)
        if not d: break
        b += d
    return b


def tput(bin):
    p = start(bin)
    try:
        s = socket.create_connection(("127.0.0.1", _PORT[0]), timeout=60)
        batch = set8 * PIPE; s.sendall(batch); rx_n(s, 5 * PIPE); s.close()
        time.sleep(0.05)
        s = socket.create_connection(("127.0.0.1", _PORT[0]), timeout=120)
        got = 0; t0 = time.perf_counter()
        while got < REQS:
            cnt = min(PIPE, REQS - got)
            s.sendall(batch if cnt == PIPE else set8 * cnt)
            rx_n(s, 5 * cnt); got += cnt
        dt = time.perf_counter() - t0; s.close()
        return REQS / dt
    finally:
        sh = socket.socket();  # ensure cleanup later
        try: p.kill()
        except Exception: pass


def lat(bin):
    p = start(bin)
    try:
        s = socket.create_connection(("127.0.0.1", _PORT[0]), timeout=120)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        for _ in range(100):
            s.sendall(set8); rx_n(s, 5)
        sam = []
        for _ in range(NSAMP):
            t = time.perf_counter(); s.sendall(set8); rx_n(s, 5)
            sam.append((time.perf_counter() - t) * 1e6)
        s.close()
        sam.sort()
        return statistics.mean(sam), sam[int(len(sam) * 0.50)]
    finally:
        try: p.kill()
        except Exception: pass


def main():
    print(f"== argv-steal vs 默认: SET8 交错 A/B (rounds={ROUNDS}) ==")
    td, ts, ld, ls = [], [], [], []
    for r in range(1, ROUNDS + 1):
        # 交错顺序：奇轮 先默认后 steal，偶轮 反序，抵消时间漂移
        order = [(DEF, 0), (STEAL, 1)] if r % 2 == 1 else [(STEAL, 1), (DEF, 0)]
        for bin, who in order:
            q = tput(bin)
            a, p50 = lat(bin)
            if who == 0: td.append(q); ld.append(a)
            else: ts.append(q); ls.append(a)
            print(f"  轮{r} {'default' if who==0 else 'steal '}: tput={q:8.0f}QPS  lat_avg={a:6.1f}us")
    m_df, m_st = statistics.median(td), statistics.median(ts)
    m_ld, m_ls = statistics.median(ld), statistics.median(ls)
    with open(OUT, "w") as f:
        f.write("metric,default,steal,steal_vs_default_pct\n")
        f.write("tput_qps_med,%d,%d,%.1f\n" % (m_df, m_st, (m_st / m_df - 1) * 100))
        f.write("lat_avg_us_med,%.1f,%.1f,%.1f\n" % (m_ld, m_ls, (m_ls / m_ld - 1) * 100))
    print("-> wrote", OUT)


if __name__ == "__main__":
    main()
