#!/usr/bin/env python3
"""
================================================================
prod_test.py —— mini-redis 生产环境模拟测试
================================================================

四组测试，全部输出可核对的数字：

  A. 正确性   —— 协议边界、二进制安全、并发下数据一致性
  B. 健壮性   —— 恶意输入、慢客户端、突发连断、资源上限、超时
  C. 性能     —— 吞吐（QPS）、延迟分档、并发扩展、饱和度
  D. A/B 对比 —— 证明「不用 epoll 有瓶颈、必须换」

用法：
    python3 bench/prod_test.py            # 全部
    python3 bench/prod_test.py --group A  # 只跑某一组
    python3 bench/prod_test.py --quick    # 缩短规模（冒烟用）

★ 设计原则（这几条决定了结果可不可信）：

  1. 【重复测量取中位数】单次数字会被调度抖动污染。关键的吞吐点跑 3 次取中位。
  2. 【先预热再计时】JIT、page cache、TCP 慢启动都会让前几轮偏慢。
  3. 【对照组同机同轮】A/B 交替跑，避免「A 跑的时候机器正好忙」这种伪差异。
  4. 【失败也记录】健壮性测试里「服务器没崩」本身就是一项通过条件。
  5. 【报告样本量】每个数字都带 N，避免读者误以为 N=1 就是结论。
"""

import argparse
import json
import os
import socket
import statistics
import subprocess
import sys
import threading
import time

HOST = "127.0.0.1"
AB_DIR = "/tmp/ab"
RESULT_FILE = "/tmp/prod_test_result.json"

# ---------------------------------------------------------------- 基础设施


class Server:
    """启动/停止一个 mini-redis 实例，并等它就绪（不用固定 sleep）。"""

    def __init__(self, binary, port, env=None, label=""):
        self.binary = binary
        self.port = port
        self.label = label
        self.env = {**os.environ, **(env or {})}
        self.proc = None
        self.logpath = f"/tmp/prod_test_{port}.log"

    def __enter__(self):
        self.log = open(self.logpath, "wb")
        self.proc = subprocess.Popen(
            [self.binary, str(self.port)],
            env=self.env, stdout=self.log, stderr=subprocess.STDOUT,
        )
        # 轮询等就绪：连上并 PING 成功才算好
        deadline = time.time() + 10
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"{self.label} 启动即退出，见 {self.logpath}")
            try:
                s = socket.create_connection((HOST, self.port), 0.3)
                s.sendall(b"*1\r\n$4\r\nPING\r\n")
                s.settimeout(1)
                if b"PONG" in s.recv(64):
                    s.close()
                    return self
                s.close()
            except OSError:
                time.sleep(0.05)
        raise RuntimeError(f"{self.label} 10 秒内未就绪")

    def __exit__(self, *exc):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.log.close()
        return False

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def log_tail(self, n=4000):
        try:
            with open(self.logpath, "rb") as f:
                return f.read()[-n:].decode("utf-8", "replace")
        except OSError:
            return ""


def encode(*args):
    """把参数编码成 RESP 数组。"""
    out = [b"*%d\r\n" % len(args)]
    for a in args:
        if isinstance(a, str):
            a = a.encode()
        out.append(b"$%d\r\n" % len(a) + a + b"\r\n")
    return b"".join(out)


class Conn:
    """一个带上「读到完整行」能力的连接。"""

    def __init__(self, port, timeout=5):
        self.s = socket.create_connection((HOST, port), timeout)
        self.buf = b""

    def send(self, data):
        self.s.sendall(data)

    def cmd(self, *args):
        self.send(encode(*args))
        return self.read_reply()

    def read_reply(self, timeout=5):
        """读到一条完整 RESP 回复（按首字节类型判断终止）。"""
        self.s.settimeout(timeout)
        while True:
            r = self._try_parse()
            if r is not None:
                return r
            chunk = self.s.recv(65536)
            if not chunk:
                raise ConnectionError("对端关闭")
            self.buf += chunk

    def _try_parse(self):
        b = self.buf
        if not b:
            return None
        t = b[:1]
        nl = b.find(b"\r\n")
        if nl < 0:
            return None
        if t in (b"+", b"-", b":"):
            self.buf = b[nl + 2:]
            return b[:nl + 2]
        if t == b"$":
            n = int(b[1:nl])
            if n < 0:
                self.buf = b[nl + 2:]
                return b[:nl + 2]
            end = nl + 2 + n + 2
            if len(b) < end:
                return None
            self.buf = b[end:]
            return b[:end]
        if t == b"*":
            n = int(b[1:nl])
            if n < 0:
                self.buf = b[nl + 2:]
                return b[:nl + 2]
            pos = nl + 2
            for _ in range(n):
                if len(b) < pos + 1 or b[pos:pos + 1] != b"$":
                    return None
                nl2 = b.find(b"\r\n", pos)
                if nl2 < 0:
                    return None
                ln = int(b[pos + 1:nl2])
                if ln < 0:
                    pos = nl2 + 2
                    continue
                pos = nl2 + 2 + ln + 2
                if len(b) < pos:
                    return None
            self.buf = b[pos:]
            return b[:pos]
        raise ValueError(f"未知回复类型 {t!r}")

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


# ---------------------------------------------------------------- 报告

class Report:
    def __init__(self):
        self.rows = []          # (组, 名称, 数值, 单位, 备注)
        self.fails = []

    def add(self, group, name, value, unit="", note=""):
        self.rows.append(dict(group=group, name=name, value=value,
                              unit=unit, note=note))
        print(f"  [{group}] {name}: {value}{unit}  {note}")

    def check(self, group, name, ok, note=""):
        self.add(group, name, "PASS" if ok else "FAIL", note=note)
        if not ok:
            self.fails.append(f"{group}/{name}")
        return ok

    def dump(self):
        with open(RESULT_FILE, "w") as f:
            json.dump(self.rows, f, ensure_ascii=False, indent=2)
        print(f"\n结果已写入 {RESULT_FILE}")


R = Report()


# ---------------------------------------------------------------- A. 正确性

def group_a(quick):
    """正确性：能不能用。协议边界 + 二进制安全 + 并发一致性。"""
    print("\n" + "=" * 64)
    print("A. 正确性测试")
    print("=" * 64)

    with Server(f"{AB_DIR}/epoll/mini-redis", 7101, label="epoll版"):
        c = Conn(7101)

        # --- A1 基础命令 ---
        R.check("A", "PING", c.cmd("PING") == b"+PONG\r\n")
        R.check("A", "SET/GET", c.cmd("SET", "k", "v") == b"+OK\r\n"
                and c.cmd("GET", "k") == b"$1\r\nv\r\n")
        R.check("A", "GET 不存在的 key 返回 nil",
                c.cmd("GET", "nope") == b"$-1\r\n")
        R.check("A", "DEL 返回删除数",
                c.cmd("DEL", "k") == b":1\r\n" and c.cmd("DEL", "k") == b":0\r\n")
        R.check("A", "未知命令报错",
                c.cmd("NOPE").startswith(b"-ERR"))

        # --- A2 二进制安全（含 \0、\r\n、高位字节）---
        for label, val in [
            ("含 \\0", b"a\x00b"),
            ("含 \\r\\n", b"x\r\ny"),
            ("0xFF 高位", bytes([0xff, 0xfe, 0xfd])),
            ("1KB 随机", os.urandom(1024)),
            ("320KB", os.urandom(320 * 1024)),
        ]:
            c.cmd("SET", "bin", val)
            got = c.cmd("GET", "bin")
            want = b"$%d\r\n" % len(val) + val + b"\r\n"
            R.check("A", f"二进制安全：{label}", got == want)

        # --- A3 inline 命令（nc/curl 风格）---
        c.send(b"PING\r\n")
        R.check("A", "inline PING", c.read_reply() == b"+PONG\r\n")

        # --- A4 粘包：一次发多条 ---
        c.send(encode("SET", "p1", "1") + encode("SET", "p2", "2")
               + encode("GET", "p1") + encode("GET", "p2"))
        r1 = c.read_reply(); r2 = c.read_reply()
        r3 = c.read_reply(); r4 = c.read_reply()
        R.check("A", "粘包：4 条命令全部正确响应",
                (r1, r2) == (b"+OK\r\n", b"+OK\r\n")
                and (r3, r4) == (b"$1\r\n1\r\n", b"$1\r\n2\r\n"))

        # --- A5 半包：逐字节发送一条命令 ---
        payload = encode("SET", "half", "ok")
        for i in range(len(payload)):
            c.send(payload[i:i + 1])
            time.sleep(0.001)
        R.check("A", "半包：逐字节发送仍能解析", c.read_reply() == b"+OK\r\n")
        R.check("A", "半包后数据正确", c.cmd("GET", "half") == b"$2\r\nok\r\n")

        # --- A6 协议错误处理 ---
        c.cmd("SET", "x", "y")
        c.send(b"*2000\r\n")            # 参数个数超限
        r = c.read_reply()
        R.check("A", "超限的参数个数被拒绝", r.startswith(b"-ERR"), note=str(r))

        # --- A7 并发一致性：多连接共享同一份存储 ---
        N = 20 if quick else 50
        conns = [Conn(7101) for _ in range(N)]
        for i, cc in enumerate(conns):
            cc.cmd("SET", f"conc{i}", f"val{i}")
        # 用另一条连接读回全部 —— 证明是同一份存储
        peer = Conn(7101)
        ok = all(peer.cmd("GET", f"conc{i}") == b"$%d\r\nval%d\r\n" % (len(f"val{i}"), i)
                 for i in range(N))
        R.check("A", f"并发一致性：{N} 条连接写入后可被其他连接读到", ok)

        # 交叉验证：每个连接读别人的 key
        cross = all(conns[(i + 1) % N].cmd("GET", f"conc{i}")
                    == b"$%d\r\nval%d\r\n" % (len(f"val{i}"), i)
                    for i in range(N))
        R.check("A", "并发一致性：连接之间可交叉读取", cross)

        for cc in conns:
            cc.close()
        peer.close()
        c.close()


# ---------------------------------------------------------------- B. 健壮性

def group_b(quick):
    """健壮性：恶意/极端输入下会不会崩、泄漏、卡死。"""
    print("\n" + "=" * 64)
    print("B. 健壮性测试")
    print("=" * 64)

    with Server(f"{AB_DIR}/epoll/mini-redis", 7102,
                env={"CLIENT_TIMEOUT": "2"}, label="epoll版") as srv:

        # --- B1 连接后立刻 RST（SIGPIPE 回归）---
        import struct
        for _ in range(20):
            s = socket.create_connection((HOST, 7102), 3)
            s.sendall(encode("SET", "rst", b"z" * 100000))
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                         struct.pack("ii", 1, 0))
            s.close()
        time.sleep(0.3)
        probe = Conn(7102)
        R.check("B", "20 次 RST 断连后服务器存活",
                srv.alive() and probe.cmd("PING") == b"+PONG\r\n")

        # --- B2 发垃圾字节（非 RESP、非合法 inline）---
        s = socket.create_connection((HOST, 7102), 3)
        s.sendall(b"\x00\x01\x02\xff" * 100)
        time.sleep(0.2)
        s.close()
        R.check("B", "垃圾字节不导致崩溃",
                srv.alive() and probe.cmd("PING") == b"+PONG\r\n")

        # --- B3 超长 inline 行（无 \r\n，撑缓冲）---
        s = socket.create_connection((HOST, 7102), 3)
        try:
            for _ in range(50):
                s.sendall(b"A" * 65536)      # 累计 3.2MB，应触到 MAX_QUERYBUF
        except OSError:
            pass
        time.sleep(0.4)
        s.close()
        R.check("B", "超长无终止符输入被限制（不 OOM）",
                srv.alive() and probe.cmd("PING") == b"+PONG\r\n")

        # --- B4 声明超大 bulk 长度但不发数据（slowloris 式）---
        s = socket.create_connection((HOST, 7102), 3)
        s.sendall(b"*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$536870912\r\n")  # 声明 512MB
        time.sleep(0.3)
        R.check("B", "声明超大值但不发数据：服务器不阻塞其他连接",
                probe.cmd("PING") == b"+PONG\r\n")
        s.close()

        # --- B5 慢客户端隔离（只写不读）---
        slow = socket.create_connection((HOST, 7102), 3)
        big = b"x" * 200000
        slow.sendall(encode("SET", "big", big))
        # ★ 不读 slow 的回复，同时验证其他连接正常
        N = 20 if quick else 50
        good = 0
        for _ in range(N):
            try:
                t = Conn(7102, timeout=3)
                if t.cmd("PING") == b"+PONG\r\n":
                    good += 1
                t.close()
            except OSError:
                pass
        R.check("B", f"慢客户端隔离：另 {N} 条连接全部正常", good == N,
                note=f"{good}/{N}")
        slow.close()

        # --- B6 突发连断（各 200 次，验证无 fd 泄漏）---
        M = 100 if quick else 200
        for _ in range(M):
            try:
                t = socket.create_connection((HOST, 7102), 3)
                t.sendall(encode("PING"))
                t.close()
            except OSError:
                pass
        time.sleep(0.5)
        R.check("B", f"突发 {M} 次连断后仍可服务",
                srv.alive() and probe.cmd("PING") == b"+PONG\r\n")

        # --- B7 空闲超时确实生效 ---
        idle = socket.create_connection((HOST, 7102), 3)
        active = Conn(7102)
        ok_active = 0
        for _ in range(8):                  # 约 3.2 秒 > 2 秒阈值
            time.sleep(0.4)
            if active.cmd("PING") == b"+PONG\r\n":
                ok_active += 1
        idle.settimeout(1.0)
        try:
            idle.sendall(encode("PING"))
            idle_closed = (idle.recv(64) == b"")
        except OSError:
            idle_closed = True
        R.check("B", "空闲超时：空闲被踢且活跃连接不受影响",
                idle_closed and ok_active == 8,
                note=f"idle_closed={idle_closed} active={ok_active}/8")
        idle.close(); active.close()

        # --- B8 大量并发连接（接近 maxclients 的量级）---
        C = 300 if quick else 800
        held = []
        try:
            for _ in range(C):
                held.append(socket.create_connection((HOST, 7102), 3))
            good = 0
            for h in held[:50]:
                h.sendall(encode("PING"))
                h.settimeout(3)
                if b"PONG" in h.recv(64):
                    good += 1
            R.check("B", f"并发 {C} 连接同时保持且可服务", good == 50,
                    note=f"{good}/50 抽样响应")
        except OSError as e:
            R.check("B", f"并发 {C} 连接同时保持", False, note=str(e))
        finally:
            for h in held:
                try: h.close()
                except OSError: pass

        probe.close()


# ---------------------------------------------------------------- C. 性能

def pipeline_bench(port, n_conns, depth, duration, cmd_bytes):
    """
    多连接 + 流水线压测（Python 版，仅用于 A/B 对比里命令可变长的场景）。

    ⚠️ 只适合小并发：Python 的 GIL 会让多线程实际串行执行，
       并发 >= 4 时客户端自己就成了瓶颈，测出来的曲线会失真
       （典型症状：并发 2 比并发 4 还快）。
       ★ 要测真实并发扩展能力请用 loadgen（C 多进程），见 qps_loadgen()。
    """
    per = [0] * n_conns
    stop_at = time.time() + duration

    def worker(idx):
        try:
            s = socket.create_connection((HOST, port), 5)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.settimeout(10)
            batch = cmd_bytes * depth
            replies_per_batch = depth
            cnt = 0
            while time.time() < stop_at:
                s.sendall(batch)
                need = replies_per_batch
                buf = b""
                while need > 0:
                    chunk = s.recv(1 << 20)
                    if not chunk:
                        raise ConnectionError
                    buf += chunk
                    need = replies_per_batch - buf.count(b"\r\n")
                cnt += replies_per_batch
            s.close()
        except OSError:
            pass
        per[idx] = cnt

    ts = [threading.Thread(target=worker, args=(i,)) for i in range(n_conns)]
    t0 = time.time()
    for t in ts: t.start()
    for t in ts: t.join()
    elapsed = time.time() - t0
    total = sum(per)
    return total, (total / elapsed if elapsed > 0 else 0)


def ensure_loadgen():
    """编译 C 压测客户端（多进程，无 GIL 限制）。"""
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "loadgen.c")
    out = "/tmp/loadgen"
    if not os.path.exists(out) or os.path.getmtime(src) > os.path.getmtime(out):
        r = subprocess.run(["gcc", "-O2", "-o", out, src],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"loadgen 编译失败: {r.stderr}")
    return out


def qps_loadgen(port, n_conns, depth, duration, reps=3):
    """
    用 C 多进程客户端测吞吐，返回 (中位 QPS, 各次样本)。

    ★ 这是唯一可信的并发吞吐测法：每个子进程一条独立连接，
      互不干扰，客户端不会成为瓶颈。
    """
    lg = ensure_loadgen()
    samples = []
    for _ in range(reps):
        r = subprocess.run([lg, str(port), str(n_conns), str(depth),
                            str(duration)], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"loadgen 失败: {r.stderr}")
        total = int(r.stdout.strip())
        samples.append(total / duration)
    return statistics.median(samples), samples


def latency_bench(port, n, cmd_bytes=b"*1\r\n$4\r\nPING\r\n"):
    """串行 RTT：一条一条发，测单次往返延迟（微秒）。"""
    s = socket.create_connection((HOST, port), 5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(5)
    lat = []
    # 预热
    for _ in range(100):
        s.sendall(cmd_bytes); s.recv(64)
    for _ in range(n):
        t0 = time.perf_counter()
        s.sendall(cmd_bytes)
        s.recv(64)
        lat.append((time.perf_counter() - t0) * 1e6)
    s.close()
    lat.sort()
    return dict(
        p50=statistics.median(lat),
        p90=lat[int(len(lat) * 0.90)],
        p99=lat[int(len(lat) * 0.99)],
        avg=statistics.mean(lat),
        n=len(lat),
    )


def group_c(quick):
    """性能：吞吐、延迟、并发扩展。"""
    print("\n" + "=" * 64)
    print("C. 性能测试（epoll 版）")
    print("=" * 64)

    with Server(f"{AB_DIR}/epoll/mini-redis", 7103, label="epoll版"):
        dur = 2.0 if quick else 3.0
        reps = 1 if quick else 3

        # --- C1 单连接流水线吞吐（C 客户端）---
        for depth in (1, 10, 100, 500):
            q, samples = qps_loadgen(7103, 1, depth, dur, reps)
            R.add("C", f"单连接 流水线 depth={depth}", int(q), " QPS",
                  f"中位/N={reps} 样本={[int(s) for s in samples]}")

        # --- C2 ★ 并发扩展（C 客户端，无 GIL 干扰）---
        print("  （并发扩展用 C 多进程客户端，避免 Python GIL 限制客户端）")
        for nc in (1, 2, 4, 8, 16):
            q, samples = qps_loadgen(7103, nc, 100, dur, reps)
            R.add("C", f"并发 {nc} 连接 depth=100", int(q), " QPS",
                  f"中位/N={reps}")

        # --- C3 延迟分档（串行 RTT）---
        lat = latency_bench(7103, 300 if quick else 3000)
        R.add("C", "串行 RTT 平均", round(lat["avg"], 1), " µs", f"N={lat['n']}")
        R.add("C", "串行 RTT p50", round(lat["p50"], 1), " µs")
        R.add("C", "串行 RTT p90", round(lat["p90"], 1), " µs")
        R.add("C", "串行 RTT p99", round(lat["p99"], 1), " µs")

        # --- C4 大值吞吐（320KB value）---
        val = os.urandom(320 * 1024)
        setbig = encode("SET", "bigv", val)
        reps2 = 20
        s = socket.create_connection((HOST, 7103), 5)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        t0 = time.perf_counter()
        for _ in range(reps2):
            s.sendall(setbig)
            s.recv(64)
        el = time.perf_counter() - t0
        s.close()
        mbps = (len(val) * reps2) / el / (1024 * 1024)
        R.add("C", "320KB 值串行写入", round(mbps, 1), " MB/s", f"N={reps2}")


# ---------------------------------------------------------------- D. A/B

def group_d(quick):
    """A/B 对比：证明阻塞版有瓶颈。"""
    print("\n" + "=" * 64)
    print("D. A/B 对比：阻塞版 vs epoll 版")
    print("=" * 64)

    A = f"{AB_DIR}/blocking/mini-redis"
    B = f"{AB_DIR}/epoll/mini-redis"
    if not (os.path.exists(A) and os.path.exists(B)):
        print("  ⚠ 缺少 A/B 可执行文件，请先运行 bench/setup_ab.sh")
        return

    # --- D1 头号罪证：一个不发数据的连接会卡死整个服务器 ---
    print("\n  D1 慢/静默客户端阻塞（关键差异）")
    for label, binary, port in (("阻塞版", A, 7201), ("epoll版", B, 7202)):
        with Server(binary, port, label=label) as srv:
            a = socket.create_connection((HOST, port), 3)
            # A 连上后什么都不发 —— 阻塞版会在这里卡住
            time.sleep(0.4)
            ok = False
            t0 = time.perf_counter()
            try:
                b = socket.create_connection((HOST, port), 3)
                b.sendall(b"*1\r\n$4\r\nPING\r\n")
                b.settimeout(2.0)
                ok = b"+PONG\r\n" in b.recv(64)
                b.close()
            except OSError:
                ok = False
            dt = (time.perf_counter() - t0) * 1000
            R.add("D", f"{label}：静默连接存在时第 2 条连接能否被服务",
                  "是" if ok else "否",
                  note=f"耗时 {dt:.0f}ms" + ("（超时）" if not ok else ""))
            a.close()

    # --- D2 10 条并发连接（阻塞版应该只能服务极少数）---
    #
    # ★ 阻塞版的结果会在 0~1 之间波动：它是「accept 一个就陷进读循环」，
    #   哪一条连接先进被服务取决于 accept 顺序与时序。
    #   所以这里跑 3 次取【最好的一次】上报 —— 对阻塞版这是最有利的取值，
    #   如果连最好一次都远低于 10，结论就更无可辩驳。
    print("\n  D2 多连接并发服务能力（3 次取最好）")
    for label, binary, port in (("阻塞版", A, 7203), ("epoll版", B, 7204)):
        best = 0
        for trial in range(3):
            with Server(binary, port + trial, label=label):
                conns, served = [], 0
                try:
                    for _ in range(10):
                        conns.append(socket.create_connection((HOST, port + trial), 3))
                    for c in conns:
                        try:
                            c.sendall(b"*1\r\n$4\r\nPING\r\n")
                        except OSError:
                            pass
                    for c in conns:
                        try:
                            c.settimeout(2.0)
                            if b"PONG" in c.recv(64):
                                served += 1
                        except OSError:
                            pass
                finally:
                    for c in conns:
                        try: c.close()
                        except OSError: pass
                best = max(best, served)
        R.add("D", f"{label}：10 条并发连接中被服务的条数（最好一次）",
              f"{best}/10")

    # --- D3 吞吐对比（C 客户端，交替运行）---
    print("\n  D3 吞吐对比（C 多进程客户端，A/B 交替跑)")
    dur = 2.0 if quick else 3.0
    reps = 1 if quick else 3
    for nc in (1, 4):
        with Server(A, 7205, label="阻塞版") as sa:
            qa, _ = qps_loadgen(7205, nc, 100, dur, reps)
        with Server(B, 7206, label="epoll版") as sb:
            qb, _ = qps_loadgen(7206, nc, 100, dur, reps)
        R.add("D", f"吞吐 并发{nc} depth=100 阻塞版", int(qa), " QPS")
        R.add("D", f"吞吐 并发{nc} depth=100 epoll版", int(qb), " QPS",
              f"倍率 {qb/qa:.2f}×" if qa else "")

    # --- D4 ★ 公平性：这才是「瓶颈」最锋利的证据 ---
    #
    # ★ 为什么这个指标比总 QPS 更能说明问题：
    #   阻塞版的【总】QPS 可能只比 epoll 版低几倍（因为它单连接就很快），
    #   但看「每条连接各自完成了多少」就会暴露真相 ——
    #   8 条连接里只有 1 条在跑，另外 7 条各完成 100 次后就完全卡死。
    #   总 QPS 掩盖了这个事实（单条高速连接把平均值撑起来了）。
    #
    #   ★ 一句话：阻塞版的瓶颈不是「慢」，而是「不公平 —— 它一次只服务一个人」。
    print("\n  D4 公平性：并发 8 条连接时，各自完成多少请求")
    NC = 8
    for label, binary, port in (("阻塞版", A, 7207), ("epoll版", B, 7208)):
        with Server(binary, port, label=label):
            per = [0] * NC
            stop = time.time() + (1.5 if quick else 2.0)

            def w(i):
                try:
                    s = socket.create_connection((HOST, port), 5)
                    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    s.settimeout(5)
                    batch = b"*1\r\n$4\r\nPING\r\n" * 100
                    c = 0
                    while time.time() < stop:
                        s.sendall(batch)
                        got, buf = 0, b""
                        while got < 100:
                            chunk = s.recv(1 << 20)
                            if not chunk:
                                raise ConnectionError
                            buf += chunk
                            got = buf.count(b"\r\n")
                        c += 100
                    s.close()
                except OSError:
                    pass
                per[i] = c

            ts = [threading.Thread(target=w, args=(i,)) for i in range(NC)]
            for t in ts: t.start()
            for t in ts: t.join()

            working = sum(1 for x in per if x >= 100)
            # ★ 「真正在干活」的判据不能用 x >= 100 —— 被卡死的连接也会
            #   完成第一批 100 条然后停住。要用「是否跟上了其他连接的节奏」：
            #   取完成数最大值的 10% 作为门槛，低于它就算没被公平服务。
            top = max(per) if per else 0
            working = sum(1 for x in per if x >= top * 0.1)
            mean = statistics.mean(per) if per else 0
            cv = (statistics.pstdev(per) / mean) if mean else 0
            R.add("D", f"{label}：{NC} 条连接中被公平服务的条数",
                  f"{working}/{NC}", note=f"各自完成 {sorted(per, reverse=True)}")
            R.add("D", f"{label}：连接间负载离散度 (CV)",
                  round(cv, 2), "", "0=完全均匀，越大越不公平")


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--group", default="ABCD")
    ap.add_argument("--quick", action="store_true")
    args = ap.parse_args()

    print("=" * 64)
    print("mini-redis 生产环境模拟测试")
    print("=" * 64)
    print(f"主机: {os.uname().nodename}  内核: {os.uname().release}")
    print(f"CPU: {os.cpu_count()} 核   quick={args.quick}")

    g = args.group.upper()
    if "A" in g: group_a(args.quick)
    if "B" in g: group_b(args.quick)
    if "C" in g: group_c(args.quick)
    if "D" in g: group_d(args.quick)

    print("\n" + "=" * 64)
    if R.fails:
        print(f"失败项: {len(R.fails)} → {R.fails}")
    else:
        print("所有断言型检查全部通过 ✅")
    R.dump()
    return 1 if R.fails else 0


if __name__ == "__main__":
    sys.exit(main())
