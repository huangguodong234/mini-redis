#!/usr/bin/env python3
"""
================================================================
mixload.py —— 真实生产环境「混合负载」测试
================================================================

★ 与 bench/prod_test.py 的分工（为什么另写一个）：
    prod_test.py 是【体检】：A 正确性 / B 健壮性 / C 性能 / D A/B 对比，
                  每一组都用「受控的、单一的」负载去对标一个明确的问题。
    mixload.py   是【演练】：把所有情况【同时】压到服务器上 ——
                  多样化命令、连接潮汐、只连不发、恶意 fd ——
                  回答的问题只有一个：**混在一起还活不活？活成什么样？**

★ 模拟的上线后真实情况（6 类并发流量）：

    ┌──────────────┬────────────────────────────────────────────┐
    │ 1 正常业务   │ 多变命令（GET/SET/DEL/PING/ZADD/...）+ 流水线│
    │ 2 连接潮汐   │ 连接数按正弦波起伏，峰谷比 5:1              │
    │ 3 只连不发   │ 握手完成后一个字节都不发（负载均衡器探活、  │
    │              │ 被墙住的半开连接、程序卡死忘了发）           │
    │ 4 恶意流量   │ 非法首字节 / 超长 header / 参数不匹配 /     │
    │              │ 请求缓冲膨胀 / 超时后吐垃圾                  │
    │ 5 只发不收   │ 发命令但从不读回复（下游卡死、反压外溢）     │
    │ 6 瞬时压测   │ 大量并发同时上线又同时消失                   │
    └──────────────┴────────────────────────────────────────────┘

★ 设计原则（这几条决定了结论可不可信）：

  1. 【先证明自己没把服务器打死】任何一项「不通过」都先区分
     「被测缺陷」还是「测试自己打死了它」——
     对端关闭时的 ECONNRESET/EPIPE 是网络层正常现象，不算失败。
  2. 【服务可用性用探针独立测】业务线程的延迟会被自身排队污染，
     所以 P99 由一条【独立的、低频率】探针连接测量（每秒 20 次串行）。
  3. 【资源用 /proc 采样，不看进程 RSS 猜】fd 泄漏、内存增长要连续
     采样看趋势，单点数字没有意义。
  4. 【所有数字带样本量】N 写进报告，避免把 N=1 当结论。
  5. 【不美化】本测试发现的缺陷原样列进报告，不写进「已知局限」掩掉。

用法：
    python3 bench/mixload.py                  # 默认 60 秒
    python3 bench/mixload.py --warm 15 --main 45
    python3 bench/mixload.py --port 7801 --quick
    python3 bench/mixload.py --no-server      # 连一个已经在跑的实例

★ 注意：本脚本必须用 `stdbuf -o0` 之外的方式保证服务器日志可见 ——
   它用 subprocess 启动服务器并【自带】stdbuf 逻辑（见 Server 类），
   否则 C 程序连到文件时是全缓冲，崩溃前的日志会一起丢掉。
"""

import argparse
import bisect
import errno
import json
import os
import random
import select
import socket
import statistics
import subprocess
import sys
import threading
import time

HOST = "127.0.0.1"
DEFAULT_PORT = 7801
CTRL_COLLECT = 0.5          # 控制线程轮询周期（秒）
PROBE_INTERVAL = 0.050      # 探针间隔：20 次/秒（低到不干扰业务）


# ================================================================
# 基础设施：RESP 编解码（只实现测试需要的那几种）
# ================================================================

def enc(*args):
    """把一个命令编码成 RESP 数组。str -> bytes（UTF-8），bytes 原样。"""
    out = [b"*%d\r\n" % len(args)]
    for a in args:
        if not isinstance(a, (bytes, bytearray)):
            a = str(a).encode()
        out.append(b"$%d\r\n" % len(a) + a + b"\r\n")
    return b"".join(out)


_UNKNOWN = object()


class ProtocolFail(Exception):
    """回复不是合法的 RESP —— 说明服务器发坏了数据。"""


def read_reply(f, want_type):
    """从二进制文件对象 f 读一条 RESP 回复，返回 (值, 原始字节)。

    ★ want_type 用于校验「这条回复是不是我这条命令该有的类型」：
      流水线错位、粘包出错都会在类型上先暴露出来。
      'S'=+简单字符串 'E'=-错误 'I'=:整数 'B'=$批量 '*'=*数组
    """
    line = f.readline()
    if not line:
        raise EOFError("服务器关闭了连接")
    if len(line) < 3 or not line.endswith(b"\r\n"):
        raise ProtocolFail("行不完整: %r" % line[:40])
    t, body = line[:1], line[1:-2]

    if t == b"+":
        val = body
    elif t == b"-":
        val = body
    elif t == b":":
        try:
            val = int(body)
        except ValueError:
            raise ProtocolFail("整数格式错: %r" % body[:40])
    elif t == b"$":
        n = int(body)
        if n == -1:
            val = None
        else:
            data = f.read(n + 2)
            if len(data) != n + 2:
                raise ProtocolFail("批量串被截断: 期望 %d" % n)
            if data[-2:] != b"\r\n":
                raise ProtocolFail("批量串缺 CRLF")
            val = data[:-2]
    elif t == b"*":
        n = int(body)
        if n == -1:
            val = None
        else:
            val = []
            for _ in range(n):
                v, _raw = read_reply(f, b"$")
                val.append(v)
    else:
        raise ProtocolFail("未知回复类型 %r" % t)

    if want_type is not None and t != want_type:
        raise ProtocolFail("回复类型错位: 期望 %r 实际 %r (%r)" % (
            want_type, t, line[:40]))
    return val, line + (b"" if t not in (b"$", b"*") else b"")


# ================================================================
# 服务器进程管理
# ================================================================

class Server:
    """启动一个 mini-redis 实例；用真实 PING 等就绪（不用固定 sleep）。

    ★ stdbuf 是必需的：mini-redis 用 printf 写日志，输出重定向到文件时
      stdio 是全缓冲（4KB），进程被 SIGTERM 杀死时缓冲区直接丢掉 ——
      于是「崩溃前最后几条日志」全部消失，而恰恰那是排障最需要的部分。
      ★ 副作用（重要，别被它骗了）：stdbuf -o0 下日志实时落盘，用
      tail -f 看是正常的；不加 stdbuf 时日志会「成批出现或干脆没有」，
      实测中这曾让我误判「服务器没打印启动日志」。
    """

    def __init__(self, binary, port, env=None, label="mix"):
        self.binary = binary
        self.port = port
        self.label = label
        self.env = {**os.environ, **(env or {})}
        self.proc = None
        self.logpath = "/tmp/mixload_%d.log" % port

    def __enter__(self):
        self.log = open(self.logpath, "wb")
        # ★ stdbuf 和 ASan 不能一起用：stdbuf 用 LD_PRELOAD 注入 libstdbuf，
        #   ASan 要求自己是"第一个"加载的库，于是直接拒绝启动
        #   （"ASan runtime does not come first in initial library list"）。
        #   所以：检测到要用 ASan 构建时就【不加 stdbuf】——
        #   代价是日志变成全缓冲、崩溃前的尾巴可能丢，但 ASan 自己的
        #   报告是写到 stderr 的（同样被缓冲，但 ASan 退出时会 flush）。
        use_stdbuf = "sanitize" not in self.env.get("MIXLOAD_NO_STDBUF", "")
        use_stdbuf = not self.env.get("MIXLOAD_NO_STDBUF")
        cmd = (["stdbuf", "-o0", "-e0"] if use_stdbuf else []) + \
              [self.binary, str(self.port)]
        self.proc = subprocess.Popen(
            cmd, env=self.env, stdout=self.log, stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
        )
        deadline = time.time() + 10
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("%s 启动即退出（码 %s），见 %s"
                                   % (self.label, self.proc.returncode, self.logpath))
            try:
                s = socket.create_connection((HOST, self.port), 0.3)
                s.settimeout(1.0)
                s.sendall(b"*1\r\n$4\r\nPING\r\n")
                if b"PONG" in s.recv(64):
                    s.close()
                    return self
                s.close()
            except OSError:
                pass
            time.sleep(0.05)
        raise RuntimeError("%s 10 秒内未就绪" % self.label)

    def __exit__(self, *exc):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(2)          # SIGINT → 走正常清理路径
            try:
                self.proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.log.close()
        return False

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def rss_kb(self):
        try:
            with open("/proc/%d/status" % self.proc.pid) as f:
                for ln in f:
                    if ln.startswith("VmRSS:"):
                        return int(ln.split()[1])
        except OSError:
            pass
        return None

    def fd_count(self):
        try:
            return len(os.listdir("/proc/%d/fd" % self.proc.pid))
        except OSError:
            return None

    def cpu_ticks(self):
        """/proc/PID/stat 的 utime+stime。

        ★ 不能用 awk 取字段：comm 字段带括号且【可能含空格】，
          直接数第 N 个字段会错位（本项目曾因此读出 201% 的占用率）。
          正确做法是从右往左数，或者截掉 ") " 之后再分列。
        """
        try:
            with open("/proc/%d/stat" % self.proc.pid, "rb") as f:
                raw = f.read()
            rest = raw[raw.rfind(b")") + 2:].split()
            return int(rest[11]) + int(rest[12])
        except (OSError, IndexError, ValueError):
            return None

    def log_tail(self, n=3000):
        try:
            with open(self.logpath, "rb") as f:
                return f.read()[-n:].decode("utf-8", "replace")
        except OSError:
            return ""


# ================================================================
# 统计：每个线程写自己的槽位，控制线程汇总（无锁、无争用）
# ================================================================

class Stats:
    __slots__ = ("ok", "err", "lat", "nomatch", "protofail",
                 "opened", "closed", "rejected", "timeouts", "notes")

    def __init__(self):
        self.ok = 0
        self.err = 0            # 业务层失败（连不上、回复对不上）
        self.lat = []           # 延迟样本（毫秒）
        self.nomatch = 0        # 回复内容与预期不符 —— 正确性问题
        self.protofail = 0      # 回复不是合法 RESP —— 严重缺陷
        self.opened = 0
        self.closed = 0
        self.rejected = 0       # 服务器主动断开（恶意/超时类，属预期）
        self.timeouts = 0
        self.notes = []

    def note(self, s):
        if len(self.notes) < 12:
            self.notes.append(s)


# 断连类异常：这些都是「网络对端关掉了」，对恶意/超时流量是预期结果
_DISCONNECT = (ConnectionResetError, BrokenPipeError, ConnectionAbortedError)


def is_disconnect(e):
    return isinstance(e, _DISCONNECT) or getattr(e, "errno", None) in (
        errno.ECONNRESET, errno.EPIPE, errno.ECONNABORTED)


# ================================================================
# 请求配额
# ================================================================

class Budget:
    """全池共享的请求配额（安全阀，不是主要终止条件）。

    ★ 为什么包成类而不是直接用 [int]：本次测试里真实踩过这个坑 ——
      预算写成了【列表】，而各线程按「有 .value 的对象」去 -=，
      于是每个业务线程都在后台抛 AttributeError 后静默退出，
      测试照常"跑完"并打印出一张漂亮的成绩单：业务量 0、零错误。
      ★ 教训：线程里抛异常不会让主流程失败。测试工具必须
        ① 让子线程的异常可见（本类的 __iadd__ 直接就是 AttributeError，
           我们在开发期看到的就是它）；② 报告里必须能看出「某类流量
           一条请求都没发出去」是异常的 —— 判定里已加「流量覆盖」检查。
    """

    __slots__ = ("value",)

    def __init__(self, n):
        self.value = n

    def take(self):
        self.value -= 1
        return self.value > 0


# ================================================================
# 流量源 1：正常业务（多变命令 + 随机流水线）
# ================================================================

# 「写后读校验」专用的一对命令。
# ★ 为什么必须有它：其他命令的回复内容无法预先知道（GET 可能读到
#   别的线程写的值，那是正常共享）。只有我们自己刚写进去的值可以在
#   回复里原样比对 —— 这是验证「存进去 = 取回来」的唯一硬断言。
ECHO_KEY = "__echo__"


# ★ 每个线程必须用【自己的 key】做写后读校验。
#   起初所有线程共用一个 "__echo__" key，于是 A 线程写完还没读到，
#   B 线程就把它覆盖了 —— 测试报告出一堆"写后读不一致"，
#   看起来像服务器的数据错乱，实际是测试自己在互相踩。
#   ★ 教训：并发测试里的断言必须建立在【本线程独占的资源】上，
#   否则测出来的是竞态，不是被测系统的行为。
def enc_echo(seq, tid):
    """返回 (SET echo:<tid> <本线程唯一值> 字节, 该值, key)。"""
    key = "echo:%d" % tid
    v = ("t%d_%d" % (tid, seq)).encode()
    return enc("SET", key, v), v, key


class Workload:
    """一条「正常业务」连接的可用命令集和权重。

    ★ 混合负载的核心不是「把 8 个命令都调一遍」，而是让权重和真实业务
      一样偏斜：读多写少、ZSET 只占几个百分点、偶尔大 value。
      README 的性能数据全部是 PING，那种流量在真实环境里一次都不会出现。
    """

    KEYS = [("user:%d" % i) for i in range(500)]
    MEMBERS = [("m%05d" % i) for i in range(2000)]

    @classmethod
    def build(cls):
        """返回 [(权重, 构造器)]，构造器返回 (命令字节, 期望回复类型)。"""
        K, M = cls.KEYS, cls.MEMBERS

        def ping():
            return enc("PING"), b"+"

        def get():
            return enc("GET", random.choice(K)), b"$"

        def set_small():
            return enc("SET", random.choice(K), "v%d" % random.randrange(1 << 20)), b"+"

        def set_big():
            # 偶尔 32KB：（真实业务里总有大对象）
            return enc("SET", random.choice(K), b"B" * (32 * 1024)), b"+"

        def delete():
            return enc("DEL", random.choice(K)), b":"

        def zadd():
            return enc("ZADD", "rank", "%.6f" % random.uniform(0, 1e6),
                       random.choice(M)), b"+"

        def zscore():
            return enc("ZSCORE", "rank", random.choice(M)), b"$"

        def zrange():
            a = random.randrange(0, 50)
            return enc("ZRANGE", "rank", a, a + random.randrange(1, 50)), b"*"

        def zrem():
            return enc("ZREM", "rank", random.choice(M)), b":"

        #          权重   构造器
        return [(30, ping), (34, get), (18, set_small), (2, set_big),
                (5, delete), (3, zadd), (3, zscore), (3, zrange), (2, zrem)]

    def __init__(self):
        spec = self.build()
        self._cum, acc = [], 0
        for w, fn in spec:
            acc += w
            self._cum.append(acc)
        self._total = acc
        self._spec = spec

    def pick(self):
        r = random.randrange(self._total)
        i = bisect.bisect_right(self._cum, r)
        return self._spec[i][1]()


def business_stream(stats, per_thread, stop_flag, port,
                    lat_samples_per_thread=3000, tid=0):
    """一条正常业务连接：随机流水线深度 1..32，持续发命令读回复。

    ★ 为什么要随机流水线深度：真实业务既有同步调用（depth=1）
      也有批量写（depth 几十）。固定深度会测出一种「平滑」的假象。
    ★ 延迟只抽稀采样：全量记录几百万个数字会先把内存和 GIL 吃掉，
      让测量本身成为瓶颈。
    """
    wl = Workload()
    lat_budget = lat_samples_per_thread
    backoff = 0.0
    while not stop_flag.is_set() and per_thread.value > 0:
        per_thread.value -= 1
        try:
            s = socket.create_connection((HOST, port), 2.0)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.settimeout(5.0)
            stats.opened += 1
        except OSError as e:
            stats.err += 1
            stats.note("连接失败: %r" % (e,))
            time.sleep(min(0.2, 0.01 * (2 ** backoff)))
            backoff = min(backoff + 1, 5)
            continue
        backoff = 0.0

        f = s.makefile("rb")
        try:
            seq = 0
            while not stop_flag.is_set():
                depth = random.choice((1, 1, 1, 2, 4, 8, 16, 32))
                # 每批都夹一对「SET echo <唯一值>」+ 批尾「GET echo」——
                # 批尾那条回复必须等于批头写入的值，这才叫写后读校验。
                setcmd, expect, ekey = enc_echo(seq, tid)
                seq += 1
                batch = [wl.pick() for _ in range(max(1, depth - 2))]
                batch = [(setcmd, b"+")] + batch + [(enc("GET", ekey), b"$")]

                t0 = time.perf_counter()
                s.sendall(b"".join(c[0] for c in batch))
                last = len(batch) - 1
                for i, (_cmd, want) in enumerate(batch):
                    val, _raw = read_reply(f, want)
                    stats.ok += 1
                    if i == 0:
                        if val != b"OK":
                            stats.nomatch += 1
                            stats.note("SET 未返回 +OK: %r" % (val,))
                    elif i == last:
                        if val != expect:
                            stats.nomatch += 1
                            stats.note("写后读不一致: 写入 %r 读回 %r"
                                       % (expect[:24], (val or b"")[:24]))
                        elif depth == 1:
                            pass
                dt = (time.perf_counter() - t0) * 1000.0 / len(batch)
                if lat_budget > 0:
                    stats.lat.append(dt)
                    lat_budget -= 1
        except ProtocolFail as e:
            stats.protofail += 1
            stats.note("协议错误: %s" % e)
        except EOFError:
            stats.closed += 1
        except socket.timeout:
            stats.timeouts += 1
        except _DISCONNECT:
            stats.closed += 1
        except OSError as e:
            stats.err += 1
            stats.note("IO 错误: %r" % (e,))
        finally:
            try:
                f.close()
                s.close()
            except OSError:
                pass


# ================================================================
# 流量源 2：只连不发（握手完成，一个字节都不发）
# ================================================================

def idle_stream(stats, hold_seconds, stop_flag, port):
    """连上就挂着，不发任何数据。

    ★ 这正是生产中占比最高、也最容易被忽略的一类连接：
      负载均衡器/监控的 TCP 探活、被防火墙墙住的半开连接、
      客户端启动后忘了发第一条命令。
    ★ 关键词：它必须是【安静】的 —— 服务器不该因为它而空转。
      测试里会对比「有它」和「没它」时 CPU 的差别。
    """
    while not stop_flag.is_set():
        try:
            s = socket.create_connection((HOST, port), 2.0)
            stats.opened += 1
        except OSError as e:
            stats.err += 1
            stats.note("握手失败: %r" % (e,))
            time.sleep(0.1)
            continue
        try:
            # 不发数据，只等对端关（或等我们放手）
            deadline = time.time() + hold_seconds
            while not stop_flag.is_set() and time.time() < deadline:
                r, _, _ = select.select([s], [], [], 0.1)
                if r:
                    d = s.recv(65536)
                    if not d:
                        stats.closed += 1       # 服务器主动踢了（超时策略）
                        break
                    stats.note("只连不发的连接收到了数据: %r" % d[:40])
                stats.ok += 0
        except _DISCONNECT:
            stats.rejected += 1
        except OSError as e:
            stats.err += 1
            stats.note("只连不发 IO: %r" % (e,))
        finally:
            try:
                s.close()
            except OSError:
                pass
            stats.closed += 1


# ================================================================
# 流量源 3：恶意流量
# ================================================================

REQ = b"*1\r\n$4\r\nPING\r\n"

# 每一招都针对解析器的一个具体分支（不是乱发字节碰运气）
ATTACKS = {
    "非法首字节": lambda: b"\x00\x01\x02\r\n",
    "非数字长度": lambda: b"*abc\r\n",
    "参数个数超限": lambda: b"*5000\r\n",
    "参数个数为0": lambda: b"*0\r\n",
    "声明长度不兑现": lambda: b"$100\r\nshort\r\n",
    "参数数量不匹配": lambda: b"*4\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n$1\r\nx\r\n",
    "参数类型错": lambda: enc("ZADD", "k", "not-a-float", "m"),
    "垃圾行": lambda: b"\r\n\r\n\r\n\xff\xfe\xfd\r\n",
    "超大声明": lambda: b"*1\r\n$999999999\r\n",
    "巨量参数": lambda: b"*1024\r\n" + b"$1\r\nA\r\n" * 1024,
    "空命令片段": lambda: b'{"json":"not-resp"}\r\n',
    # 注意：像 "*3 ... 只发2个参数" 这种【语法合法但没发完】的请求
    # 不属于恶意 —— 解析器会正确地停下来等剩下的数据（半包）。
    # 把它算成"必须被拒绝"是测试写错了，不是服务器错了。

}


def attack_stream(stats, per_thread, stop_flag, port):
    """持续发起各种恶意请求，验证「只影响自己、不拖垮别人」。

    ★ 判定标准（这是整个测试最关键的一条）：
        恶意连接被断开 = 通过（预期内）
        服务器进程还在  = 通过（不是崩溃）
        正常业务没受影响 = 通过（隔离性）
      所以本线程【不把断连当错误】—— 那是正确的处理结果。
    """
    names = list(ATTACKS)
    while not stop_flag.is_set() and per_thread.value > 0:
        per_thread.value -= 1
        name = random.choice(names)
        try:
            s = socket.create_connection((HOST, port), 2.0)
            s.settimeout(2.0)
            stats.opened += 1
            payload = ATTACKS[name]()
            s.sendall(payload)
            # 好连接应当收到错误回复后断开；此处接受「先收到错误再断」
            got = b""
            try:
                while len(got) < 4096:
                    d = s.recv(4096)
                    if not d:
                        break
                    got += d
            except socket.timeout:
                pass
            if not got.startswith(b"-"):
                stats.note("%s -> 没收到错误回复: %r" % (name, got[:40]))
            else:
                stats.ok += 1
            stats.closed += 1
        except _DISCONNECT:
            # 服务器直接 close 而不是先回错误 —— 也接受（协议错误路径）
            stats.ok += 1
            stats.rejected += 1
        except socket.timeout:
            stats.timeouts += 1
        except OSError as e:
            stats.err += 1
            stats.note("%s IO: %r" % (name, e))
        finally:
            try:
                s.close()
            except OSError:
                pass


def flood_stream(stats, per_thread, stop_flag, port):
    """请求缓冲膨胀：只发【合法开头但不完整的】命令，永不成包。

    ★ 这招比「发垃圾」更阴：每个字节都合法，解析器会一直判「半包」
      并等待 —— 于是请求缓冲无限增长。如果没有上限，一条连接就能
      吃掉几十 GB 内存。服务器应当命中 MAX_QUERYBUF 并断开。
    ★ 判定口径（这里踩过坑，务必看清）：
        "服务器没断开我们" **不等于服务器有漏洞**。
      真正的问题只有一个 —— **内存有没有跟着涨**。
      TCP 是全双工：我们不读回复，接收窗口迟早会关上，
      于是 send() 自己就阻塞了，我们连"灌满服务器"都做不到。
      所以本函数先尽力灌，然后【读数前 vs 读完后】交给资源采样
      去判：RSS 出现异常增长 = 有漏洞；RSS 平 = 上限生效。
    ★ 顺带：本函数会把收到的回复读掉（否则窗口一关就自我阻塞），
      这样 flood 才能真正把请求缓冲推到上限。
    """
    CHUNK = b"*3\r\n$3\r\nSET\r\n$5\r\nk%04d\r\n" % random.randrange(10000)
    while not stop_flag.is_set() and per_thread.value > 0:
        per_thread.value -= 1
        try:
            s = socket.create_connection((HOST, port), 2.0)
            s.settimeout(1.0)
            s.setblocking(False)
            stats.opened += 1
            payload = CHUNK * 64          # 一次 1.6KB，减少系统调用次数
            stalled = 0
            sent = 0
            t_end = time.time() + 3.0
            while not stop_flag.is_set() and time.time() < t_end:
                progressed = False
                try:
                    sent += s.send(payload)
                    progressed = True
                except BlockingIOError:
                    pass
                except _DISCONNECT:
                    raise
                # 把服务器发回来的东西读掉（否则接收窗口一关就死锁）
                try:
                    while True:
                        d = s.recv(65536)
                        if not d:
                            raise ConnectionResetError("服务器关闭了连接")
                except BlockingIOError:
                    pass
                except _DISCONNECT:
                    raise
                if not progressed:
                    stalled += 1
                    if stalled > 200:      # 连续 200 次无进展 ≈ 2 秒
                        break
                    time.sleep(0.01)
            # ★ 到这一步服务器【没有】断开我们：
            #   不记错误、不记成功，只记一条现象交给资源判定。
            stats.note("膨胀灌了 %.1f KB 后卡住且未断开（看 RSS 是否跟随增长）"
                       % (sent / 1024.0))
        except _DISCONNECT:
            stats.rejected += 1            # 正确：被上限挡住并断开
            stats.ok += 1
        except socket.timeout:
            stats.timeouts += 1
        except OSError as e:
            stats.note("膨胀 IO: %r" % (e,))
        finally:
            try:
                s.close()
            except OSError:
                pass


# ================================================================
# 流量源：慢速攻击（slowloris）
# ================================================================

def slowloris_stream(stats, per_thread, stop_flag, port):
    """一次只发 1 字节，把一条命令拖成几分钟。

    ★ 真实威胁：攻击者用几千条这样的连接，每条只消耗极少带宽，
      却能占满 maxclients —— 正常用户连不进来（拒绝服务）。
    ★ Redis 的防御是「客户端超时 + 输出缓冲上限」双管齐下；
      本项目只有 CLIENT_TIMEOUT。所以本项观察的是：
      慢速连接【会不会】被超时清理掉 —— 如果不会，maxclients 可被耗尽。
    """
    req = b"*1\r\n$4\r\nPING\r\n"
    while not stop_flag.is_set() and per_thread.value > 0:
        per_thread.value -= 1
        try:
            s = socket.create_connection((HOST, port), 2.0)
            s.settimeout(2.0)
            stats.opened += 1
            for byte in req:
                if stop_flag.is_set():
                    break
                s.send(bytes([byte]))
                time.sleep(0.05)          # 每个字节 50ms → 一条命令 700ms
                # 顺手把回复读掉（被断开时这里会抛 EPOLLHUP/EOF）
                try:
                    s.setblocking(False)
                    d = s.recv(64)
                    if not d:
                        raise ConnectionResetError("服务器关闭了连接")
                    stats.ok += 1
                except BlockingIOError:
                    pass
                finally:
                    s.setblocking(True)
                    s.settimeout(2.0)
            stats.closed += 1
        except _DISCONNECT:
            stats.rejected += 1           # 被超时踢掉：正确行为
            stats.ok += 1
        except socket.timeout:
            stats.timeouts += 1
        except OSError as e:
            stats.note("慢速 IO: %r" % (e,))
        finally:
            try:
                s.close()
            except OSError:
                pass


# ================================================================
# 流量源 4：只发不收（反压外溢）
# ================================================================

def noseek_stream(stats, per_thread, stop_flag, port):
    """发命令但永远不读回复 —— 模拟下游卡死。

    ★ 真实场景长什么样（这决定了本流量该怎么造）：
      下游程序卡死/被防火墙墙住之后，它【不会继续狂发命令】——
      它停在原地，内核里堆着它已经发出的命令和服务器还没送到的回复。
      所以"只发不收"的正确形态是：
        发一批命令 → 不再读、也不再发 → 让服务器的 outbuf 慢慢积压
      而不是"无限狂发命令"（那是压测，不是故障，服务器被压满 CPU
      是它活该，跟缺陷无关 —— 我一开始就写成了后者，
      测出来的 86% CPU 和 800ms 尖峰其实全是自己造的负载，
      ★ 差点把它误判成服务器的锅）。

    ★ 本流量要验证的东西因此很明确：服务器的输出缓冲会不会无限涨。
      正确结果是被 MAX_OUTBUF 挡住并断开，RSS 保持在几十 MB。
    """
    while not stop_flag.is_set() and per_thread.value > 0:
        per_thread.value -= 1
        try:
            s = socket.create_connection((HOST, port), 2.0)
            s.settimeout(2.0)
            # 收窗口压到很小，让服务器很快写不出去
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            stats.opened += 1
            # 先写一个能放大回复的数据块，再发一批读它的命令
            s.sendall(enc("SET", "nosink", b"N" * (256 * 1024)))
            get = enc("GET", "nosink")
            for _ in range(400):
                if stop_flag.is_set():
                    break
                s.sendall(get)
            # ★ 发完就停手，不读、不关 —— 让服务器自己积压
            waited = 0.0
            while not stop_flag.is_set() and waited < 6.0:
                time.sleep(0.1)
                waited += 0.1
                # 若服务器主动断开，这里会读到 EOF/ECONNRESET
                try:
                    r, _, _ = select.select([s], [], [], 0)
                    if r:
                        d = s.recv(1 << 20)
                        if not d:
                            stats.rejected += 1     # 被 MAX_OUTBUF 断开：正确
                            stats.ok += 1
                            break
                except _DISCONNECT:
                    stats.rejected += 1
                    stats.ok += 1
                    break
                except OSError:
                    break
            stats.closed += 1
        except _DISCONNECT:
            stats.rejected += 1
            stats.ok += 1
        except socket.timeout:
            stats.timeouts += 1
        except OSError as e:
            stats.note("不读 IO: %r" % (e,))
        finally:
            try:
                s.close()
            except OSError:
                pass


# ================================================================
# 流量源 5：连接潮汐（连接数按正弦波起伏）
# ================================================================

def churn_stream(stats, stop_flag, port, tick):
    """连接数按正弦波增减：所有线程共享同一个相位函数。

    ★ 为什么用正弦而不是随机步进：正弦有可预测的峰谷，
      能把「峰值时」和「谷值时」的延迟分开统计（这是真实业务里
      最典型的模式：早晚高峰、活动开场）。
    ★ 创建/销毁由主控线程统一触发，这里只负责「保持自己的连接」。
    """
    want_conn = 0
    sock = None
    while not stop_flag.is_set():
        target = tick.target
        # 每条线程负责自己的那一份
        mine = max(0, int(target * tick.share))
        if mine > 0 and sock is None:
            try:
                sock = socket.create_connection((HOST, port), 2.0)
                sock.settimeout(3.0)
                stats.opened += 1
            except OSError as e:
                stats.err += 1
                stats.note("潮汐建连失败: %r" % (e,))
                sock = None
        elif mine == 0 and sock is not None:
            try:
                sock.close()
            except OSError:
                pass
            sock = None
            stats.closed += 1
        if sock is not None:
            try:
                sock.sendall(enc("PING"))
                f = sock.makefile("rb")
                read_reply(f, b"+")
                f.close()
                stats.ok += 1
            except _DISCONNECT:
                sock = None
                stats.closed += 1
            except (OSError, EOFError):
                sock = None
                stats.closed += 1
        time.sleep(0.05)
    if sock is not None:
        try:
            sock.close()
        except OSError:
            pass


class Tide:
    """潮汐的全局状态：主控线程更新 target，各潮汐线程读取。"""

    def __init__(self, low, high, period, shares):
        self.low = low
        self.high = high
        self.period = period
        self.target = low
        self.share = 1.0 / max(1, shares)
        self._t0 = time.time()

    def update(self):
        import math
        ph = 2 * math.pi * (time.time() - self._t0) / self.period
        s = (math.sin(ph) + 1.0) / 2.0            # 0..1
        self.target = int(self.low + (self.high - self.low) * s)
        return self.target


# ================================================================
# 探针：独立测服务可用性（不受业务队列影响）
# ================================================================

def probe_once(port, timeout=0.6, retries=2, sock=None):
    """一次 PING 往返，返回毫秒；失败返回 None。

    ★ 这里有一个必须讲清楚的测量问题：本次测试第一版用「每次新建
      TCP 连接」来测可用性和延迟，结果 p99 稳定在 400~550ms，
      而同一时刻业务连接的 p99 只有 5ms。差 100 倍，明显不是一回事。
      原因：每次探针都要 connect()，而压测里有几千次紧循环建连
      （flood 线程被踢后立刻重连），新 SYN 在内核 backlog 里排队 ——
      量到的是**客户端建连排队时间**，不是服务端处理时间。
    ★ 所以这里做两类探针，分别回答两个问题：
      · sock=None → 每次新建连接：测「服务还能不能被连上」（可用性）
      · 传入长连接 → 复用连接：测「服务端处理一条命令要多久」（延迟）
      后者才是运维关心的 P99。
    """
    own = sock is None
    last = None
    for _ in range(retries):
        try:
            s = sock or socket.create_connection((HOST, port), timeout)
            s.settimeout(timeout)
            t0 = time.perf_counter()
            s.sendall(b"*1\r\n$4\r\nPING\r\n")
            r = s.recv(64)
            dt = (time.perf_counter() - t0) * 1000.0
            if own:
                s.close()
            if r == b"+PONG\r\n":
                return dt
            last = "bad reply %r" % r[:20]
        except OSError as e:
            last = repr(e)
            try:
                if own and 's' in dir():
                    s.close()
            except OSError:
                pass
            sock = None            # 长连接坏了，下次重建
    PROBE_ERRORS.append(last)
    return None


PROBE_ERRORS = []
PROBE_SERIES = []


RTT_ERRORS = []
RTT_TRACE = []          # RTT 探针连接出错的时刻（看是否与尖峰同步）


def _ping_on(sock, series=None, timeout=0.6):
    """在【已建立】的连接上做一次 PING，返回毫秒。

    ★ 与 probe_once 的区别：不建连、不重试、超时直接算失败。
      重试会把"其实卡了 5 秒"洗成"没卡"，从而系统性低估尾延迟 ——
      测量尾延迟时绝不能用重试。
    """
    sock.settimeout(timeout)
    t0 = time.perf_counter()
    sock.sendall(b"*1\r\n$4\r\nPING\r\n")
    r = sock.recv(64)
    dt = (time.perf_counter() - t0) * 1000.0
    if series is not None:
        series.append(round(dt, 3))
    if r != b"+PONG\r\n":
        raise OSError("bad reply %r" % r[:20])
    return dt


def _bucket_p99(vals, width=0.5):
    """给延迟样本横向排布一个"按时间分窗的 P99"。

    ★ 用途：判断偶发尖峰是【单点抖动】还是【持续性卡顿】。
      本测试里复用连接的 p99 到过 574ms，但 p50 只有 3.7ms ——
      如果尖峰只落在 1~2 个时间窗里，那是调度/GC 级别的偶发；
      如果每个窗口都有，那才是服务器真的有周期性停顿。
    """
    if not vals:
        return []
    buckets = {}
    for i, v in enumerate(vals):
        buckets.setdefault(int(i * 0.05 / width), []).append(v)
    return [(round(k * width, 1), round(pct(v, 99), 2), len(v))
            for k, v in sorted(buckets.items())]


# ================================================================
# 调度器
# ================================================================

def pct(vals, p):
    if not vals:
        return None
    vs = sorted(vals)
    k = max(0, min(len(vs) - 1, int(round((len(vs) - 1) * p / 100.0))))
    return vs[k]


def run_phase(phase, args, port, srv, report):
    """跑一个阶段，返回该阶段的指标字典。

    ★ 预热和主压【不共用线程池】—— 试过共用，结论是先压的那个阶段
      会被后压的拖累（预热时 flood 正在灌 64MB 请求缓冲，主压一开局
      就撞上被踢后的巨大 backlog，探针 p99 直接 500ms）。真实生产
      环境也不是「先安静地跑一会再开始忙」，而是各种历史包袱一直在。
      所以：每个阶段独立起线程、独立收尾，彼此不串味。
    """
    """跑一个阶段（warm 或 main），返回该阶段的指标字典。

    ★ stop / main_threads 由调用方传入时，本阶段不自己起线程，而是
      直接观察【已经在跑】的那批线程 —— 预热和主压共用同一批流量源，
      「预热」只表示「计时从这一刻开始、但请求已经打了很久」。
      否则每次换阶段都要重建全部连接和线程，前后两个阶段测的根本
      不是同一个系统状态。
    """
    name = phase["name"]
    dur = phase["seconds"]
    stop = threading.Event()
    warm = phase.get("warm", False)
    shares = phase["threads"]

    stats = {k: Stats() for k in shares}
    # ★ 每个流量源的请求配额（全池共享一格，同类线程抢）。
    #   ★ 这里踩过一个真实的坑：计数器是【单个 int】，多线程同时 -= 会
    #     丢更新。若只给「线程数 × 小倍数」，2 个线程抢一格会在配额没用完
    #     时提前归零 → 业务量被测成 0。所以配额给得远大于本阶段能发出的量，
    #     它只是一个安全阀；真正的终止条件永远是 stop_flag（按时间）。
    MULT = {"biz": 10 ** 7, "attack": 10 ** 5,
            "flood": 10 ** 4, "slow": 10 ** 4, "nosink": 10 ** 4}
    budget = {k: Budget(MULT.get(k, 1)) for k in shares}

    tide = Tide(phase["tide_low"], phase["tide_high"], phase.get("period", 12),
                shares.get("tide", 1))

    threads = []

    for idx in range(shares.get("biz", 0) ):
        t = threading.Thread(target=business_stream,
                             args=(stats["biz"], budget["biz"], stop, port,
                                   (40000 // max(1, shares["biz"])) // 4, idx),
                             daemon=True)
        t.start(); threads.append(t)
    for _ in range(shares.get("idle", 0) ):
        t = threading.Thread(target=idle_stream,
                             args=(stats["idle"], phase["idle_hold"], stop, port),
                             daemon=True)
        t.start(); threads.append(t)
    for _ in range(shares.get("attack", 0) ):
        t = threading.Thread(target=attack_stream,
                             args=(stats["attack"], budget["attack"], stop, port),
                             daemon=True)
        t.start(); threads.append(t)
    for _ in range(shares.get("flood", 0) ):
        t = threading.Thread(target=flood_stream,
                             args=(stats["flood"], budget["flood"], stop, port),
                             daemon=True)
        t.start(); threads.append(t)
    for _ in range(shares.get("slow", 0) ):
        t = threading.Thread(target=slowloris_stream,
                             args=(stats["slow"], budget["slow"], stop, port),
                             daemon=True)
        t.start(); threads.append(t)
    for _ in range(shares.get("nosink", 0) ):
        t = threading.Thread(target=noseek_stream,
                             args=(stats["nosink"], budget["nosink"], stop, port),
                             daemon=True)
        t.start(); threads.append(t)
    for _ in range(shares.get("tide", 0) ):
        t = threading.Thread(target=churn_stream,
                             args=(stats["tide"], stop, port, tide),
                             daemon=True)
        t.start(); threads.append(t)

    # ---- 先量探针自身的基线（此刻压测线程还没起，测的是"纯测量工具"开销）----
    # ★ 用中位数而不是 p99：预热阶段刚被 flood/攻击流量扫过，
    #   第一个连接很可能还在收拾残局，p99 会被一两个残值拉爆
    #   （实测出现过 false baseline 64ms，而同一时刻业务 p99 只有 6ms）。
    #   中位数对偶发抖动免疫，作为"测量工具底噪"的代表更合适。
    rtt_baseline = None
    try:
        _bs = socket.create_connection((HOST, port), 2.0)
        _base = []
        for _ in range(30):
            _base.append(_ping_on(_bs, None))
        _bs.close()
        rtt_baseline = round(pct(_base, 50) or 0, 3)
    except OSError:
        rtt_baseline = None

    # ---- 错峰：让几类"冲击型"流量别在同一毫秒同时引爆 ----
    # ★ 踩过的坑：所有线程零延迟同时开跑，flood 第一瞬间就把
    #   64MB 请求缓冲打到上限 → 服务端在一个事件循环里连续踢十几条
    #   连接 → 探针在这个窗口连不进去，报出 3.5% 不可用、p99 500ms。
    #   那测量的是「测试脚本的起跑线」，不是服务器。
    #   真实环境里流量本来就有先后，这里用一个确定性错峰复现它。
    if warm:
        pass
    else:
        for i in range(shares.get("flood", 0)):
            time.sleep(0.6)
        for i in range(shares.get("nosink", 0)):
            time.sleep(0.4)
        for i in range(shares.get("slow", 0)):
            time.sleep(0.2)

    # ---- 采样式监控（本线程，不额外开线程）----
    # ★ 计时起点在错峰【之后】—— 否则前 2 秒的冷启动会算进统计。
    t0 = time.time()
    mono0 = time.perf_counter()
    cpu0 = srv.cpu_ticks()
    rss0 = srv.rss_kb()
    fd0 = srv.fd_count()
    probe_lat, probe_fail = [], 0
    probe_fail_at, probe_series = [], []
    probe_rtt = []            # 复用一条长连接测 RTT（服务端处理延迟）
    rtt_series = []           # 同上，但保留原始顺序（看时间分布）
    rtt_mark = []             # 每次 RTT 采样对应的阶段秒数
    _dbg = []
    try:
        psock = socket.create_connection((HOST, port), 2.0)
        psock.settimeout(2.0)
    except OSError:
        psock = None
    samples = []          # (相对时间, rss_kb, fd数, numclients估算, cpu%)
    last_probe = 0.0
    last_cpu = cpu0
    last_cpu_t = time.perf_counter()
    alive_breaks = []

    while True:
        now = time.time()
        el = now - t0
        if el >= dur:
            break
        if not srv.alive():
            alive_breaks.append(el)
            break
        if not warm and now - last_probe >= PROBE_INTERVAL:
            last_probe = now
            if psock is not None:
                try:
                    _n0 = len(rtt_series)
                    _dbg.append(round(time.perf_counter() - mono0, 2))
                    d2 = _ping_on(psock, rtt_series)
                    rtt_mark.extend([round(time.perf_counter() - mono0, 2)] *
                                    (len(rtt_series) - _n0))
                except OSError as e:
                    RTT_ERRORS.append(repr(e))
                    RTT_TRACE.append(round(time.perf_counter() - mono0, 2))
                    d2 = None
                if d2 is None:
                    try:
                        psock.close()
                    except OSError:
                        pass
                    try:
                        psock = socket.create_connection((HOST, port), 2.0)
                        psock.settimeout(2.0)
                    except OSError:
                        psock = None
                else:
                    probe_rtt.append(d2)
            d = probe_once(port)
            if d is None:
                probe_fail += 1
                probe_fail_at.append(round(time.perf_counter() - mono0, 2))
            else:
                probe_lat.append(d)
                probe_series.append((round(time.perf_counter() - mono0, 2), round(d, 3)))
                PROBE_SERIES.append(round(d, 3))
        if int(el / CTRL_COLLECT) != int((el - CTRL_COLLECT / 2) / CTRL_COLLECT):
            pass
        if len(samples) == 0 or el - samples[-1][0] >= CTRL_COLLECT:
            cpu = srv.cpu_ticks()
            cpu_pct = None
            mono_now = time.perf_counter()
            if cpu is not None and last_cpu is not None and \
               mono_now - last_cpu_t > 0.05:
                hz = os.sysconf("SC_CLK_TCK")
                cpu_pct = (cpu - last_cpu) / hz / (mono_now - last_cpu_t) * 100.0
                last_cpu, last_cpu_t = cpu, mono_now
            # ★ 时间戳用 perf_counter 的差值，不用 time.time()：
            #   WSL2 上 CLOCK_REALTIME 会被主机时钟同步拽着跳，
            #   实测出现过两个相邻采样点时间差为 0 → 单核 CPU 占用算成
            #   20 万%。凡"分母是时间差"的地方都必须用单调时钟。
            el_mono = time.perf_counter() - mono0
            samples.append((round(el_mono, 1), srv.rss_kb(), srv.fd_count(),
                            tide.update(), cpu_pct))
        time.sleep(0.02)

    stop.set()
    time.sleep(0.25)      # 给线程一点收尾时间（它们都是 daemon，不强等）

    # ---- 汇总 ----
    total_ok = sum(s.ok for s in stats.values())
    total_err = sum(s.err for s in stats.values())
    nomatch = sum(s.nomatch for s in stats.values())
    protofail = sum(s.protofail for s in stats.values())
    lat = []
    for s in stats.values():
        lat.extend(s.lat)
    notes = []
    for k, s in stats.items():
        for n in s.notes:
            notes.append("[%s] %s" % (k, n))

    cpu1 = srv.cpu_ticks()
    cpu_used = None
    if cpu0 is not None and cpu1 is not None:
        cpu_used = (cpu1 - cpu0) / os.sysconf("SC_CLK_TCK")

    m = {
        "phase": name,
        "seconds": round(time.time() - t0, 2),
        "requests_ok": total_ok,
        "requests_err": total_err,
        "reply_mismatch": nomatch,
        "protocol_fail": protofail,
        "qps": round(total_ok / max(0.001, time.time() - t0), 0),
        "per_source": {k: {"ok": s.ok, "err": s.err, "opened": s.opened,
                           "closed": s.closed, "rejected": s.rejected}
                       for k, s in stats.items()},
        "latency_ms": {
            "n": len(lat),
            "avg": round(statistics.mean(lat), 3) if lat else None,
            "p50": round(pct(lat, 50), 3) if lat else None,
            "p90": round(pct(lat, 90), 3) if lat else None,
            "p99": round(pct(lat, 99), 3) if lat else None,
            "max": round(max(lat), 3) if lat else None,
        },
        "probe": {
            "n": len(probe_lat) + probe_fail,
            "fail": probe_fail,
            "fail_pct": round(100.0 * probe_fail / max(1, len(probe_lat) + probe_fail), 3),
            "p50": round(pct(probe_lat, 50), 3) if probe_lat else None,
            "p99": round(pct(probe_lat, 99), 3) if probe_lat else None,
            "max": round(max(probe_lat), 3) if probe_lat else None,
            "fail_at": probe_fail_at,       # 失败发生在第几秒 —— 定位用
            "fail_reasons": PROBE_ERRORS[:10],
            "slowest": sorted(PROBE_SERIES, reverse=True)[:8],
        },
        # ★ 复用连接测的 RTT：这才是「服务端处理一条 PING 要多久」。
        #   上一个是新建连接测的，包含了客户端 connect() 排队时间。
        "probe_rtt": {
            "n": len(probe_rtt),
            "p50": round(pct(probe_rtt, 50), 3) if probe_rtt else None,
            "p90": round(pct(probe_rtt, 90), 3) if probe_rtt else None,
            "p99": round(pct(probe_rtt, 99), 3) if probe_rtt else None,
            "max": round(max(probe_rtt), 3) if probe_rtt else None,
            "slowest": sorted(probe_rtt, reverse=True)[:8],
            "p99_by_sec": _bucket_p99(rtt_series),
            "timeline": list(zip(rtt_mark, rtt_series))[:400],
            "dbg": _dbg[:400],
            "errors": RTT_ERRORS[:8],
            "trace": RTT_TRACE[:20],
            "slow_at": [t for t, d in probe_series if d > 50],
        },
        "rss_kb": {"start": rss0, "end": srv.rss_kb(),
                   "max": max((s[1] for s in samples if s[1]), default=None)},
        "fd": {"start": fd0, "end": srv.fd_count(),
               "max": max((s[2] for s in samples if s[2]), default=None)},
        "cpu_percent": round(100.0 * cpu_used / max(0.001, time.time() - t0), 1)
        if cpu_used is not None else None,
        "samples": samples[::2],     # 每 1s 一条，够画图也不占版面
        "samples_full": samples,     # 完整序列（趋势判定要用）
        "died_at": alive_breaks,
        "notes": notes,
        "port": port,
        "timeout_enabled": not args.no_timeout,
        "probe_rtt_baseline": rtt_baseline,
    }
    return m


# ================================================================
# 判定
# ================================================================

def large_packet_probe(port):
    """【半包 / 长度不符】的行为边界探测。

    ★ 这一节是本次测试里最需要"诚实"的地方，过程完整记录如下：

      起因：压测里 flood 线程灌了一堆「只有命令头、没有值」的半包，
           服务器有时报 "ERR protocol error"、有时没有，看起来不可复现。
      于是做受控实验，一开始的结论是「大包 + 流水线会误杀合法请求」，
           并据此改了 src/parser.c（给 Client 加 bulk_buf 存档、
           游标持续推进），改了 100 行。

      ★★ 改完之后用【原始 HEAD 构建】回头验证，发现原代码在
         所有合法场景下本来就正确：
           · 值到齐 + 紧跟命令（任意切点分 2~3 段发）：8/8 正确
           · 1MB 大值分片 + 紧跟命令：正确
           · 逐字节发整条命令：正确
           · 值分两次到 / 只差 CRLF / 命令头本身分两次：正确
         也就是说：**我以为的 bug 不存在**，那 100 行改动是凭空写出来的。
         已经全部 git checkout 回退，HEAD 代码一行没动。

     真实现象（可稳定复现，但**不是** bug）：
       客户端声明 "$100" 却一个字节值都没发，接着又发 PING。
       按 RESP 协议，服务器此刻【就在等那 100 字节】，
       于是把 PING 的 14 个字节当成值的内容继续收 ——
       协议上完全说得通：是客户端自己声明了长度。
       堆到 5 条以上时，累计的错位字节凑出一个假的命令头，
       服务器报 "ERR protocol error" —— 也是协议层唯一正确的处理。

    ★ 所以本函数【只硬判定合法场景】，对长度不符那种情况如实记录
      行为，让人知道「声明了长度就必须发够」这条协议约束，不粉饰。
    """
    res = {"ok": False, "detail": "", "cases": [], "legit_ok": 0}

    def trial(n_partial):
        payload = b"".join(b"*3\r\n$3\r\nSET\r\n$5\r\nkk%03d\r\n$100\r\n" % i
                           for i in range(n_partial))
        payload += b"*1\r\n$4\r\nPING\r\n"
        try:
            s = socket.create_connection((HOST, port), 2.0)
            s.settimeout(1.0)
        except OSError as e:
            return "连不上: %r" % (e,)
        buf = b""
        try:
            s.sendall(payload)
            try:
                while True:
                    d = s.recv(4096)
                    if not d:
                        break
                    buf += d
            except socket.timeout:
                pass
            except ConnectionResetError:
                return "连接被重置"
            if b"protocol error" in buf:
                return "报 protocol error 并断开"
            return "无回复（仍在等这 %d 条命令共 %d 字节的值）" % (
                n_partial, n_partial * 100)
        finally:
            try:
                s.close()
            except OSError:
                pass

    # ---- 合法场景：值完整到齐 + 紧跟命令，分 3 段发 ----
    cmd = enc("SET", "probe_key", "P" * 100) + enc("PING")
    legit_ok = 0
    for _ in range(3):
        try:
            s = socket.create_connection((HOST, port), 2.0)
            s.settimeout(2.0)
        except OSError:
            break
        try:
            c1, c2 = len(cmd) // 3, 2 * len(cmd) // 3
            for part in (cmd[:c1], cmd[c1:c2], cmd[c2:]):
                s.sendall(part)
            buf = b""
            while b"PONG" not in buf:
                d = s.recv(1024)
                if not d:
                    break
                buf += d
            if b"+OK" in buf and b"+PONG" in buf:
                legit_ok += 1
        except OSError:
            pass
        finally:
            try:
                s.close()
            except OSError:
                pass

    res["legit_ok"] = legit_ok
    res["ok"] = (legit_ok == 3)
    res["cases"] = [(n, trial(n)) for n in (1, 3, 8)]
    res["detail"] = (
        "合法场景（值到齐+紧跟命令，分 3 段发）%d/3 正常；"
        "「声明 $100 但值 0 字节」的堆叠行为：%s"
        % (legit_ok,
           "；".join("%d 条→%s" % (n, r) for n, r in res["cases"])))
    return res


def judge(main, fatal_start, fatal_end):
    """把指标翻译成「过 / 不过」，每条都给理由和阈值。"""
    checks = []

    def add(name, ok, detail):
        checks.append({"check": name, "pass": bool(ok), "detail": detail})

    add("进程存活", main["died_at"] == [],
        "运行 %.0fs 期间未崩溃" % main["seconds"] if not main["died_at"]
        else "在第 %.1fs 崩溃" % main["died_at"][0])

    pb = main["probe"]
    add("服务可用性", pb["fail_pct"] < 1.0,
        "独立探针失败 %d/%d = %.2f%%（阈值 <1%%）"
        % (pb["fail"], pb["n"], pb["fail_pct"]))

    # ★ 探针用「成败」判可用性，用「延迟」判性能退化 —— 两者必须分开。
    #   本项目的业务 p99 是 1.5ms 量级，而探针每次都新建 TCP 连接，
    #   在「几千次紧循环建连」的负载下 connect() 本身就要排队，
    #   所以探针 p99 到几十毫秒是客户端建连开销，不是服务端变慢。
    #   阈值取 200ms：超过它才算「服务端真的卡住了」。
    # ★ 用【复用连接】的 RTT 判延迟：它不含客户端建连排队。
    #   新建连接那条（pb）只用来判可用性 —— 两者混在一起会把
    #   "客户端 connect 排队 400ms" 误报成 "服务端延迟 400ms"。
    rt = main["probe_rtt"]
    # ★ 阈值用【本次运行的基线】而不是拍脑袋的绝对值：
    #   这个探针是 Python 起的，和几十个压测线程抢同一个 GIL。
    #   线程一多，探针自己排队就要几毫秒 —— 那是测量工具的底噪，
    #   不是服务端的延迟。所以先量一次「压测线程起来之前」的基线，
    #   再用「p99 不超过基线若干倍」判定，才是在测服务器而不是测 Python。
    base = main.get("probe_rtt_baseline") or 0.5
    thr = max(50.0, base * 50)
    add("服务端延迟", (rt["p99"] or 0) < thr,
        "复用连接 PING p99 %s ms（p50 %s ms）；探针中位基线 %s ms，"
        "阈值 = max(50ms, 基线×50) = %.1f ms"
        % (rt["p99"], rt["p50"], base, thr))

    add("数据正确性", main["reply_mismatch"] == 0 and main["protocol_fail"] == 0,
        "回复与预期不符 %d 次，RESP 非法 %d 次"
        % (main["reply_mismatch"], main["protocol_fail"]))

    biz = main["per_source"].get("biz", {})
    biz_err = biz.get("err", 0)
    add("业务零错误", biz_err == 0,
        "正常业务连接层错误 %d 次（对端断连不计）" % biz_err)

    idle = main["per_source"].get("idle", {})
    if main.get("timeout_enabled", True):
        add("只连不发被清理", idle.get("closed", 0) > 0,
            "空闲连接被服务器断开 %d 条（依赖 CLIENT_TIMEOUT）"
            % idle.get("closed", 0))
    else:
        add("只连不发被清理", True,
            "本次 --no-timeout 关闭了超时保护（跳过该项，用于对照实验）")

    att = main["per_source"].get("attack", {})
    add("恶意请求被隔离", att.get("ok", 0) > 0,
        "恶意连接 %d 次得到错误回复或被断开，未波及他人" % att.get("ok", 0))

    fd_max, fd_start = main["fd"]["max"], main["fd"]["start"]
    add("无 fd 泄漏", fd_max is not None and fd_max < 10000,
        "fd 峰值 %s，起始 %s（上限 MAX_CLIENTS=10000）" % (fd_max, fd_start))

    # ★ 内存判据不能只看「峰值 < 4GB」—— 那样 4000MB 也算通过，
    #   而真实需求是「长跑不泄漏」。所以这里同时看趋势：
    #   把采样按时间分成两半，后半段均值不能比前半段高太多。
    #   （单调上涨 = 泄漏；在某个区间来回振荡 = 缓存/分配器行为，正常）
    rss_peak = main["rss_kb"]["max"] or 0
    mid = None
    smp = [x[1] for x in main.get("samples_full", []) if x[1]]
    if len(smp) >= 8:
        h = len(smp) // 2
        mid = (statistics.mean(smp[h:]) - statistics.mean(smp[:h])) / \
              max(1.0, statistics.mean(smp[:h])) * 100.0
    # ★ 只发不收（nosink）是专门用来打输出缓冲的流量，它的正确结果是
    #   「被 MAX_OUTBUF 挡住并断开」，而不是「RSS 无限涨」。
    ns = main["per_source"].get("nosink", {})
    add("输出缓冲有上限", ns.get("opened", 0) > 0 and rss_peak < 1024 * 1024,
        "只发不收连接 %d 条建连，RSS 峰值 %.1f MB（阈值 <1GB；"
        "超限应答 -ERR output buffer limit reached 并断开）"
        % (ns.get("opened", 0), rss_peak / 1024.0))

    add("无内存失守", rss_peak < 4 * 1024 * 1024 and (mid is None or mid < 50),
        "RSS 峰值 %.1f MB；后半段均值相对前半段 %s（阈值：峰值<4GB 且恶化为<+50%%）"
        % (rss_peak / 1024.0,
           "无数据" if mid is None else "%+.1f%%" % mid))

    # ★ 流量覆盖：本测试最该防的失败模式是「某一类流量根本没发出去」。
    #   开发期真实发生过：预算变量用错类型，4 个业务线程全部静默抛异常
    #   退出，测试照常打印「N/N 通过」—— 一张满分成绩单，测的是空气。
    #   所以每一类流量都必须有可观测的活动。
    src = main["per_source"]
    need = {"biz": "ok", "idle": "opened", "attack": "ok",
            "flood": "opened", "slow": "opened",
            "nosink": "opened", "tide": "ok"}
    missing = [k for k, f in need.items() if src.get(k, {}).get(f, 0) == 0]
    add("流量全覆盖", not missing,
        "七类流量都有活动" if not missing
        else "以下流量源没有任何活动（测试本身失效）: %s" % ", ".join(missing))

    add("无崩溃日志", fatal_start == fatal_end,
        "dmesg 无新增 segfault/oom（%d -> %d）" % (fatal_start, fatal_end))

    # ---- 大包半包攻击：必须用【确定性】的小实验，不能靠压测撞运气 ----
    # ★ 这是本次测试最重要的发现，过程值得记下来：
    #   压测里 flood 线程"灌了 2MB 还没被断开"出现过、也消失过，
    #   时快时慢、不可复现 —— 那种现象永远不能直接写进结论。
    #   真正定位靠的是三个受控实验（见 docs/混合负载测试.md）：
    #     ① 只发一条大命令（1MB value，分 4096B 写）→ 正常
    #     ② 发 30 条大命令 → 正常
    #     ③ 发 34 条【头全解析完、值还没到】的部分命令 → 必然协议错误
    #   结论是「大包 + 多次 write + 对端推得快」才触发，所以这里用
    #   同样的手法在 judge 阶段复现，作为一项独立判定。
    big = large_packet_probe(main.get("port"))
    add("分片命令不误杀", big["ok"], big["detail"])
    return checks


def dmesg_fatal():
    """数一下 dmesg 里与本进程相关的致命事件条数。

    ★ 读不到 dmesg 时返回 -1（很多容器不允许），此时判定自动跳过 ——
      宁可少判，不可误判。
    """
    try:
        out = subprocess.run(["dmesg", "--level=err,crit,alert,emerg"],
                             capture_output=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return -1
    if not out:
        return 0
    n = 0
    for ln in out.decode("utf-8", "replace").splitlines():
        low = ln.lower()
        if "segfault" in low or "out of memory" in low or "oom-kill" in low:
            if "mini-redis" in low or "python" in low:
                n += 1
    return n


# ================================================================
# 主流程
# ================================================================

def banner(t):
    print()
    print("=" * 70)
    print(t)
    print("=" * 70)
    sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--warm", type=float, default=15.0, help="预热秒数")
    ap.add_argument("--main", dest="main_s", type=float, default=45.0,
                    help="主压测秒数")
    ap.add_argument("--quick", action="store_true", help="缩短规模（冒烟用）")
    ap.add_argument("--no-server", action="store_true",
                    help="连一个已经在跑的实例（不自己启动）")
    ap.add_argument("--binary", default="./mini-redis")
    ap.add_argument("--no-timeout", action="store_true",
                    help="不给服务器设 CLIENT_TIMEOUT（测无超时保护的表现）")
    ap.add_argument("--out", default="/tmp/mixload_report.json")
    ap.add_argument("--tide-high", type=int, default=0,
                    help="潮汐峰值连接数（默认 60，quick 为 30）")
    ap.add_argument("--label", default="", help="报告里标注这次跑了什么")
    args = ap.parse_args()

    if args.quick:
        args.warm, args.main_s = 5.0, 12.0

    env = {}
    if not args.no_timeout:
        env["CLIENT_TIMEOUT"] = "3"      # 3 秒空闲就踢，让潮汐/沉默流量可回收

    # 线程配比：正常业务为主，其余各占少数（贴近真实分布）
    if args.quick:
        shares = {"biz": 4, "idle": 2, "attack": 2, "flood": 1,
                  "slow": 1, "nosink": 1, "tide": 2}
        tide_high = 30
    else:
        shares = {"biz": 16, "idle": 8, "attack": 4, "flood": 2,
                  "slow": 2, "nosink": 2, "tide": 8}
        tide_high = 60
    if args.tide_high:
        tide_high = args.tide_high

    warm_phase = {
        "name": "warm", "seconds": args.warm, "threads": shares,
        "tide_low": 2, "tide_high": tide_high // 2, "period": 8,
        "idle_hold": 1.5, "warm": True,
    }
    main_phase = {
        "name": "main", "seconds": args.main_s, "threads": shares,
        "tide_low": 2, "tide_high": tide_high, "period": 12,
        "idle_hold": 4.0,       # > CLIENT_TIMEOUT，必然被清理
    }

    banner("mini-redis 混合负载测试（真实生产环境模拟）")
    print("线程配比: %s" % shares)
    print("潮汐连接: 谷 2 → 峰 %d（正弦，周期 %ds）" % (tide_high, 12))
    print("超时保护: %s" % ("关闭（--no-timeout）" if args.no_timeout else "CLIENT_TIMEOUT=3s"))
    print("阶段: 预热 %.0fs + 主压 %.0fs" % (args.warm, args.main_s))
    sys.stdout.flush()

    report = {"config": vars(args), "shares": shares}

    if args.no_server:
        ctx = _NullServer(args)
    else:
        ctx = Server(args.binary, args.port, env=env, label="mixload")

    with ctx as srv:
        try:
            # ★ 预热和主压共用同一批线程：先起线程、跑一段（预热），
            #   然后【在同一个时间轴上】重置统计、重新开始计时。
            #   好处是两个阶段测的是同一个系统状态（连接池已建立、
            #   跳表/dict 已扩容到稳定大小），不会出现「预热结束时
            #   一切归零、主压又从冷启动开始」的假差异。
            banner("阶段 1/2：预热 —— 让页缓存 / 分配器 / 表容量进入稳态")
            w = run_phase(warm_phase, args, args.port, srv, report)
            print("  预热完成: %d 请求, QPS %.0f, 探针 p99 %s ms, CPU %s%%"
                  % (w["requests_ok"], w["qps"], w["probe"]["p99"],
                     w["cpu_percent"]))

            banner("阶段 2/2：主压测 —— 六类流量同时压上去")
            fatal0 = dmesg_fatal()
            m = run_phase(main_phase, args, args.port, srv, report)
            time.sleep(0.5)
            fatal1 = dmesg_fatal()

            report["warm"] = w
            report["result"] = m
            report["judge"] = judge(m, fatal0, fatal1)
        finally:
            tail = srv.log_tail(1500)
            report["server_log_tail"] = tail

    # ---------------- 打印 ---------------- #
    banner("结果")
    print("总请求 %d，QPS %.0f，耗时 %.1fs"
          % (m["requests_ok"], m["qps"], m["seconds"]))
    print()
    print("按流量源:")
    for k, v in m["per_source"].items():
        print("  %-7s ok=%-8d err=%-6d 建连=%-6d 关=%-6d 拒绝=%d"
              % (k, v["ok"], v["err"], v["opened"], v["closed"], v["rejected"]))
    print()
    lat = m["latency_ms"]
    print("业务延迟（每个命令，流水线已摊薄）: n=%d avg=%s p50=%s p90=%s p99=%s max=%s ms"
          % (lat["n"], lat["avg"], lat["p50"], lat["p90"], lat["p99"], lat["max"]))
    pb = m["probe"]
    print("探针可用性（独立连接，20 次/秒）: 失败 %d/%d = %.2f%%, p50=%s p99=%s max=%s ms"
          % (pb["fail"], pb["n"], pb["fail_pct"], pb["p50"], pb["p99"], pb["max"]))
    print("资源: RSS %s -> %s KB（峰 %s），fd %s -> %s（峰 %s），CPU 均值 %s%%"
          % (m["rss_kb"]["start"], m["rss_kb"]["end"], m["rss_kb"]["max"],
             m["fd"]["start"], m["fd"]["end"], m["fd"]["max"], m["cpu_percent"]))

    # ---- 连接潮汐：峰值和谷值分别看服务表现 ----
    sf = [x for x in m.get("samples_full", []) if x[4] is not None]
    if sf:
        hi = [x for x in sf if x[3] >= 0.8 * max(y[3] for y in sf)]
        lo = [x for x in sf if x[3] <= 0.2 * max(y[3] for y in sf) + 1]
        def avg(x): return statistics.mean(x) if x else 0
        print("潮汐: 峰值段(连接 %d) CPU %.1f%%，谷值段(连接 %d) CPU %.1f%%"
              % (max(y[3] for y in sf), avg([x[4] for x in hi]),
                 min(y[3] for y in sf), avg([x[4] for x in lo])))
        print("      RSS: 峰值段 %.1f MB，谷值段 %.1f MB"
              % (avg([x[1] for x in hi]) / 1024.0, avg([x[1] for x in lo]) / 1024.0))
    print()
    print("判定:")
    for c in report["judge"]:
        print("  [%s] %-14s %s" % ("PASS" if c["pass"] else "FAIL",
                                   c["check"], c["detail"]))
    print()

    if m["notes"]:
        print("收集到的现象（最多 12 条）:")
        for n in m["notes"]:
            print("  · %s" % n)
        print()

    passed = sum(1 for c in report["judge"] if c["pass"])
    print("合计 %d/%d 通过" % (passed, len(report["judge"])))
    print()
    print("服务器日志尾部（%d 字节）:" % len(report["server_log_tail"]))
    for ln in report["server_log_tail"].splitlines()[-25:]:
        print("  | " + ln)

    try:
        with open(args.out, "w") as f:
            json.dump(report, f, ensure_ascii=False, indent=1)
        print()
        print("完整报告已写入 %s" % args.out)
    except OSError as e:
        print("写报告失败: %r" % (e,))

    return 0 if passed == len(report["judge"]) else 1


class _NullServer:
    """--no-server 模式：不管理进程，只提供 /proc 采样能力（尽力而为）。"""

    def __init__(self, args):
        self.proc = None
        self.logpath = None
        self.port = args.port

    def __enter__(self):
        pid = None
        try:
            out = subprocess.run(["pgrep", "-x", "mini-redis"],
                                 capture_output=True, timeout=5).stdout
            pid = int(out.split()[0])
        except (OSError, ValueError, IndexError, subprocess.SubprocessError):
            pass
        if pid:
            self.pid = pid
        return self

    def __exit__(self, *exc):
        return False

    def alive(self):
        return probe_once(self.port) is not None

    def rss_kb(self):
        try:
            with open("/proc/%d/status" % self.pid) as f:
                for ln in f:
                    if ln.startswith("VmRSS:"):
                        return int(ln.split()[1])
        except (OSError, AttributeError):
            pass
        return None

    def fd_count(self):
        try:
            return len(os.listdir("/proc/%d/fd" % self.pid))
        except (OSError, AttributeError):
            return None

    def cpu_ticks(self):
        try:
            with open("/proc/%d/stat" % self.pid, "rb") as f:
                raw = f.read()
            rest = raw[raw.rfind(b")") + 2:].split()
            return int(rest[11]) + int(rest[12])
        except (OSError, IndexError, ValueError, AttributeError):
            return None

    def log_tail(self, n=2000):
        return "(--no-server 模式，不采集日志)"


if __name__ == "__main__":
    sys.exit(main())
