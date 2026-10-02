
---

## 握手必须满足的两个条件（逆向 + 实测确认）

vm_manager 的 `Service::HandleConnectSerial` 把串口第一帧解析成 `Handshake` 后调用
`HandleConnectMode(connectionMode, clientName)`；该函数（libvm_manager.z.so 0x2e6388）
做两项校验，任一不满足即 `return 0` 并打错误日志：

| # | 校验内容 | 反编译依据 |
|---|---|---|
| ① | **`connectionMode` 必须包含字符串 `guest-event-connection:`（23 字符）** | 该串由 `_GLOBAL__sub_I_service.cpp`(0x1291e0) 写入全局 `xmmword_369D60`(0x369D60)，`HandleConnectMode` 用它做 memchr/bcmp 子串匹配；不匹配则 `Warn("service handle connect mode: %{public}s error", 138)` |
| ② | **`clientName` 必须包含 `serialClient`（12 字符）** | 同函数里以 `0x6C6C6169726573`/`0x746E6569` 常量做匹配 |

### 实测（serialtest4，客户机 Debian 12）

- 之前用 `connectionMode=""` / `"1"`、`clientName="debian-guest"`：宿主 hilog 每秒打印
  `(HandleConnectSerial:177) Invalid handshake, op code: N`（N 即我们发的 methodID），
  且**虚拟机状态永远停在 9**。
- 改用 `connectionMode="guest-event-connection:"`、`clientName="serialClient"` 后：
  **`Invalid handshake` 彻底消失**，框架行为转为发起 call（`SetResult:89` / `WakeUp:68`），
  **虚拟机状态由 9 变为 1（安装判定为完成）** —— 这一转变在 serialtest/2/3 上从未出现过。

判据（随时可复现）：
```sh
# 宿主侧：修复后不应再出现 Invalid handshake
hdc shell "hilog -x | grep -a 'Invalid handshake'"
# 状态：9=正在安装，1=安装已完成（该状态会 latch，停掉 agent 不会回退）
./hvm-cli vmstat <名字>
```

注：agent 侧可用环境变量 `HVM_AGENT_MODE` / `HVM_AGENT_NAME` / `HVM_AGENT_PORT` 覆盖，
便于在客户机里直接迭代（见 `scripts/hvm-serial-agent.py`）。

---

## 可直接读写的旁路通道（TCP，同一套帧协议）

框架侧的串口握手协商仍有未解之处（`HandleConnectSerial` 只在连接建立那一次读一帧，
且其调用/注册点在本 .so 内零引用，实测打开 Debug 后也未见 `connectionmode=` 日志，
说明框架并未接受到连接）。为了能**实测地读/写 agent 的数据通道**，agent 增加了
一条 TCP 旁路通道，用的就是同一套 7 字节帧格式：

| 环境变量 | 作用 |
|---|---|
| `HVM_AGENT_TCP` | 监听端口（0 = 关闭）；例如 20001 |
| `HVM_AGENT_TCP_ECHO` | 1 = 把 TCP 客户端发来的帧回显回去（便于在无框架参与时验证读方向） |

工具：`scripts/agentctl.py`
```sh
python3 agentctl.py call 27        # 发一个 Call{methodID=27} 帧（等价 serial-write）
python3 agentctl.py handshake      # 发 type=3 Handshake（用逆向出的两个必需串）
python3 agentctl.py raw 0000...    # 发原始字节
python3 agentctl.py listen 8       # 只收（等价 serial-read）
```

实测（clean3，客户机 Debian 12）：
```
设备侧  → Call{methodID=27} 9 字节: 00 00 00 02 01 00 03 10 1b
设备侧  ← 应答            9 字节: 00 00 00 02 01 00 03 10 1b      ← 双向 ✓
客户机  TCP→串口 9 字节 / 已回显 9 字节 / 已把 9 字节写入串口      ← 两侧日志均有 ✓
```

注：`/etc/hvm-agent.env` 由包装脚本 `/usr/local/sbin/hvm-agent-run.sh` 以
`set -a`（自动 export）方式 source —— 少了 `set -a` 时只有显式 export 的变量才传进 agent，
像 `HVM_AGENT_TCP` 就会静默失效（实测踩到）。
