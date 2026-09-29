# hvm-cli

鸿蒙 PC（HarmonyOS PC）**虚拟机与 Linux 兼容环境控制工具**，纯 C++ 实现。

**不需要 root、不需要 HAP** —— 全部通过 `dlopen` 直接调用系统自带库。

> ⚠️ **必须在系统自带的 HiShell 终端中运行。**
>
> **原因：只有 HiShell 终端在虚拟机白名单内。**
> 虚拟机服务 `vm_manager`（SA 65621）对*每一个*请求都做调用者身份校验
> （`VmmCommonUtils::CheckCallerIdentity`），白名单里是这几类身份：
> HiShell HAP、LinuxFusionService(uid 5005)、hwf_service(uid 7700)、openEuler HAP。
> 其余都是系统服务或专用应用，**能被用户敲命令的终端只有 HiShell 一个**。
> 因此从 MKCode / BitFun / WorkBuddy / CodeArts Agent 等第三方应用的内置终端运行，
> 会在服务端被直接拒绝（日志 `... permission denied`），拿不到任何虚拟机能力。
> 详见[权限模型](#权限模型)。

本仓库构建**两个命令**，对应两条完全独立的技术栈：

| 命令 | 用途 | 底层通路 |
|---|---|---|
| `hvm-cli` | 控制**我们自己创建**的虚拟机：状态/能力/电源/快照/共享目录/网络/磁盘/显示 | `vm_manager`（SA 65621）+ StratoVirt |
| `openeuler` | 连入**融合开发引擎**（内部代号 RGM / LinuxFusion）的 openEuler 环境执行命令 | `/system/lib64/ndk/libfusion_pty_ndk.so`（virtio-vsock PTY） |

> 两者互不依赖：`hvm-cli` 走 Binder IPC 到 `vm_manager`；`openeuler` 走 LinuxFusion
> 的 PTY 通道。系统里 `hvm-cli` 的"活动虚拟机"与 `openeuler` 连进去的 openEuler
> 环境**不是同一个东西**（前者由 vm_manager 管理，后者由 LinuxFusion 管理）。

## hvm-cli：虚拟机管理

```console
$ ./hvm-cli info
虚拟化能力     : 支持
活动虚拟机     : (无)
状态码         : 0 (none)
openEuler 版本 : (未知)
共享目录开关   : 关

$ ./hvm-cli vms                       # 列/探测虚拟机（API 无枚举接口，按名字探测）
$ ./hvm-cli capability                # 本机是否支持虚拟化
$ ./hvm-cli --json info               # JSON 输出，便于脚本
$ ./hvm-cli help                      # 全部命令
```

命令覆盖：`info/status`、`vms`、`vm-info`、`capability`、`active-name/status`、
`vm-status`、`process-exist`、`feature`、`open-euler-version`、`force-stop`、
`quit-by-reboot-host`、`require-big-mem`、`snapshot list|create|restore|destroy|rename`、
`share list|enable|disable|add|remove|setup`、`net ip|proxy|share-on/off|dns-on/off`、
`disk capacity|path|size|expand|delete-data`、`resolution`、`touch-mem`、`swap-2d`。

## openeuler：融合开发引擎里的 openEuler 环境

HiShell 终端有个"openEuler"标签页，能连进 openEuler 执行命令；本命令用的是**同一个
系统接口**（`@ohos:fusion_pty_napi` 背后的 NDK 库）：

```console
$ ./openeuler selftest
dlopen=ok;manager=ok;open=ok;send=ok;image=ok;share=ok

$ ./openeuler exec 'uname -r; cat /etc/os-release | head -2'
6.6.0
NAME="openEuler"
VERSION="24.03 (LTS-SP3)"

$ ./openeuler shell            # 交互式 shell（Ctrl-D 退出）
$ ./openeuler status           # 通道/会话/共享目录概览
$ ./openeuler share status     # 共享目录开关
$ ./openeuler image install    # 安装/更新 openEuler 镜像
```

`exec` 的退出码即远端命令退出码；`--json` 输出 `{"stdout":...,"exitCode":N}`。

`shell` **不做任何行规程**：本地 tty 切 raw 后双向透传字节，行编辑、回显、历史、
补全、Ctrl-C/Ctrl-D 全部由远端 bash/readline 处理；CLI 只额外把本地窗口尺寸变化
同步给远端（SIGWINCH → `SetWinSize`）。

## hvm-cli：创建 / 启动 / 销毁虚拟机

`CreateVm` / `StartVm` 需要一个 `CfgInfo` 对象，而它是华为私有类型（无头文件、库被
strip）。本仓库按逆向出的**内联构造序列**手工构造它（见 `src/cfginfo.cpp` 与
`include/ohos/vm_manager_service/cfg_info.h`），并已实测打通：

```console
$ ./hvm-cli vm range                     # 先查可用范围（服务端校验依据）
CPU 数范围     : 6 .. 8
内存范围       : 6 .. 18

$ ./hvm-cli vm ctor --cpu 6 --mem 8 --disk-gb 128     # 只构造，不调服务
CfgInfo@0x... base=0x... vptr=0x...(base+0xB2F90)
  cpuNum(+12) = 6 ... startType(+80) = -1

$ ./hvm-cli vm create --name myvm --image /path/to/win.iso \
      --bios /path/to/uefi.fd --cpu 6 --mem 8 --disk-gb 128 --apply
```

不带 `--apply` 时是**预演**（打印将发送的内容，不产生副作用）。

### 参数规则（逆向自服务端校验，实测确认）

| 参数 | 单位 | 约束 |
|---|---|---|
| `--cpu` | 个数 | 必须在 `GetVmAvailableCpuNumRange` 返回的区间内（本机 6..8） |
| `--mem` | **GB** | 必须在 `GetVmAvailableMemorySizeRange` 区间内（本机 6..18） |
| `--disk` / `--disk-gb` | MB / GB | 服务端要求 `>= 0x10000`（64 GB）且不超过宿主磁盘 |
| `--bios` | 路径 | 必须存在且可读（服务端 `access(R_OK)`）；如 `/system/opt/virt_service/virtualized_hwf/stratovirt-vars` |
| `--image` | 路径 | 必须存在且可读，且**是 ISO 或 qcow2 镜像**（`DetectIsoType`/`GetQcowState`） |

> 本机未预置任何 VM 镜像（`/data/virt_service` 不存在），所以最后一步会停在
> `file is not iso.` / `not image` —— 协议层已通，只差提供真实镜像。

## 权限模型

`vm_manager` 对每个请求做调用者校验（`VmmCommonUtils::CheckCallerIdentity`），
白名单里除 LinuxFusionService（uid 5005）、hwf_service（uid 7700）、openEuler HAP 外，
**还单独放行 HiShell HAP**（服务端日志：`isLinuxFusionService:%d, isHiShellHap:%d,
isOpenEulerHap:%d`）。因此从系统自带终端 **HiShell** 启动的进程可以直接调用：

> 不 root、不做 HAP，直接调用系统自带的库，权限由**进程身份**决定。

反过来，从 MKCode / BitFun / WorkBuddy / CodeArts Agent 等第三方应用的内置终端
启动会被拒绝（`permission denied`）。

**结论：本工具只能在系统自带的 HiShell 终端里运行 —— 因为白名单里能被用户使用的
终端只有它一个**（其余放行身份都是系统服务或专用 HAP，用户无法在其中开终端）。

## 构建

```bash
make            # 同时生成 hvm-cli 与 openeuler
make check      # 分别自检两条通路
make install    # 可选，装到 ~/.local/bin
```

## 调试

系统自带的 lldb-server 在应用沙箱里 `ptrace` 会被拒；用华为随开发者工具提供的
`~/.local/bin/huawei-debug-lldb-server` 即可正常调试（脚本已封装）：

```console
$ scripts/hwdbg.sh ./hvm-cli 7799 -o "b main" -o continue -o bt
```

进程由 lldb-server 预先拉起并停在动态链接器入口，所以下完断点用 `continue`
恢复，不要用 `run`。

## 目录结构

| 路径 | 说明 |
|---|---|
| `src/hvm_client.h/.cpp` | vm_manager 客户端封装（dlopen + dlsym → 类型化接口） |
| `src/main.cpp` | `hvm-cli` 命令入口 |
| `src/fusion_pty.h/.cpp` | LinuxFusion PTY 通道封装 |
| `src/openeuler_main.cpp` | `openeuler` 命令入口 |
| `docs/api-notes.md` | 逆向笔记：SA、白名单、PTY 通道 C ABI、CfgInfo 构造、已知返回码 |
| `scripts/hwdbg.sh` | 沙箱内可用的 lldb 调试封装（华为 lldb-server） |

## 实现状态

- [x] `hvm-cli`：状态/能力/电源/快照/共享目录/网络/磁盘/显示/内存
- [x] `openeuler`：`exec` / `shell` / 共享目录 / 镜像安装
- [ ] `hvm-cli`：启动/创建虚拟机（`StartVm`/`CreateVm` 需要 `CfgInfo`，逆向中）
- [ ] `hvm-cli`：自建虚拟机的 `exec`/`shell`（走串口或 vsock，待定）
- [ ] 事件回调（`RegisterVmStatusCallback` 等）

## 说明

仅供在自有设备上做互操作性与自动化研究。
