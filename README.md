# hvm-cli

鸿蒙 PC（HarmonyOS PC）**虚拟机控制命令行工具**，纯 C++ 实现。

直接调用系统自带的虚拟机客户端库
`/system/lib64/libvm_manager_kits.z.so`
（`OHOS::VmManagerService::VmManagerClientWrapper`），
**不需要 root、不需要 HAP、不需要 Python**。

```console
$ ./hvm-cli info
虚拟化能力      : 支持
活动虚拟机      : (无)
状态码          : 0 (none)
openEuler 版本  : (未知)
共享目录开关    : 关
```

## 原理

```
┌──────────────────────────────────────────────┐
│  hvm-cli（本仓库，纯 C++，系统 clang++ 构建）  │
│    dlopen + dlsym 解析 C++ mangled 符号       │
└───────────────────┬──────────────────────────┘
                    │ 直接函数调用
┌───────────────────▼──────────────────────────┐
│  /system/lib64/libvm_manager_kits.z.so        │
│  OHOS::VmManagerService::VmManagerClientWrapper│
└───────────────────┬──────────────────────────┘
                    │ Binder IPC（/dev/binder）
┌───────────────────▼──────────────────────────┐
│  SA 65621  vm_manager（uid hwf_service）      │
│    └─ 拉起 /system/bin/virt_service/…/stratovirt│
│       （StratoVirt 2.5.0，HMV 后端 /dev/hmv） │
└──────────────────────────────────────────────┘
```

### 为什么能直接用（权限模型）

`vm_manager` 对每个请求做调用者校验
（`VmmCommonUtils::CheckCallerIdentity` → `IsleaglSystemAbility` /
`CheckCallingOpenEulerHap`），白名单里除了 LinuxFusionService（uid 5005）、
hwf_service（uid 7700）和 openEuler HAP 之外，**还单独放行 HiShell HAP**
（服务端日志：`isLinuxFusionService:%d, isHiShellHap:%d, isOpenEulerHap:%d`）。

因此从系统自带终端 **HiShell** 里启动的进程可以直接调用虚拟机 API：

> 不 root、不做 HAP，直接调用系统自带的库，权限由**进程身份**决定。

反过来，从 MKCode / BitFun / WorkBuddy / CodeArts Agent 等第三方应用的
内置终端启动会被拒绝。

## 构建与使用

```bash
make                 # 生成 hvm-cli
make check           # 自检：加载 kit 并读取状态
sudo make install    # 可选，装到 ~/.local/bin
```

```bash
./hvm-cli help                      # 全部命令
./hvm-cli info                      # 汇总状态
./hvm-cli active-name               # 活动虚拟机名
./hvm-cli snapshot list             # 快照列表
./hvm-cli share add <宿主> <客机>    # 添加共享目录
./hvm-cli net ip                    # 虚拟机 IPv4
./hvm-cli disk path                 # 磁盘镜像路径
./hvm-cli --json info               # JSON 输出，便于脚本调用
```

`--json` 让输出变成单行 JSON，所以 shell/Python/任何语言都能直接消费：

```bash
hvm-cli --json info | python3 -c "import json,sys; print(json.load(sys.stdin)['data']['capable'])"
```

## 连入 openEuler 虚拟机执行命令

系统里还有一条**通往 openEuler 访客的 PTY 通道**（HiShell 终端的"连接 openEuler
执行命令"就是这个功能），入口是 NDK 库
`/system/lib64/ndk/libfusion_pty_ndk.so`。本工具直接调用它：

```console
$ ./hvm-cli exec 'uname -r; cat /etc/os-release | head -2; id -un'
6.6.0
NAME="openEuler"
VERSION="24.03 (LTS-SP3)"
hu60

$ ./hvm-cli exec 'hostname; ip -4 addr show | grep inet'   # 任意命令
$ ./hvm-cli shell                                          # 交互式 shell（纯字节透传）
$ ./hvm-cli pty-selftest                                   # 通道自检
dlopen=ok;manager=ok;open=ok;send=ok
```

`exec` 的退出码即远端命令的退出码；`--json` 输出 `{"stdout":...,"exitCode":N}`，
便于脚本消费。实测环境：openEuler 24.03 LTS-SP3 / kernel 6.6.0 / 172.16.105.2。

`shell` 不做任何行规程：本地 tty 切 raw 后**双向透传字节**，行编辑、回显、
历史、补全、Ctrl-C/Ctrl-D 全部由远端 bash/readline 处理；
CLI 只额外负责把本地窗口尺寸变化同步给远端（SIGWINCH → `SetWinSize`）。

该库没有公开头文件，`OhPtyConfig`、回调等结构是从二进制里恢复出来的
（见 [docs/api-notes.md](docs/api-notes.md#7-fusion-pty-通道)）。

## 目录结构

| 路径 | 说明 |
|---|---|
| `src/hvm_client.h/.cpp` | 客户端封装：dlopen/dlsym + 类型化接口 |
| `src/main.cpp` | CLI 入口、子命令分发、文本/JSON 输出 |
| `src/fusion_pty.h/.cpp` | fusion PTY 通道：连入 openEuler 执行命令 |
| `docs/api-notes.md` | 逆向笔记：SA、接口、白名单、PTY 通道、状态码 |

## 实现状态

- [x] 调用链路打通（`GetActiveVmName` / `CheckVmCapability` / `GetVmStatus` / `IsProcessExist`）
- [x] 状态、能力、电源（强制关机）、快照、共享目录、网络、磁盘、显示、内存
- [x] 连入 openEuler 访客执行命令（`exec` / `shell`，走 fusion PTY NDK 通道）
- [ ] 启动/创建虚拟机（`StartVm` / `CreateVm` 需要 `CfgInfo`，逆向中）
- [ ] 事件回调（`RegisterVmStatusCallback` 等）

## 说明

仅供在自有设备上做互操作性与自动化研究。
