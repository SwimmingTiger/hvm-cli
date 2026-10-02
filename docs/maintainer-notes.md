# 维护者笔记：从 README 移出的内部细节

[README.md](../README.md) 面向**用户**，只讲"怎么用"：命令怎么写、参数取值范围与必填性、
用户能感知的限制。本文件是 README 中**维护者细节的归档** —— 是搬家不是删除，
原先写在 README 里的这些事实都完整落在下面，只是按主题重新组织。

| 主题 | 章节 |
|---|---|
| 服务端参数校验内幕（`Check*` / `GetVmAvailable*Range` / `access(R_OK)` / `DetectIsoType` …） | [1](#1-服务端参数校验内幕) |
| 网络字段是怎么分配的（`CfgInfo` 字段与偏移、`networkDevice`、`create` vs `start`） | [2](#2-网络字段是怎么分配的cfginfo) |
| 光盘挂载的引擎内幕（`media=cdrom` 计数、两张盘只在 `CreateVm` 那次挂上） | [3](#3-光盘挂载的引擎内幕) |
| 媒体库视图的"两个域"与 SELinux / hmdfs | [4](#4-媒体库视图两个域与-selinuxhmdfs) |
| 验证配方（`pgrep -a stratovirt`、命令行检查等） | [5](#5-验证配方) |
| 其它服务端细节（API 名与日志痕迹） | [6](#6-其它服务端细节api-名与日志痕迹) |
| 开发与验证命令 | [7](#7-开发与验证命令) |
| 目录结构 | [8](#8-目录结构) |
| 实现状态 | [9](#9-实现状态) |
| 调试（lldb / hwdbg） | [10](#10-调试lldb--hwdbg) |
| 与 openEuler 构建环境打交道的两条硬规则 | [11](#11-与-openeuler-构建环境打交道的两条硬规则) |

> 更深的逆向证据见 [`api-notes.md`](api-notes.md)（§9 CfgInfo 构造与校验链、§10 安装介质路径、
> §13 能力边界）与 [`iso-install-notes.md`](iso-install-notes.md)（§3 磁盘检查门槛、§4 网络字段）。

## 1. 服务端参数校验内幕

### 1.1 调用者身份校验

虚拟机服务 `vm_manager`（SA 65621）对**每一个**请求都做调用者身份校验
（`VmmCommonUtils::CheckCallerIdentity`），白名单里是这几类身份：
HiShell HAP、LinuxFusionService(uid 5005)、hwf_service(uid 7700)、openEuler HAP。
服务端日志形如 `isLinuxFusionService:%d, isHiShellHap:%d, isOpenEulerHap:%d`。

其余都是系统服务或专用应用，**能被用户敲命令的终端只有 HiShell 一个** ——
所以从 MKCode / BitFun / WorkBuddy / CodeArts Agent 等第三方应用的内置终端运行，
会在服务端被直接拒绝（日志 `... permission denied`），拿不到任何虚拟机能力。

> README 只保留结论：只有特定身份能用、只有 HiShell 能开终端。

### 1.2 参数校验链（实测日志，逐条对应）

```
hvm-cli    [CreateVm:213]        [virtio-mem] write cfgInfo      ← 客户端 marshal 成功
vm_manager [HandleCreateVm:300]  [virtio-mem] Read cfgInfo       ← 服务端 unmarshal 成功
vm_manager [IsValidPath:81]      Standardized path fail!         ← bios/image 路径
vm_manager [CheckParams:110/124] invalid memory size / disk size ← 单位：内存 GB、磁盘 MB
vm_manager [CheckFiles:159]      no such file.                   ← access(path, R_OK)
vm_manager [DetectIsoType:148]   file is not iso.                ← 镜像类型
vm_manager [CheckWinImgPath:175] not image
```

完整顺序（便于排错）：

```
IsValidPath(81)          路径 realpath（服务端命名空间）
DetectIsoType(144/148)   路径有效 + 必须是 ISO
CheckFiles(154/159)      存在 + access(R_OK)
CheckParams(110/124)     memory(GB) / disk(MB, >=0x10000) / cpu
CheckEnhanceFilePath     enhance 必须非空且扩展名为 .iso
CheckWinImgPath(175)     Windows 安装镜像判定
→ 建目录、写设备 JSON、配网、注册回调 → rc=0
```

各函数行为：

- `GetVmAvailableCpuNumRange` / `GetVmAvailableMemorySizeRange`：`--cpu` / `--mem` 的取值区间
  来源（本机 6..8 / 6..18 GB），也就是 `range` 命令打印的内容；磁盘下限不在其中，
  由 `CheckParams` 单独校验；
- `CheckParams`：`memory`（GB）、`disk`（MB，要求 `>= 0x10000` = 64 GB）、`cpu`；
- `CheckFiles`：`access(path, R_OK)` —— 注意是**服务端进程**的可读性，不是当前用户；
- `DetectIsoType`：路径有效且必须是 ISO 镜像；
- `CheckEnhanceFilePath`：`enhanceFilePath`（CfgInfo+56）不能为空且扩展名必须是 `.iso`，
  否则 `create vm fail, enhance file path is null`；
- `IsValidPath`：对路径做 `realpath`（在服务端命名空间里），失败即 `Standardized path fail!`；
  应用沙箱 `/data/storage/el2/base/files/...`、`/data/local/tmp`、`/dev/shm` 也都过不了它；
- `CheckWinImgPath`：Windows 安装镜像判定（非 UOS 调用方必须命中 Windows 安装盘特征串）。

### 1.3 参数规则表（内部依据）

| 参数 | 单位 | 服务端来源 / 校验 |
|---|---|---|
| `--cpu` | 个数 | 必须在 `GetVmAvailableCpuNumRange` 返回的区间内（本机 6..8） |
| `--mem` | **GB** | 必须在 `GetVmAvailableMemorySizeRange` 区间内（本机 6..18）；不接受 0 |
| `--disk` / `--disk-gb` | MB / GB | 服务端要求 `>= 0x10000`（64 GB）且不超过宿主磁盘 |
| `--bios` | 路径 | 必须存在且可读（服务端 `access(R_OK)`）；如 `/system/opt/virt_service/virtualized_hwf/stratovirt-vars` |
| `--image` | 路径 | 必须存在且**服务端进程**可读，且是 ISO 镜像（`DetectIsoType`） |
| `--enhance` | 路径 | 必须存在且可读，扩展名必须是 `.iso`（`CheckEnhanceFilePath`）；缺省会导致 `create vm fail, enhance file path is null` |

## 2. 网络字段是怎么分配的（`CfgInfo`）

网络**不是磁盘里的东西**，而是宿主侧按 `CfgInfo` 分配的：

```
VmManager::CreateVm / StartVm
  → VmAssistantManager::CheckBeforeStartVm
  → Engine::CheckBeforeStartVm
  → Engine::NetConfig(CfgInfo, vmName, deviceUuid)
  → NetManager::AllocateNet
  → DefaultNetConfig / BridgeNetConfig
  → VmNetProperties::SetNetConfigInfo
```

结果写进一个**全局单例**，`GetVmIpv4Address` / `SetVmNetMode` 都读它。

字段位于 `deviceInfo`（CfgInfo+88）子对象内，字段名取自 napi 库
（`UnwrapNetworkDevice` 的 `Unwrap*ByPropertyName` 实参），偏移与
`Engine::NetConfig` / `NetManager::SetNetConfig` 的读取逐一对应（已核对）：

| jsKey | 偏移(CfgInfo) | 类型 |
|---|---|---|
| `netMode` | +228 | int（0=桥接，1=NAT） |
| `nicName` | +232 | string（桥接时要给宿主物理网卡名） |
| `bridgeIp` | +256 | string（桥接 IP） |
| `proxyAutoSyncEnabled` | +280 | bool |
| `dnsAutoSyncEnabled` | +281 | bool |
| `isHostNetworkSyncFeatureEnabled` | +282 | bool |
| `isNetworkShareSupported` | +283 | bool |
| `networkDevice` | +284 | bool（deviceInfo+196，子对象开关） |

**`networkDevice` 必须为 false（桥接时）**：它为真时 `Engine::NetConfig` 会**强制**
`netMode=1`（NAT），所以桥接时若置真，服务端会把桥接按 NAT 处理。

### 2.1 `create` 与 `start` 的差别（实测，不要照抄推测）

| 操作 | 结果 |
|---|---|
| `create … --net nat` | 虚拟机有网卡：`stratovirt` 命令行里出现 `virtio-net-pci,netdev=net0,id=nic0` 与宿主侧 tap `WVMTap…` ✓ |
| 只把 `--net nat` 加在 `start` 上 | **没有网卡** ✗ |

即网络配置要进 `create` 时那份**存档配置**里；`StartVm` 传的 `CfgInfo` 起不到这个作用。
原因与 `Engine::CheckBeforeStartVm` 里的**磁盘检查门槛**共同作用
（详见 [iso-install-notes §3/§4](iso-install-notes.md)）：

```c
QcowDisk = Engine::CreateQcowDisk(...);          // state==5005 时立即 return 0
if (!QcowDisk) {
    StratovirtVars = Engine::CreateStratovirtVars(...);   // 同样 5005 直接 return 0
    if (!StratovirtVars) {
        VmDevice::InitDeviceInfo();
        VmDevice::QueryDeviceUuid(uuid);
        return Engine::NetConfig(cfg, vmName, uuid);      // ← 只有这里才建网络
    }
}
```

磁盘处理任一步返回非 0，整个块（含建网）都会被跳过。`QcowState` 每个虚拟机一条，
存在系统设置里（`SettingProvider`，key = 常量前缀 + 虚拟机名），缺省 2；
`VmAssistantManager::SetDefaultQcowState` 每次启动都会把它重置成 2，
而 **5005 = "磁盘无需处理"**（两条判断最前面就 `return 0`）。

**导入出来的虚拟机没有网络**：`ImportVmDiskImage(vmName, srcPath, sha256, opts)` 的签名里
**根本没有 `CfgInfo`**，所以框架眼里它"没有网络配置"，`net ip` / 模式切换一律返回
405 (`VM_IP_UNAVAILABLE`)。

**桥接尚未验证**：此前在 `start` 上加 `--net bridge --nic …` 一律得到常量错误
`-151060477`，`create` 时未测。（README 保留这一条用户能感知的限制。）

## 3. 光盘挂载的引擎内幕

挂盘发生在 **`CreateVm`** 那次（`create` 顺带完成的启动），不在之后的 `start`：

| 操作 | 光盘 | 实测证据 |
|---|---|---|
| **`create`** | 安装盘（`--image`）+ 扩展盘（`--enhance`）**两张都会挂上** | `create` 返回 rc=0 后立刻能查到 `stratovirt` 进程（状态 9、`vminfo` 给出 PID），其命令行里 `media=cdrom` 计数为 **2** |
| **之后的任何 `start`** | **一张都不挂** —— 即使命令行里再传 `--image` / `--enhance` | 对该虚拟机执行 `start --image … --enhance …`，命令行里只有 UEFI 固件、磁盘、UEFI vars，`media=cdrom` 计数为 **0** |

也就是说：`CreateVm` 会顺带完成一次启动（安装阶段就是这一次），安装介质也只在这一次挂上；
启动后 `stratovirt` 才真正打开光盘，**光盘能不能读，是到这一步才暴露的**。
"`CreateVm` 为什么会启动"推测与内部的 `InstallVm` / 快速启动流程有关
（`DoStartVm` 内会调 `InstallVm`），**尚未反汇编确认**，只记录实测行为。

以后再挂盘必须用**热插拔**接口 `MountCDDriveToVm`（`hvm-cli mount-cd`）：
它通过 QMP 以 `usb-storage` 设备热插拔进去，服务端返回分配的设备 id（不是错误码）。
`vmlog` 里可见：

```
QMP: --> blockdev_add { node_name: "cdrom-drive1" }
QMP: --> device_add { driver: "usb-storage" }
```

## 4. 媒体库视图：两个域与 SELinux/hmdfs

挂安装盘时，路径要同时被**两个进程**读：校验阶段的 `ohos_vm_manager` 与真正打开光盘的
`ohsw_stratovirt`。实测三种写法里只有一种同时满足：

| 写法 | `ohos_vm_manager`（校验阶段） | **`ohsw_stratovirt`**（真正打开光盘） |
|---|---|---|
| `/storage/Users/currentUser/Download/x.iso` | ✗ 它的命名空间里没有这个挂载（`realpath` 失败 → `Standardized path fail!`） | — |
| `/data/service/el2/100/hmdfs/account/files/Docs/Download/x.iso`（hmdfs 真实路径） | ✓ | ✗ `Permission denied` |
| **`/storage/media/100/local/files/Docs/Download/x.iso`（媒体库视图）** | ✓ | ✓ |

**SELinux 域隔离**（hmdfs 真实路径不行的原因）：

```
(typetransition ohos_vm_manager ohsw_stratovirt_exec process ohsw_stratovirt)
```

| 阶段 | 域 | 结果 |
|---|---|---|
| `IsValidPath` / `DetectIsoType` / `CheckFiles` | `ohos_vm_manager` | 能读 hmdfs ✓ |
| `stratovirt` 打开光盘文件 | `ohsw_stratovirt` | hmdfs 一律 `Permission denied` ✗ |

`/storage/Users/...` 则是**挂载命名空间隔离**：服务进程看不到该挂载，`realpath()` 直接失败
（日志 `IsValidPath:81 Standardized path fail!`）。

**这个写法是从正在安装 Windows 的第三方应用虚拟机**（`com.sanway.ecoengine`）**的
`stratovirt` 完整命令行里抓到的**：

```
if=none,id=disk,format=raw,media=cdrom,file=/storage/media/100/local/files/Docs/Download/<bundle>/Win11_....iso
if=none,id=unattend,format=raw,media=cdrom,readonly=true,file=/storage/media/100/local/files/Docs/Download/<bundle>/server.iso
```

即 HAP 把 ISO 放在用户下载目录后，传给服务端的是**媒体库视图**路径 ——
服务端字符串里 `^/storage/media/\d+/local/files/Docs/` 那条正则正是它的白名单。

`hvm-cli` 已内置转换：`/storage/Users/currentUser/<p>` 与
`file://docs/storage/Users/currentUser/<p>` → `/storage/media/<账号 id>/local/files/Docs/<p>`。

**账号 id 的取值顺序**（`hvm-cli` 实现）：

1. `HVM_USER_ID`（显式覆盖，便于引用别的账号视图下的文件）；
2. **`$USER`**（要求纯数字 —— 本机就是 `100`）；
3. 回落到 OpenHarmony 的 uid 编码规则推导：`userId = uid / 200000`
   （HiShell 的 20020085 → 100；第二账号下的应用 202xxxxx → 101）。

发生转换时会打印一行提示：
`（账号 id=100，取自 $USER（回落到 uid 20020085 / 200000））`。
README 只保留用户能感知的那一层：id 取自 `$USER`、多账号第二个是 `101`、
`HVM_USER_ID` 可覆盖、转换时有提示。

### 4.1 记录备查：受限的 sudo 域

`sudo` 得到的是 `u:r:sudo_execv_label:s0`：读不了别的域的 `/proc/<pid>/status`、
读不了 `dmesg`（拿不到 AVC）、连 `/data/log/hwf_service` 都进不去；
`su hwf_service` 不存在，toybox 版 `chcon` 不支持 `--reference`，
且 hmdfs 上 `chmod` 报告成功但权限不变 —— 所以"改标签 / 进服务命名空间"两条路都断了。
**正解不需要 root。**

### 4.2 磁盘不用自己准备

框架按 `CfgInfo.diskSize`（CfgInfo+20，单位 GB，对应 `--disk-gb`）自行创建稀疏 qcow2：

```
/data/service/el0/virt_service/<账号 id>/vm_manager/<hash>/<vm 名>/img/vm.qcow2
```

随写增长。README 保留"磁盘不用自己准备"与 `disk path` 查出来的实际路径。

## 5. 验证配方

- **看引擎进程与完整命令行**（能同时确认两张光盘的 `file=`）：

  ```console
  $ pgrep -a stratovirt | grep <vm 名字>
  ```

  `create` 那次启动时命令行里能看到两张光盘的 `file=`；`media=cdrom` 计数应为 **2**，
  之后的 `start` 为 **0**。
- **看状态与 PID**：`./hvm-cli vms <名字>`（状态 9 = 运行中）、`./hvm-cli vminfo <名字>`
  给出 PID。
- **挂盘成功的用户态证据**：`vmlog` 里 `Permission denied` 计数为 0；
  热插拔成功时 `vmlog` 里出现 QMP 的
  `blockdev_add { node_name: "cdrom-drive1" }` 与 `device_add { driver: "usb-storage" }`。
- **客户机串口的落点**：`-serial redirect-to-log` → `/data/log/hwf_service/vmlog`，
  客户机的控制台文本就夹在引擎自己的日志行之间；
  `./hvm-cli vmlog` 过滤出客户机串口，`strings /data/log/hwf_service/vmlog`
  看引擎侧（stratoVirt / hwf_service）的原始日志。
- **`405 (VM_IP_UNAVAILABLE)` 不是启动失败**：那只是"启动后立刻查客户机 IP 没查到"，
  虚拟机通常已经跑起来了。

## 6. 其它服务端细节（API 名与日志痕迹）

### 6.1 停止的 API 与日志

两者差别很关键（实测）：

| 命令 | 服务端 API | 生效条件 |
|---|---|---|
| `stop` | `StopVm(name, clean)` | **需要客户机里的 GuestAgent 在线** —— 它本质是"请客户机自己关机"。客户机没起来（例如停在 GRUB）会返回 `405`，服务端日志是 `HostService has lost connection with GuestAgentService` / `system power request calling failed` |
| `force-stop` | `ForceStopVm(name)` | 直接关掉，实测任何状态下都能停（我们自己那台停在 GRUB 时也只有它能停） |

README 保留用户能感知的部分：`stop` 需要客户机配合、`force-stop` 总能停、
`stop` 失败会返回 `405`。

### 6.2 参数存档

`start` 时省略的字段，服务端用**创建时存档的值**（实测：省略 `--cpu` 时仍按存档的 6 核启动，
省略 `--bios` 时仍用存档的固件路径）。`--mem` 至少要给：服务端不接受内存为 0
（只给 `--name` 会返回 `invalid memory size: 0`）。

## 7. 开发与验证命令

这些是逆向与自检用的，用户日常操作不需要它们：

| 命令 | 用途 |
|---|---|
| `ctor [选项]` | 只构造 `CfgInfo` 入参对象并打印字段布局，**不调服务端**（验证构造配方） |
| `view-state <0\|1\|2>` / `displays <id>` | 上报 `HapViewState` / 显示器 id 列表（研究视图机制用） |
| `serial-read` / `serial-write` | 读写客户机通道（`ChannelInfo`） |
| `sha256 <文件> [线程数]` | 内置 SHA-256：多线程预读 + ARMv8 加密指令（实测 ~1.8 GB/s），也用于 `import` 自动算摘要 |
| `hash-name` | 迁移用的 Hash 名（`GetHashName`；未发起过迁移时为空串） |
| `selftest` | 两条通路的加载自检（`hvm-cli selftest` / `openeuler selftest`） |
| `share-volumes` | 列出全部共享卷（`GetAllSharedVolume` 的返回元素类型未还原，命令保留但直接报错） |
| `linux-path <宿主路径..>` | 宿主路径 → 客户机内路径；服务端只允许 LinuxFusion 服务调用，HiShell 身份会被拒 |
| `buffer avail\|low <字节>` | 上报内存阈值（语义是"应用上报"，不是查询，慎用） |
| `perf <a> <b>` / `perf-ex …` | 性能请求；服务端返回 `permission denied` |
| `make symcheck` | 核对生成的 mangled 名与设备符号快照是否一致 |

## 8. 目录结构

| 路径 | 说明 |
|---|---|
| `src/main.cpp` | `hvm-cli` 命令入口（含媒体库视图路径转换） |
| `src/hvm_client.h/.cpp` | vm_manager 客户端封装（dlopen + dlsym → 类型化接口，已接 87/120 个方法） |
| `src/cfginfo.h/.cpp` | 手工构造私有类：`CfgInfo` / `MigrationOptions` / `ChannelInfo` / `PortInfoList` |
| `src/fusion_pty.h/.cpp` | LinuxFusion PTY 通道封装（`openeuler` 的底层） |
| `src/openeuler_main.cpp` | `openeuler` 命令入口 |
| `include/` | **逆向还原的公共头文件**（可直接 `#include` 调用私有库），见 [`include/README.md`](../include/README.md) |
| `docs/api-notes.md` | 逆向笔记 §1–§14：白名单、ABI 陷阱、线上格式、安装介质路径、通道、能力边界、接口覆盖 |
| `docs/abi/vm_manager_client_wrapper.symbols.txt` | 设备导出符号快照（121 个方法与 mangled 名，生成器的核对基准） |
| `scripts/gen-wrapper-api.py` | 生成 `vm_manager_kits.h` 与 `.syms.h`；借 ABI shim 让**编译器**产出 mangled 名并与设备核对 |
| `scripts/abi-shim/__config_site` | 强制 `_LIBCPP_ABI_NAMESPACE __h`，使本机 clang 产出的符号名与设备库一致 |
| `scripts/build-deb12min.sh` | 从零构建最小 Debian 12 arm64 qcow2（可导入、可发布） |
| `scripts/hwdbg.sh` | 沙箱内可用的 lldb 调试封装（CodeArts IDE 提供的 lldb-server） |
| `scripts/check-no-binary.sh` | 提交前检查：仓库内不得有二进制文件 |

## 9. 实现状态

- [x] `hvm-cli`：状态/能力/电源/快照/共享目录/网络/磁盘/显示/内存
- [x] `hvm-cli`：**创建 / 启动 / 销毁虚拟机**（`CfgInfo` 手工构造，实测 `CreateVm` 返回 0）
- [x] `hvm-cli`：安装盘与扩展盘挂载 + 运行中热插拔（`mount-cd` / `unmount-cd`）。实测：**`create` 那次启动**两张盘都会挂上，之后的 `start` 不再挂盘，改用 `mount-cd`；ISO 路径会自动转成媒体库视图
- [x] `hvm-cli`：主机 ↔ 客户机通道（`ChannelInfo` + `Send/RecvDataFromVm`）
- [x] `hvm-cli`：LinuxFusion / RGM 运维面（`pause`/`resume`/剪贴板/图库/客户机磁盘共享/
      自动暂停之外的 23 个 kit 接口；kit 120 个方法已接 **78** 个）
- [x] `openeuler`：`exec` / `shell` / 共享目录 / 镜像安装
- [ ] `DeviceInfo`(0x260) 内部字段逐个还原（各 `Unwrap*Device`）
- [ ] 事件回调（`RegisterVmStatusCallback` / `IVmEventListener` 等）
- [ ] 自建虚拟机的画面与键鼠 —— **架构上不可达**，见[能力边界](../README.md#能力边界)

## 10. 调试（lldb / hwdbg）

DevBox、Harmonybrew 和 OHOS-SDK 的 `lldb` / `lldb-server` 在应用沙箱里 `ptrace` 会被拒。
可改用**应用商店里的 CodeArts IDE**（`com.huawei.codearts`，
注意与白名单一节提到的 `com.huawei.codearts.agent` 是两个应用）自带的 `huawei-debug-lldb-server`：
它躺在 CodeArts IDE 自己的沙箱里，要在 **CodeArts IDE 的终端**里拷出来：

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

## 11. 与 openEuler 构建环境打交道的两条硬规则

这两条是在真机上反复踩出来的（构建两次"神秘死亡"就是违反第一条导致的），记下来免得再犯。

### 11.1 `./openeuler exec` 必须前台阻塞到任务结束

openEuler 那台虚拟机由 **LinuxFusion（HiShell 的自动启停机制）** 管理，它的存活取决于
**有没有一个会话持有着它**：

* `./openeuler exec '…'`、`./openeuler shell`、打开 openEuler 终端标签页 —— 都会**自动开机**，
  并且在**该会话存续期间**让虚拟机保持运行；
* **exec 一退出**，此时若没有任何标签页持有它，**HiShell 就会把虚拟机关掉**。

因此：

* 想让虚拟机活到任务结束，`exec` 就**必须一直阻塞**（把整条任务写在同一次 exec 里、
  前台跑完再返回）；
* **`nohup` / `setsid` / `systemd-run` 这类后台保活全部无效** —— 它们只保证"进程不被
  SIGHUP 杀掉"，可 exec 一退宿主是把**整台机器**关掉，进程自然一起没；
* 症状很好认：任务**无声消失、日志停在半路、没有任何报错**（不是 OOM，也不是脚本问题）。

### 11.2 关掉所有 openEuler 标签页 = HiShell 自动关机

* 打开标签页会**自动开机**；关掉**所有** openEuler 标签页会**自动关机**（HiShell 的行为）；
* **`./openeuler exec` 阻止不了这次关机** —— 长任务可能被这一下打断；
* 对策：跑长任务时**留一个终端标签页**（或直接用 `./openeuler shell` 在前台跑），
  并接受"任务仍可能被用户操作杀掉"；被杀就重跑即可，构建脚本本身可重复。

### 11.3 串口日志里的中文乱码（顺带记录）

`vmlog` 里中文曾显示成 `å®è£…` 这种样子。按字节定位后确认：**ISO 里那份安装器脚本是标准
UTF-8**（`e5 ae 89 e8 a3 85` = 「安装」），但 `hvm-cli vmlog` 读到的字节**已经是**
"UTF-8 被当成 Latin-1 解释、再按 UTF-8 编码"的形态 —— 所以**不是终端显示的问题**，
而是数据在「客户机 → 串口 → 日志」这条路上被某一层做了字符集转换。

`vmlog` 现在做一次**无损逆变换**（把码位 ≤0xFF 的字符映射回单字节）还原它：Latin-1 在
`0x00–0xFF` 上是双射，因此不丢信息；仅在"整行都在 Latin-1 范围且还原后是合法 UTF-8"时启用。
验证：同一份日志样本按字节统计，乱码形态 **18 → 0** 处、正常 UTF-8 中文 **0 → 22** 处。
详见提交 `4d1cc40`。
