#!/usr/bin/env python3
"""与客户机 agent 的 TCP 旁路通道直接收发（同一套帧协议）。
用法:
  agentctl.py listen [秒]            只收（等价 serial-read）
  agentctl.py call <methodID> [callID]  发一个 Call 帧（等价 serial-write）
  agentctl.py raw <hex 字节>          发原始字节
  agentctl.py handshake              发 type=3 Handshake（用逆向出的两个必需串）
环境: HVM_AGENT_HOST（默认 172.16.100.2） HVM_AGENT_TCP_PORT（默认 20001）
"""
import os, socket, sys, time

HOST = os.environ.get("HVM_AGENT_HOST", "172.16.100.2")
PORT = int(os.environ.get("HVM_AGENT_TCP_PORT", "20001"))
MODE = "guest-event-connection:"
NAME = "serialClient"


def vu(n):
    o = bytearray()
    while True:
        b = n & 0x7F; n >>= 7; o.append(b | (0x80 if n else 0))
        if not n: return bytes(o)


def f_varint(f, v): return vu((f << 3) | 0) + vu(v)
def f_bytes(f, v): return vu((f << 3) | 2) + vu(len(v)) + v
def f_str(f, v): return f_bytes(f, v.encode())


def frame(payload, mtype):
    n = len(payload); h = bytearray(7)
    h[0] = (n >> 24) & 0xFF; h[1] = (n >> 16) & 0xFF; h[2] = (n >> 8) & 0xFF
    h[3] = n & 0xFF; h[4] = mtype & 0xFF
    s = (h[0]+h[1]+h[2]+h[3]+h[4]) & 0xFFFF
    h[5] = (s >> 8) & 0xFF; h[6] = s & 0xFF
    return bytes(h) + payload


def enc_call(mid, cid=0): return (f_varint(1, cid) if cid else b"") + f_varint(2, mid)
def enc_handshake(): return f_str(1, MODE) + f_str(2, NAME)


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "listen"
    s = socket.create_connection((HOST, PORT), timeout=8)
    print(f"已连接 {HOST}:{PORT}", flush=True)
    if cmd == "listen":
        secs = float(sys.argv[2]) if len(sys.argv) > 2 else 8
        s.settimeout(secs); total = 0
        try:
            while True:
                d = s.recv(65536)
                if not d: print("对端关闭"); break
                total += len(d)
                print(f"← 收到 {len(d)} 字节: {d[:48].hex(' ')}", flush=True)
        except socket.timeout:
            print(f"（{secs} 秒内共收到 {total} 字节）")
    elif cmd == "call":
        mid = int(sys.argv[2]); cid = int(sys.argv[3]) if len(sys.argv) > 3 else 0
        f = frame(enc_call(mid, cid), 1)
        s.sendall(f); print(f"→ 已发送 Call{{methodID={mid}}} {len(f)} 字节: {f.hex(' ')}")
        s.settimeout(3)
        try:
            d = s.recv(4096); print(f"← 应答: {d.hex(' ')}")
        except socket.timeout:
            print("← 3 秒内无应答")
    elif cmd == "handshake":
        f = frame(enc_handshake(), 3)
        s.sendall(f); print(f"→ 已发送 Handshake {len(f)} 字节: {f.hex(' ')}")
    elif cmd == "raw":
        f = bytes.fromhex(sys.argv[2].replace(" ", ""))
        s.sendall(f); print(f"→ 已发送 {len(f)} 字节")
    s.close()


if __name__ == "__main__":
    main()
