#!/usr/bin/env python3
"""正确性探针：对比 sds 版与 C 字符串(旧)版的能力差异。
用法: python3 correctness_probe.py <binary>
在独立端口运行 binary，测试：
  1) 二进制值（含 \0）能否存取
  2) 二进制 zset member 能否存取
  3) 超过 1KB 的值能否存取
  4) 16-31 字节（TYPE_5 边界）字符串是否正确
输出每项 PASS/FAIL。
"""
import socket, sys, subprocess, time

BIN = sys.argv[1]
import os
# 用 PID 派生唯一端口，避免与其它探针/残留服务器冲突
PORT = 6600 + (os.getpid() % 100)

def start():
    # 先杀掉可能占住该端口的残留服务器
    subprocess.run(["pkill", "-f", f"mini-redis {PORT}"], capture_output=True)
    time.sleep(0.2)
    p = subprocess.Popen([BIN, str(PORT)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(120):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=0.3); s.close(); return p
        except OSError: time.sleep(0.05)
    p.kill(); raise RuntimeError("not ready")

def rx(s, n):
    buf = b''; s.settimeout(3)
    try:
        while len(buf) < n:
            d = s.recv(65536)
            if not d: break
            buf += d
    except socket.timeout: pass
    return buf

def pipelined(payload, expect):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=6)
    s.sendall(payload)
    got = rx(s, len(expect))
    s.close()
    return got == expect, got

def report(name, ok, got=None):
    print(("  [PASS] " if ok else "  [FAIL] ") + name + ("" if ok else (" got=%r" % (got[:40] if got else got))))

def main():
    print(f"== correctness probe: {BIN} ==")
    p = start()
    npass = nfail = 0
    def rep(name, cond, got=None):
        nonlocal npass, nfail
        if cond: npass += 1
        else: nfail += 1
        report(name, cond, got)

    # 1. 二进制值（含 \0）: SET k A\x00xy (4 字节) + GET
    ok, got = pipelined(
        b"*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$4\r\nA\x00xy\r\n*2\r\n$3\r\nGET\r\n$1\r\nk\r\n",
        b"+OK\r\n$4\r\nA\x00xy\r\n")
    rep("binary value (\\0) roundtrip", ok, got)

    # 2. 16 字节 key（TYPE_5 边界）
    k16 = b"k"*16
    ok, got = pipelined(
        b"*3\r\n$3\r\nSET\r\n$16\r\n"+k16+b"\r\n$1\r\nx\r\n*2\r\n$3\r\nGET\r\n$16\r\n"+k16+b"\r\n",
        b"+OK\r\n$1\r\nx\r\n")
    rep("16-byte key roundtrip", ok, got)

    # 3. 32 字节 key（TYPE_8 边界）
    k32 = b"k"*32
    ok, got = pipelined(
        b"*3\r\n$3\r\nSET\r\n$32\r\n"+k32+b"\r\n$1\r\nx\r\n*2\r\n$3\r\nGET\r\n$32\r\n"+k32+b"\r\n",
        b"+OK\r\n$1\r\nx\r\n")
    rep("32-byte key roundtrip", ok, got)

    # 4. 大值 >1KB（旧版固定 1024 缓冲区会截断）
    v2k = b"x"*2000
    ok, got = pipelined(
        b"*3\r\n$3\r\nSET\r\n$3\r\nbig\r\n$2000\r\n"+v2k+b"\r\n*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n",
        b"+OK\r\n$2000\r\n"+v2k+b"\r\n")
    rep("2000B value (超1KB低缓冲)", ok, got)

    # 5. 64KB 值
    v64k = b"y"*65536
    ok, got = pipelined(
        b"*3\r\n$3\r\nSET\r\n$4\r\nbig2\r\n$65536\r\n"+v64k+b"\r\n*2\r\n$3\r\nGET\r\n$4\r\nbig2\r\n",
        b"+OK\r\n$65536\r\n"+v64k+b"\r\n")
    rep("64KB value roundtrip", ok, got)

    # 6. 二进制 zset member
    ok, got = pipelined(
        b"*4\r\n$4\r\nZADD\r\n$2\r\nzs\r\n$4\r\n42.5\r\n$3\r\nm\x00k\r\n*3\r\n$6\r\nZSCORE\r\n$2\r\nzs\r\n$3\r\nm\x00k\r\n",
        b"+OK\r\n$4\r\n42.5\r\n")
    rep("binary zset member", ok, got)

    p.kill()
    print(f"-> {npass} pass, {nfail} fail")
    return nfail

if __name__ == "__main__":
    sys.exit(main())
