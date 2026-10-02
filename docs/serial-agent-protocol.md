
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
