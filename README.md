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

## hvm-cli：虚拟机的创建 / 启动 / 暂停 / 停止 / 删除

> 必须在 **HiShell 终端**里运行（原因见[权限模型](#权限模型)）。
> 下面命令里的路径都是**本机实测可用**的具体取值，换个文件名就能直接粘贴执行。

### 1. 先看可用范围

```console
$ ./hvm-cli vm range
CPU 数范围     : 6 .. 8
内存范围       : 6 .. 18          # 单位 GB
```

磁盘下限为 65536 MB（= 64 GB），由服务端校验，不在上面这条输出里。

### 2. 创建虚拟机（只建配置与磁盘，不开机）

```console
$ ./hvm-cli vm create \
      --name myvm \
      --image   /storage/Users/currentUser/Download/com.huawei.hmos.hishell/debian-12.0.0-arm64-netinst.iso \
      --enhance /storage/Users/currentUser/Download/com.huawei.hmos.hishell/oetool.iso \
      --bios    /system/opt/virt_service/virtualized_hwf/stratovirt-uefi \
      --cpu 6 --mem 8 --disk-gb 128
CreateVm 返回 rc=0 (OK)
```

- `--image` 是安装盘 ISO。这个路径会被**自动转换**成媒体库视图
  `/storage/media/100/local/files/Docs/Download/com.huawei.hmos.hishell/debian-...iso`
  —— 它是唯一能让 `stratovirt` 真正打开光盘的形式（原因见 [api-notes 第 10 节](docs/api-notes.md)）。
  账号 id（这里是 `100`）取自 `$USER`，多账号设备上第二个账号是 `101`；
- `--enhance` 是**扩展盘（enhance ISO）**，**创建时必填**且必须是 `.iso` 文件
  （服务端校验：`enhanceFilePath` 不能为空，否则 `create vm fail, enhance file path is null`）。
  它对应客户机里的 `unattend` 槽位；
- **磁盘不用自己准备**：框架按 `--disk-gb` 生成稀疏的
  `/data/service/el0/virt_service/100/vm_manager/<hash>/myvm/img/vm.qcow2`。

### 3. 启动（开机）

```console
$ ./hvm-cli vm start --name myvm \
      --bios /system/opt/virt_service/virtualized_hwf/stratovirt-uefi \
      --mem 6
```

- `--mem` **至少要给**：服务端不接受内存为 0（只给 `--name` 会返回
  `invalid memory size: 0`）。范围见 `vm range`（本机是 6..18 GB）；
- 其余参数可以省略：省略的字段服务端用创建时存档的值（实测：省略 `--cpu` 时
  仍按存档的 6 核启动，省略 `--bios` 时仍用存档的固件路径）。

启动后 `stratovirt` 才真正打开光盘；**光盘能不能读，是到这一步才暴露的** ——
可以这样确认：

```console
$ ./hvm-cli vms myvm
活动虚拟机: myvm
名字                     状态     活动     磁盘镜像
myvm                     9        是       /data/service/el0/virt_service/100/vm_manager/<hash>/myvm/img/myvm.qcow2

$ pgrep -a stratovirt | grep myvm        # 首次启动时命令行里能看到两张光盘的 file=
```

> 服务端可能返回 `405 (VM_IP_UNAVAILABLE)`：那只是"启动后立刻查客户机 IP 没查到"，
> **不代表启动失败**，虚拟机通常已经跑起来了。

### 3.1 光盘怎么挂（重要）

`start` 的 `--image` / `--enhance` **只在第一次启动（安装阶段）生效**：

| 时机 | 光盘 |
|---|---|
| **第一次**启动 | 安装盘（`--image`）与扩展盘（`--enhance`）**两张都会挂上** |
| **之后**每次启动 | **一张都不挂** —— 即使命令行里再传 `--image` / `--enhance` 也一样（实测：此时命令行里只有 UEFI 固件、磁盘、UEFI vars） |

装完之后要挂盘，用**热插拔**接口：

```console
$ ./hvm-cli vm mount-cd --name myvm \
      --image /storage/Users/currentUser/Download/com.huawei.hmos.hishell/oetool.iso
已挂载: /storage/media/100/local/files/Docs/Download/com.huawei.hmos.hishell/oetool.iso
服务端返回: 1
```

（`服务端返回: 1` 是服务端分配的设备 id，不是错误码。实测证据：`vmlog` 里能看到
`QMP: --> blockdev_add { node_name: "cdrom-drive1" }` 与
`QMP: --> device_add { driver: "usb-storage" }`，也就是以 USB 存储设备热插拔进去的。）

> 服务端"第一次挂、之后不挂"的内部依据（很可能是一个"已安装"标志，`DoStartVm`
> 内部会调 `InstallVm`）**尚未反汇编确认**，这里只记录实测行为。

### 4. 暂停 / 恢复

```console
$ ./hvm-cli pause                  # 暂停活动虚拟机
$ ./hvm-cli resume myvm            # 恢复
```

### 5. 停止

```console
$ ./hvm-cli stop myvm              # 请求客户机自行关机（走客户机里的 GuestAgent）
$ ./hvm-cli stop myvm --clean      # 同上，clean=true
$ ./hvm-cli force-stop myvm        # 强制关机（不需要客户机配合）
```

两者差别**很关键**（实测）：

| 命令 | 服务端 API | 生效条件 |
|---|---|---|
| `stop` | `StopVm(name, clean)` | **需要客户机里的 GuestAgent 在线** —— 它本质是"请客户机自己关机"。客户机没起来（例如停在 GRUB）会返回 `405`，服务端日志是 `HostService has lost connection with GuestAgentService` / `system power request calling failed` |
| `force-stop` | `ForceStopVm(name)` | 直接关掉，实测任何状态下都能停（我们自己那台停在 GRUB 时也只有它能停） |

### 6. 删除（连磁盘一起删）

```console
$ ./hvm-cli vm destroy myvm
已销毁 myvm
```

### 7. 日常查看

```console
$ ./hvm-cli vms                          # 已知虚拟机一览
$ ./hvm-cli active-name                  # 当前活动虚拟机
$ ./hvm-cli vm-status myvm               # 状态码（0=未运行 9=运行中）
$ ./hvm-cli --vm myvm disk path           # 磁盘镜像路径
$ ./hvm-cli --vm myvm disk capacity       # 磁盘容量
$ ./hvm-cli --vm myvm snapshot list       # 快照列表
$ ./hvm-cli --vm myvm net ip              # 客户机 IPv4（需客户机已联网）
```

安装过程中的客户机文本输出（GRUB 菜单、控制台日志）可以从这里看到：

```console
$ tail -f /data/log/hwf_service/vmlog
```

### 参数规则（逆向自服务端校验，实测确认）

| 参数 | 单位 | 约束 |
|---|---|---|
| `--cpu` | 个数 | 必须在 `GetVmAvailableCpuNumRange` 返回的区间内（本机 6..8） |
| `--mem` | **GB** | 必须在 `GetVmAvailableMemorySizeRange` 区间内（本机 6..18） |
| `--disk` / `--disk-gb` | MB / GB | 服务端要求 `>= 0x10000`（64 GB）且不超过宿主磁盘 |
| `--bios` | 路径 | 必须存在且可读（服务端 `access(R_OK)`）；如 `/system/opt/virt_service/virtualized_hwf/stratovirt-vars` |
| `--image` | 路径 | 必须存在且**服务端进程**可读，且是 ISO 镜像（`DetectIsoType`） |
| `--enhance` | 路径 | 必须存在且可读，扩展名必须是 `.iso`（`CheckEnhanceFilePath`）；缺省会导致 `create vm fail, enhance file path is null` |

#### 路径必须写成「媒体库视图」（唯一对两个域都可读的形式）

实测三种写法里只有一种可用（`hvm-cli` 会自动转换）：

| 写法 | vm_manager 能读 | **stratovirt 能读**（挂光盘要靠它） |
|---|---|---|
| `/storage/Users/currentUser/Download/x.iso` | ✗ 它的命名空间里没有这个挂载 | — |
| `/data/service/el2/100/hmdfs/account/files/Docs/Download/x.iso`（hmdfs 真实路径） | ✓ | ✗ `Permission denied` |
| **`/storage/media/100/local/files/Docs/Download/x.iso`（媒体库视图）** | ✓ | ✓ |

这个写法是从**正在安装 Windows 的第三方应用虚拟机**的命令行里抓到的：

```
if=none,id=disk,format=raw,media=cdrom,file=/storage/media/100/local/files/Docs/Download/<bundle>/Win11_....iso
if=none,id=unattend,format=raw,media=cdrom,readonly=true,file=/storage/media/100/local/files/Docs/Download/<bundle>/server.iso
```

即：HAP 把 ISO 放在用户下载目录后，传给服务端的是**媒体库视图**路径（服务端字符串里
`^/storage/media/\d+/local/files/Docs/` 那条正则正是它的白名单）。
`hvm-cli` 现在会自动把 `/storage/Users/currentUser/...` 或
`file://docs/storage/Users/currentUser/...` 转换成该形式。

路径里的数字是 **OS 账号 id**，本机 HiShell 终端里就是环境变量 **`$USER`（=100）**。
工具优先直接读 `$USER`（要求纯数字），拿不到时回落到 `uid / 200000` 推导
（HiShell 的 20020085 → 100；第二账号下的应用 202xxxxx → 101），
也可用 `HVM_USER_ID` 显式覆盖；发生转换时会打印一行提示，例如
`（账号 id=100，取自 $USER（回落到 uid 20020085 / 200000））`。

磁盘**不需要**自己准备 qcow2 —— 框架会按 `CfgInfo.diskSize` 自行创建：

```
$ ./hvm-cli --vm win11 disk path
/data/service/el0/virt_service/100/vm_manager/<hash>/win11/img/vm.qcow2
```

> **实测已能完整创建虚拟机**（`CreateVm` 返回 0）：活动 VM 变成新名字、
> 框架自动生成磁盘 `.../vm_manager/<hash>/<vm>/img/vm.qcow2`（稀疏，随写增长）。

> ✅ **安装介质能挂上**（实测）：ISO 路径写成上面那种**媒体库视图**即可 ——
> `CreateVm` 返回 0、首次启动后安装盘与扩展盘两张都出现在 `stratovirt` 命令行里、
> `vmlog` 里 `Permission denied` 计数为 0。
> 另外两种写法各有原因：用户视图路径在服务进程的命名空间里不存在（`realpath` 失败），
> hmdfs 真实路径则是 `ohsw_stratovirt` 域读不了（SELinux）。
> 详见 `docs/api-notes.md` 第 10 节。

## 能力边界

完整的证据与推导见 [`docs/api-notes.md` 第 13 节](docs/api-notes.md)。一句话版本：

| 想要的能力 | 结论 |
|---|---|
| 在 HiShell 终端里调 vm_manager（借 HiShell 身份过白名单） | ✅ 可以 |
| 创建/启动/停机/销毁**我们自己**的虚拟机 | ✅ 可以 |
| 给自己的虚拟机挂安装盘（ISO 放在下载目录） | ✅ 可以（路径会自动转成媒体库视图） |
| 查询/管理**别的应用**的虚拟机（如 OSEasy 那台） | ❌ 服务端按应用隔离，只能看到名字 |
| 看到自己虚拟机的**画面**、给它**发按键** | ❌ 画面要合成进"应用窗口"，而窗口只有 UIAbility 能创建 |
| 自己写 HAP 绕过上面两条 | ❌ appIdentifier 不在白名单 |
| 让系统四指右滑进自己虚拟机的全屏 | ❌ 同上（系统合成的对象是应用窗口） |
| 读客户机的**文本**控制台（GRUB、安装器输出） | ✅ 可以（`-serial redirect-to-log` → `/data/log/hwf_service/vmlog`，我们可读） |
| 多台虚拟机并存 / 同时只运行一台 | ✅ / ⛔ 服务端限制 |

> 因此"交互式装系统 + 看画面"只能用**厂商合作应用的界面**（OSEasy / Sanway）；
> 本仓库负责**自动化控制我们自己的虚拟机**。

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
make              # 同时生成 hvm-cli 与 openeuler
make check        # 分别自检两条通路
make check-headers  # 语法检查 include/ 里的公共头文件
make syms         # 重新生成 vm_manager_kits.h / .syms.h
make symcheck     # 核对生成的 mangled 名与设备符号快照是否 121/121 一致
make check-repo   # 确认仓库里没有二进制文件
make install      # 可选，装到 ~/.local/bin
```

## 调试

系统自带的 lldb-server 在应用沙箱里 `ptrace` 会被拒。可改用**应用商店里的 CodeArts IDE**（`com.huawei.codearts`，
注意与白名单一节提到的 `com.huawei.codearts.agent` 是两个应用）自带的 `huawei-debug-lldb-server`：它躺在 CodeArts IDE 自己的沙箱里，
要在 **CodeArts IDE 的终端**里拷出来：

```console
$ mkdir -p ~/.local/bin
$ cp /data/storage/el2/base/files/huawei-debug-lldb-server ~/.local/bin/
```

之后在 HiShell 终端里就能用（脚本已封装）：

```console
$ scripts/hwdbg.sh ./hvm-cli 7799 -o "b main" -o continue -o bt
```

进程由 lldb-server 预先拉起并停在动态链接器入口，所以下完断点用 `continue`
恢复，不要用 `run`。

## 开发与验证命令（日常不需要）

这些是逆向与自检用的，用户日常操作不需要它们：

| 命令 | 用途 |
|---|---|
| `vm ctor [选项]` | 只构造 `CfgInfo` 入参对象并打印字段布局，**不调服务端**（验证构造配方） |
| `vm view-state <0\|1\|2>` / `vm displays <id>` | 上报 `HapViewState` / 显示器 id 列表（研究视图机制用） |
| `vm serial-read` / `vm serial-write` | 读写客户机通道（`ChannelInfo`） |
| `vm import` / `vm export` | 服务端 `Import/ExportVmDiskImage`（UOS 磁盘迁移专用，见 api-notes §11） |
| `hash-name` | 已禁用（服务端返回的指针在 `GetHashName` 内部会段错误） |
| `selftest` | 两条通路的加载自检（`hvm-cli selftest` / `openeuler selftest`） |
| `make symcheck` | 核对生成的 mangled 名与设备符号快照是否一致 |

## 目录结构

| 路径 | 说明 |
|---|---|
| `src/main.cpp` | `hvm-cli` 命令入口（含媒体库视图路径转换） |
| `src/hvm_client.h/.cpp` | vm_manager 客户端封装（dlopen + dlsym → 类型化接口，已接 87/120 个方法） |
| `src/cfginfo.h/.cpp` | 手工构造私有类：`CfgInfo` / `MigrationOptions` / `ChannelInfo` / `PortInfoList` |
| `src/fusion_pty.h/.cpp` | LinuxFusion PTY 通道封装（`openeuler` 的底层） |
| `src/openeuler_main.cpp` | `openeuler` 命令入口 |
| `include/` | **逆向还原的公共头文件**（可直接 `#include` 调用私有库），见 [`include/README.md`](include/README.md) |
| `docs/api-notes.md` | 逆向笔记 §1–§14：白名单、ABI 陷阱、线上格式、安装介质路径、通道、能力边界、接口覆盖 |
| `docs/abi/vm_manager_client_wrapper.symbols.txt` | 设备导出符号快照（121 个方法与 mangled 名，生成器的核对基准） |
| `scripts/gen-wrapper-api.py` | 生成 `vm_manager_kits.h` 与 `.syms.h`；借 ABI shim 让**编译器**产出 mangled 名并与设备核对 |
| `scripts/abi-shim/__config_site` | 强制 `_LIBCPP_ABI_NAMESPACE __h`，使本机 clang 产出的符号名与设备库一致 |
| `scripts/hwdbg.sh` | 沙箱内可用的 lldb 调试封装（CodeArts IDE 提供的 lldb-server） |
| `scripts/check-no-binary.sh` | 提交前检查：仓库内不得有二进制文件 |

## 实现状态

- [x] `hvm-cli`：状态/能力/电源/快照/共享目录/网络/磁盘/显示/内存
- [x] `hvm-cli`：**创建 / 启动 / 销毁虚拟机**（`CfgInfo` 手工构造，实测 `CreateVm` 返回 0）
- [x] `hvm-cli`：安装盘与扩展盘挂载 + 运行中热插拔（`vm mount-cd` / `vm unmount-cd`）。实测：**首次启动**两张盘都会挂上，之后启动不再自动挂盘，改用 `mount-cd`；ISO 路径会自动转成媒体库视图
- [x] `hvm-cli`：主机 ↔ 客户机通道（`ChannelInfo` + `Send/RecvDataFromVm`）
- [x] `hvm-cli`：LinuxFusion / RGM 运维面（`pause`/`resume`/剪贴板/图库/客户机磁盘共享/
      自动暂停之外的 23 个 kit 接口；kit 120 个方法已接 **78** 个）
- [x] `openeuler`：`exec` / `shell` / 共享目录 / 镜像安装
- [ ] `DeviceInfo`(0x260) 内部字段逐个还原（各 `Unwrap*Device`）
- [ ] 事件回调（`RegisterVmStatusCallback` / `IVmEventListener` 等）
- [ ] 自建虚拟机的画面与键鼠 —— **架构上不可达**，见[能力边界](#能力边界)

## 说明

仅供在自有设备上做互操作性与自动化研究。
