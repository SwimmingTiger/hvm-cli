# hvm-cli

鸿蒙 PC（HarmonyOS PC）**虚拟机与 Linux 兼容环境控制工具**，纯 C++ 实现。

**不需要 root、不需要 HAP、不需要 Python** —— 全部通过 `dlopen` 直接调用系统自带库。

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

## 权限模型

`vm_manager` 对每个请求做调用者校验（`VmmCommonUtils::CheckCallerIdentity`），
白名单里除 LinuxFusionService（uid 5005）、hwf_service（uid 7700）、openEuler HAP 外，
**还单独放行 HiShell HAP**（服务端日志：`isLinuxFusionService:%d, isHiShellHap:%d,
isOpenEulerHap:%d`）。因此从系统自带终端 **HiShell** 启动的进程可以直接调用：

> 不 root、不做 HAP，直接调用系统自带的库，权限由**进程身份**决定。

反过来，从 MKCode / BitFun / WorkBuddy / CodeArts Agent 等第三方应用的内置终端
启动会被拒绝（`permission denied`）。

## 构建

```bash
make            # 同时生成 hvm-cli 与 openeuler
make check      # 分别自检两条通路
make install    # 可选，装到 ~/.local/bin
```

## 目录结构

| 路径 | 说明 |
|---|---|
| `src/hvm_client.h/.cpp` | vm_manager 客户端封装（dlopen + dlsym → 类型化接口） |
| `src/main.cpp` | `hvm-cli` 命令入口 |
| `src/fusion_pty.h/.cpp` | LinuxFusion PTY 通道封装 |
| `src/openeuler_main.cpp` | `openeuler` 命令入口 |
| `docs/api-notes.md` | 逆向笔记：SA、白名单、PTY 通道 C ABI、已知返回码 |

## 实现状态

- [x] `hvm-cli`：状态/能力/电源/快照/共享目录/网络/磁盘/显示/内存
- [x] `openeuler`：`exec` / `shell` / 共享目录 / 镜像安装
- [ ] `hvm-cli`：启动/创建虚拟机（`StartVm`/`CreateVm` 需要 `CfgInfo`，逆向中）
- [ ] `hvm-cli`：自建虚拟机的 `exec`/`shell`（走串口或 vsock，待定）
- [ ] 事件回调（`RegisterVmStatusCallback` 等）

## 说明

仅供在自有设备上做互操作性与自动化研究。
