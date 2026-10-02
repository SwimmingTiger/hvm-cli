#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""vm_manager 客户机侧最小 guest-agent（跑在客户机里，对 virtio-serial 端口收发）。

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

【本版两处关键改动，针对"状态永远是 9"的实测症状】
  1) 同时服务多个串口（默认 /dev/vport2p1 与 /dev/vport2p2）。
     实测：Windows 客户机上框架连的是 virtio_serial1，而我们此前只开 vport2p1（serial0），
     故先把两个口都服务起来，排除"串口号不对"。
  2) 周期性重发首帧，直到该端口收到过任何一帧为止。
     实测：框架侧串口连接是间歇性的（vmlog 里反复 connection opened/closed），
     只在 open 时发一次首帧，很容易发在框架没监听的那一刻而永久丢失。
     对照：Windows agent 的 link_base.cpp:153 是「read 0 bytes ... retry after 1s」——
     它一直在重试，不会只试一次。
"""

import errno
import os
import sys
import time
import struct
import select
import socket
import threading

# 多个端口用逗号分隔。
# ★ 默认把 stratovirt 暴露的 6 个 virtio-serial 口【全试一遍】——
#   实测：只在 vport2p1 上发首帧（无论 type=1 Call{1} / type=3 Handshake / Call{31}）
#   发 21 次以上，框架一个字节都不回；vport2p2 宿主端甚至没连。
#   端口映射（stratovirt 命令行）：
#     winbox_serial0 nr=1 → vport2p1 | winbox_serial1 nr=2 → vport2p2
#     clipboard_vioser0 nr=3 → vport2p3 | gfscl nr=4 → vport2p4
#     additions_vioser0 nr=5 → vport2p5 | additions_vioser1 nr=6 → vport2p6
DEFAULT_PORTS = os.environ.get("HVM_AGENT_DEFAULT_PORTS", "/dev/vport2p1")
PORTS = [p.strip() for p in os.environ.get("HVM_AGENT_PORT", DEFAULT_PORTS).split(",") if p.strip()]
LOG = os.environ.get("HVM_AGENT_LOG", "/var/log/hvm-serial-agent.log")
CLIENT_NAME = os.environ.get("HVM_AGENT_NAME", "debian-guest")
CONNECTION_MODE = os.environ.get("HVM_AGENT_MODE", "1")
GUEST_IP = os.environ.get("HVM_AGENT_IP", "172.16.100.2")
# 首帧重发间隔（秒）；设 0 表示不重发（回到旧行为）
HS_RETRY = float(os.environ.get("HVM_AGENT_HS_RETRY", "1.0"))
# 端口读回 0 后的重连退避秒数（防止死循环刷屏）
RECONNECT_DELAY = float(os.environ.get("HVM_AGENT_RECONNECT_DELAY", "3.0"))
# 可选的 TCP 旁路通道：同一套帧协议，便于从宿主/设备侧直接读写（0 = 关闭）
TCP_PORT = int(os.environ.get("HVM_AGENT_TCP", "0"))
# 测试用：把 TCP 客户端发来的帧原样广播回去，便于在无框架参与时验证【读方向】通路
TCP_ECHO = os.environ.get("HVM_AGENT_TCP_ECHO", "0") == "1"
# 每个首帧候选连发几次再换下一个
HS_REPEAT = max(1, int(os.environ.get("HVM_AGENT_HS_REPEAT", "3")))
# ★★★ 关键：Call 的 callID 【必须非零】。
# 实测（注入验证）：callID=0 时框架构造的 Result 里 callID 也是 0，
# protobuf 默认值不编码 ⟹ Result 序列化长度 = 0 ⟹ LinkBase::SendMessage 报
# "message size exceeds limit, message send failed, size:0" ⟹ 框架永远回不了帧。
# 改成非零后，框架立刻回了 type=2 的 Result（callID 与我们发的一致）✓✓
CALL_ID = int(os.environ.get("HVM_AGENT_CALL_ID", "1"))

# MessageType（取自 LinkBase::SendMessage 各调用点的 mov w1, #N）：
#   1 = Call（IService::CallMethodSync/CallMethodAsync 都用它）
#   2 = Result（DispatchAndReply 回应 Call 用它）
#   3 = EstablishEventChannel
MT_CALL = 1
# ★★★ 实测：框架是否回帧取决于【收到的帧类型】——DispatchAndReply 只在 flags==0 时回，
#     而 flags 就是 ReceiveMessage 写回的帧类型。所以 Call 帧要用 type=0 才会被应答。
MT_CALL_REPLY = 0
MT_RESULT = 2
MT_HANDSHAKE = 3     # 串口连接后第一帧（裸 Handshake）用的类型，见 HandleConnectSerial
# 握手用的 methodID：InitService 里只注册了 0 和 31，故做成环境变量便于就地迭代
HS_ID = int(os.environ.get("HVM_AGENT_HS_ID", "1"))
# 第一条消息 = Call{methodID=1}，对应 GuestManager::BootComplete()（无参数！）
# 注册表（GuestManager::RegisterGuestMessages 反汇编）：
#   1 BootComplete / 3 GuestShutdown / 5 SessionChange / 6..25 其它
HS_PARAMS = os.environ.get("HVM_AGENT_HS_PARAMS", "")   # "" = 空参数，符合 BootComplete() 签名


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
    # 实测（Windows agent 第一帧 00 00 00 02 01 00 03 10 01，payload=10 01）：
    # callID=0 与 async=false 都是 protobuf 默认值，必须【省略不编码】，否则长度对不上。
    return ((f_varint(1, call_id) if call_id else b"")
            + f_varint(2, method_id)
            + (f_bytes(3, params) if params else b"")
            + (f_bool(4, True) if is_async else b""))


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


def answer(method_id, params):
    """返回要放进 Result.response 的负载"""
    if method_id == 27:                       # AgentServiceAlive → Bool{true}
        return f_bool(1, True)
    if method_id == 40:                       # GetIpv4Address → String{ip}
        return f_str(1, GUEST_IP)
    if method_id == 43:                       # GetVmMacAddress → String{mac}
        return f_str(1, "02:00:00:00:00:01")
    return b""


# ---------------------------------------------------------------- 主循环
# ---- 首帧候选：一次构建里轮换试，避免每猜一次就重建 ISO（约 10 分钟）----
# 依据：
#   · 实测抓到的 Windows agent 第一帧 = 00 00 00 02 01 00 03 10 01
#     → type=1 Call{methodID=1}（BootComplete）
#   · 服务端 HandleConnectSerial 做 ReceiveMessage(sock,&type,&buf,3) 并把载荷
#     ParseFromArray 成 Handshake —— 所以也可能是 type=3 + 裸 Handshake
# ★ 顺序很重要：框架的 HandleConnectSerial 只在【连接建立那一次】读一帧并解析成 Handshake，
#   解析失败就 return 0 且不再重试（实测）。所以第一帧【必须】是 type=3 的 Handshake。
#   （最初 Windows agent 抓到的首帧是 type=1 Call{1}，但那是它自己 boot 后主动发的；
#     对我们这种"等框架来连"的 agent，握手必须排第一。）
# ★★ 只发 Handshake，且反复发 ★★
# 实测（hilog Debug）：框架的 HandleConnectSerial 在连接建立那一次读【一帧】并解析成
# Handshake；读到 Call（例如 7 字节头 + 2 字节载荷的 Call{methodID}）就解析失败、
# 打 "Invalid handshake, op code: N" 然后 return false，【之后不再重试】。
# 所以候选轮换是有害的：轮换迟早会让框架读到 Call。第一帧必须、且只能是 Handshake。
HS_CANDIDATES = [
    (MT_HANDSHAKE, enc_handshake(),  "type=3 Handshake{connectionMode,clientName}"),
    (MT_CALL_REPLY, enc_call(CALL_ID, 1),   "type=0 Call{callID=%d,methodID=1}  (BootComplete, 会被应答)" % CALL_ID),
    (MT_CALL_REPLY, enc_call(CALL_ID, 16),  "type=0 Call{callID=%d,methodID=16} (能力列表, 会被应答)" % CALL_ID),
]
if os.environ.get("HVM_AGENT_HS_ONLY_ID"):
    # 只想固定发某一种时：HVM_AGENT_HS_ONLY_ID=1 → 只发 Call{methodID=1}
    _only = int(os.environ["HVM_AGENT_HS_ONLY_ID"])
    HS_CANDIDATES = [(MT_CALL, enc_call(0, _only), "type=1 Call{methodID=%d}" % _only)]


def open_port(path):
    """非阻塞打开 virtio-serial 端口。对端还没打开时 open 会阻塞，故必须 O_NONBLOCK。"""
    fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
    return fd


def send_first_frame(fd, port, conn):
    """轮换发送首帧候选，直到该端口收到过任何一帧为止。

    为什么轮换：目前无法确定服务端 HandleConnectSerial 期望的第一帧到底是什么。
    反汇编显示它做 ReceiveMessage(sock,&type,&buf,3) 然后 ParseFromArray 成 Handshake，
    而实测抓到的 Windows 第一帧是 type=1 的 Call{methodID=1}。两种都可能，
    与其每猜一次就重建一次 ISO（约 10 分钟），不如一次构建里轮换试。
    """
    # 每个候选连发 HS_REPEAT 次再换下一个（握手必须先被框架读到并派发）
    idx = (conn.get("hs_count", 0) // HS_REPEAT) % len(HS_CANDIDATES)
    mtype, payload, label = HS_CANDIDATES[idx]
    os.write(fd, frame(payload, mtype))
    conn["last_hs"] = time.time()
    conn["hs_count"] = conn.get("hs_count", 0) + 1
    conn["hs_label"] = label
    if conn["hs_count"] == 1:
        log("[%s] 首帧已发出 #%d %s" % (port, idx + 1, label))
    elif conn["hs_count"] % (3 * HS_REPEAT) == 0:
        log("[%s] 已发 %d 次仍无回应，最近一次: #%d %s"
            % (port, conn["hs_count"], idx + 1, label))


def handle_buffered(fd, port, conn):
    """从 conn['buf'] 中切出完整帧并应答；返回 False 表示端口应关闭"""
    buf = conn["buf"]
    while True:
        if len(buf) < 7:
            return True
        n = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]
        mtype = buf[4]
        if n > (1 << 20):
            log("[%s] 长度异常 %d，清空缓冲" % (port, n))
            del buf[:]
            return True
        if len(buf) < 7 + n:
            return True
        head = bytes(buf[:7])
        payload = bytes(buf[7:7 + n])
        del buf[:7 + n]
        conn["got"] = True
        s = (head[0] + head[1] + head[2] + head[3] + head[4]) & 0xFFFF
        if ((s >> 8) & 0xFF) != head[5] or (s & 0xFF) != head[6]:
            log("[%s] 校验和不符：收到 %s，算出 %04x" % (port, head.hex(" "), s))
        cid, mid, params = dec_call(payload)
        log("[%s] 收到 type=%d len=%d callID=%d methodID=%d params=%s"
            % (port, mtype, n, cid, mid, params[:64].hex(" ")))
        resp = answer(mid, params)
        try:
            os.write(fd, frame(enc_result(cid, resp), MT_RESULT))
            log("[%s] 已应答 callID=%d methodID=%d response=%d 字节"
                % (port, cid, mid, len(resp)))
        except OSError as e:
            log("[%s] 应答写入失败：%s" % (port, e))
            return False
    return True


_tcp_clients = []
_tcp_lock = threading.Lock()


def tcp_server(port):
    """旁路通道：TCP 客户端可用同一套帧协议收发。
    客户端发来的字节 = 直接写到串口（等价于 serial-write）；
    串口收到的字节 = 广播给所有 TCP 客户端（等价于 serial-read）。"""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(4)
    log("TCP 旁路通道已监听 0.0.0.0:%d" % port)
    while True:
        try:
            c, a = srv.accept()
        except OSError:
            time.sleep(1); continue
        with _tcp_lock:
            _tcp_clients.append(c)
        log("TCP 客户端接入 %s:%d（共 %d 个）" % (a[0], a[1], len(_tcp_clients)))
        threading.Thread(target=tcp_reader, args=(c, a), daemon=True).start()


def tcp_reader(c, a):
    buf = bytearray()
    while True:
        try:
            d = c.recv(65536)
        except OSError:
            d = b""
        if not d:
            break
        buf += d
        while True:
            if len(buf) < 7:
                break
            n = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]
            if len(buf) < 7 + n:
                break
            frm = bytes(buf[:7 + n]); del buf[:7 + n]
            log("TCP→串口 %d 字节: %s" % (len(frm), frm[:24].hex(" ")))
            _to_serial.append(frm)
            if TCP_ECHO:
                tcp_broadcast(frm)
                log("已回显 %d 字节给 TCP 客户端（读方向通路验证）" % len(frm))
    with _tcp_lock:
        if c in _tcp_clients:
            _tcp_clients.remove(c)
    try:
        c.close()
    except Exception:
        pass
    log("TCP 客户端断开 %s:%d" % (a[0], a[1]))


_to_serial = []


def tcp_broadcast(data):
    with _tcp_lock:
        for c in list(_tcp_clients):
            try:
                c.sendall(data)
            except OSError:
                try:
                    c.close()
                except Exception:
                    pass
                _tcp_clients.remove(c)


def main():
    log("agent 启动：ports=%s name=%s ip=%s 首帧重发间隔=%ss"
        % (",".join(PORTS), CLIENT_NAME, GUEST_IP, HS_RETRY))
    # 诊断：把客户机里真实存在的 virtio-serial 设备列出来（省得靠猜端口号）
    try:
        present = sorted(n for n in os.listdir("/dev") if n.startswith("vport"))
        log("客户机 /dev 下的 virtio-serial 设备: %s"
            % (", ".join("/dev/" + n for n in present) if present else "（一个都没有！）"))
    except Exception as e:
        log("列 /dev 失败：%s" % e)
    if TCP_PORT:
        threading.Thread(target=tcp_server, args=(TCP_PORT,), daemon=True).start()
    conns = {}          # fd -> {port, buf, got, last_hs, hs_count}
    dead_until = {}     # port -> 在此时刻之前不要重开（读回 0 的端口退避）

    while True:
        # 1) 补齐所有应打开的端口
        have = {c["port"] for c in conns.values()}
        for p in PORTS:
            if p in have:
                continue
            if time.time() < dead_until.get(p, 0):
                continue
            try:
                fd = open_port(p)
            except OSError as e:
                # 对端未打开时这里会失败；静默重试，不刷屏
                if not conns:
                    log("[%s] 打开失败：%s（2 秒后重试）" % (p, e))
                continue
            conns[fd] = {"port": p, "buf": bytearray(), "got": False,
                         "last_hs": 0.0, "hs_count": 0}
            log("[%s] 已打开" % p)

        if not conns:
            time.sleep(2)
            continue

        # 2) 周期性重发首帧（直到该端口收到过任何一帧）
        now = time.time()
        if HS_RETRY > 0:
            for fd, c in list(conns.items()):
                if c["got"]:
                    continue
                if now - c["last_hs"] >= HS_RETRY:
                    try:
                        send_first_frame(fd, c["port"], c)
                    except OSError as e:
                        # ★ EAGAIN/EWOULDBLOCK 不是"端口坏了"：virtio-serial 在宿主端
                        #   还没打开时，O_NONBLOCK 写会返回 EAGAIN。此时【绝不能关端口】——
                        #   否则会陷入「打开→写失败→关闭→再打开」的每 0.5 秒死循环
                        #   （实测踩到：/dev/vport2p2 一直刷 "Resource temporarily unavailable"）。
                        if e.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                            c["last_hs"] = now      # 稍后再试，不关端口
                        else:
                            log("[%s] 写首帧失败：%s（重开）" % (c["port"], e))
                            try:
                                os.close(fd)
                            except Exception:
                                pass
                            del conns[fd]

        # 2.9) 把 TCP 客户端发来的帧写进串口（等价 serial-write）
        while _to_serial:
            frm = _to_serial.pop(0)
            peer = next(iter(conns), None)
            if peer is not None:
                try:
                    os.write(peer, frm)
                    log("已把 %d 字节写入串口" % len(frm))
                except OSError as e:
                    log("写串口失败: %s" % e)

        # 3) 收数据
        if not conns:
            continue
        now2 = time.time()
        r, _, _ = select.select(list(conns), [], [], 0.5)
        for fd in r:
            c = conns.get(fd)
            if c is None:
                continue
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                chunk = b""
            if not chunk:
                # read 返回 0（对端关闭 virtio-serial）→ 关掉，下一轮重开
                if now2 - c.get("last_dead", 0) > 10:
                    log("[%s] 端口断开（读回 0），稍后重开" % c["port"])
                c["last_dead"] = now2
                dead_until[c["port"]] = time.time() + RECONNECT_DELAY
                try:
                    os.close(fd)
                except Exception:
                    pass
                del conns[fd]
                continue
            tcp_broadcast(chunk)
            c["buf"] += chunk
            if not handle_buffered(fd, c["port"], c):
                try:
                    os.close(fd)
                except Exception:
                    pass
                del conns[fd]

        time.sleep(0.05)


if __name__ == "__main__":
    main()
