#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""vm_manager 客户机侧最小 guest-agent（跑在客户机里，对 /dev/vport2p1 收发）。

协议（全部由服务端 libvm_manager.z.so 逆向 + 内嵌 protobuf 描述符解出，见
docs/serial-agent-protocol.md）：

  帧 = [b0 b1 b2 b3]  大端 32 位【载荷长度】
       [b4]           消息类型（接收侧 csel: min(b4,3)，即枚举 0..3）
       [b5 b6]        校验和 = (b0+b1+b2+b3+b4) 的高字节、低字节
       [载荷]         protobuf 序列化

  信封 Guest.protobuf.link.Call   { 1: uint32 callID, 2: uint32 methodID,
                                    3: bytes parameters, 4: bool async }
  应答 Guest.protobuf.link.Result { 1: uint32 callID, 2: bytes response, 3: int32 errorCode }
  Guest.protobuf.GuestAgent.Bool   { 1: bool value }
  Guest.protobuf.GuestAgent.String { 1: string value }
  Guest.protobuf.link.Handshake    { 1: string connectionMode, 2: string clientName,
                                     3: bytes authString }

主机侧方法号（SendToGuest::xxx 里 CallMethodSync 的 w1）：
  AgentServiceAlive = 27   → 回 Bool{value:true}    （这直接决定虚拟机状态能否变成 1）
  GetIpv4Address    = 40   → 回 String{value:"<ip>"}
  其余一律回 Void（空 Result.response）
"""

import os
import sys
import time
import struct
import select

PORT = os.environ.get("HVM_AGENT_PORT", "/dev/vport2p1")
LOG = os.environ.get("HVM_AGENT_LOG", "/var/log/hvm-serial-agent.log")
CLIENT_NAME = os.environ.get("HVM_AGENT_NAME", "debian-guest")
CONNECTION_MODE = os.environ.get("HVM_AGENT_MODE", "1")
GUEST_IP = os.environ.get("HVM_AGENT_IP", "172.16.100.2")

MT_CALL = 0          # 发送 Call 用的消息类型（0..3，待实测确认，可用环境变量覆盖）
MT_RESULT = 1        # 发送 Result 用的消息类型


def log(msg):
    line = "%s %s\n" % (time.strftime("%Y-%m-%dT%H:%M:%S"), msg)
    try:
        with open(LOG, "a") as f:
            f.write(line)
    except Exception:
        pass
    try:
        sys.stderr.write(line)
    except Exception:
        pass


# ---------------------------------------------------------------- protobuf 编码
def vu(n):                       # varint
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def tag(field, wire):
    return vu((field << 3) | wire)


def f_varint(field, value):
    return tag(field, 0) + vu(value)


def f_bytes(field, value):
    return tag(field, 2) + vu(len(value)) + value


def f_str(field, value):
    return f_bytes(field, value.encode("utf-8"))


def f_bool(field, value):
    return f_varint(field, 1 if value else 0)


# ---------------------------------------------------------------- protobuf 解码
def rv(buf, i):
    r = 0
    s = 0
    while True:
        b = buf[i]
        i += 1
        r |= (b & 0x7F) << s
        s += 7
        if not b & 0x80:
            return r, i


def parse(buf):
    """返回 [(field, wire, value)]，值：varint→int，length-delimited→bytes"""
    out = []
    i = 0
    n = len(buf)
    while i < n:
        try:
            k, i = rv(buf, i)
        except Exception:
            break
        f, w = k >> 3, k & 7
        try:
            if w == 0:
                v, i = rv(buf, i)
            elif w == 2:
                ln, i = rv(buf, i)
                v = buf[i:i + ln]
                i += ln
            elif w == 5:
                v = buf[i:i + 4]
                i += 4
            elif w == 1:
                v = buf[i:i + 8]
                i += 8
            else:
                break
        except Exception:
            break
        out.append((f, w, v))
    return out


# ---------------------------------------------------------------- 组帧 / 解帧
def frame(payload, mtype):
    n = len(payload)
    b = [mtype & 0xFF,
         (n >> 24) & 0xFF, (n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF]
    # 服务端顺序：b0=(len>>24) b1=(len>>16) b2=(len>>8) b3=len b4=type b5/b6=校验
    head = bytearray(7)
    head[0] = (n >> 24) & 0xFF
    head[1] = (n >> 16) & 0xFF
    head[2] = (n >> 8) & 0xFF
    head[3] = n & 0xFF
    head[4] = mtype & 0xFF
    s = (head[0] + head[1] + head[2] + head[3] + head[4]) & 0xFFFF
    head[5] = (s >> 8) & 0xFF
    head[6] = s & 0xFF
    return bytes(head) + payload


def enc_call(call_id, method_id, params=b"", is_async=False):
    return (f_varint(1, call_id) + f_varint(2, method_id)
            + (f_bytes(3, params) if params else b"") + f_bool(4, is_async))


def enc_result(call_id, response=b"", error=0):
    return (f_varint(1, call_id)
            + (f_bytes(2, response) if response else b"") + f_varint(3, error))


def enc_handshake(mode=CONNECTION_MODE, name=CLIENT_NAME, auth=b""):
    return (f_str(1, mode) + f_str(2, name) + (f_bytes(3, auth) if auth else b""))


def dec_call(buf):
    """解析 Call，返回 (call_id, method_id, parameters)"""
    cid = mid = 0
    params = b""
    for f, w, v in parse(buf):
        if f == 1:
            cid = v
        elif f == 2:
            mid = v
        elif f == 3:
            params = v
    return cid, mid, params


# ---------------------------------------------------------------- 主循环
def read_exact(fd, n, timeout=30.0):
    got = bytearray()
    t0 = time.time()
    while len(got) < n:
        if time.time() - t0 > timeout:
            return None
        r, _, _ = select.select([fd], [], [], 1.0)
        if not r:
            continue
        try:
            chunk = os.read(fd, n - len(got))
        except OSError:
            return None
        if not chunk:
            return None
        got += chunk
    return bytes(got)


def answer(method_id, params):
    """返回要放进 Result.response 的负载"""
    if method_id == 27:                       # AgentServiceAlive → Bool{true}
        return f_bool(1, True)
    if method_id == 40:                       # GetIpv4Address → String{ip}
        return f_str(1, GUEST_IP)
    if method_id == 43:                       # GetVmMacAddress → String{mac}
        return f_str(1, "02:00:00:00:00:01")
    return b""


def main():
    log("agent 启动：port=%s name=%s ip=%s" % (PORT, CLIENT_NAME, GUEST_IP))
    while True:
        try:
            fd = os.open(PORT, os.O_RDWR)
        except OSError as e:
            log("打开 %s 失败：%s（2 秒后重试）" % (PORT, e))
            time.sleep(2)
            continue
        log("已打开 %s，发送握手" % PORT)
        try:
            os.write(fd, frame(enc_call(1, 0, enc_handshake()), MT_CALL))
            log("握手已发出 methodID=0")
        except OSError as e:
            log("握手发送失败：%s" % e)
            os.close(fd)
            time.sleep(2)
            continue
        while True:
            head = read_exact(fd, 7)
            if head is None:
                log("读头失败/超时，重新打开端口")
                break
            n = (head[0] << 24) | (head[1] << 16) | (head[2] << 8) | head[3]
            mtype = head[4]
            s = (head[0] + head[1] + head[2] + head[3] + head[4]) & 0xFFFF
            if ((s >> 8) & 0xFF) != head[5] or (s & 0xFF) != head[6]:
                log("校验和不符：收到 %s，算出 %04x" % (head.hex(" "), s))
            if n > 1 << 20:
                log("长度异常 %d，放弃" % n)
                break
            payload = read_exact(fd, n) if n else b""
            if payload is None:
                log("读载荷失败")
                break
            cid, mid, params = dec_call(payload)
            log("收到 type=%d len=%d callID=%d methodID=%d params=%s"
                % (mtype, n, cid, mid, params[:64].hex(" ")))
            resp = answer(mid, params)
            try:
                os.write(fd, frame(enc_result(cid, resp), MT_RESULT))
                log("已应答 callID=%d methodID=%d response=%d 字节"
                    % (cid, mid, len(resp)))
            except OSError as e:
                log("应答写入失败：%s" % e)
                break
        try:
            os.close(fd)
        except Exception:
            pass
        time.sleep(1)


if __name__ == "__main__":
    main()
