# 逆向笔记

本文件记录 hvm-cli 背后的调查结论，便于后续扩展（尤其是 `StartVm`）。

## 1. 目标环境

| 项 | 值 |
|---|---|
| 设备 | HUAWEI MateBook Pro，`const.product.model=HAD-W32` |
| 系统 | HarmonyOS `7.0.0.107(SP7C00E100R13P5)`，`const.ohos.apiversion=26` |
| 内核 | Linux 6.6.89 aarch64 |
| 固件分析样本 | `HAD-LGRP3-CHN 207.0.0.107(SP7)`（`system.img` 等，EROFS/ext4） |
| 终端身份 | HiShell 应用 uid `20020085` |

## 2. 虚拟机技术栈

```
ArkTS/HAP
  └─ libvmmanager_napi.z.so / libvmmanagerinner_napi.z.so
     liblinuxvmmanager_napi.z.so / libvirtmanager_napi.z.so
        └─ libvm_manager_kits.z.so            ← 本工具直接调用这一层
             OHOS::VmManagerService::VmManagerClientWrapper
             OHOS::VmManagerService::VmManagerProxy      （IPC 客户端）
             └─ Binder IPC（/dev/binder）
                  └─ SA 65621 vm_manager（libvm_manager.z.so）
                       进程 vm_manager，uid/gid hwf_service(7700)
                       SELinux: u:r:ohos_vm_manager:s0
                       └─ StratoVirt 2.5.0（本仓库之外的引擎层）
                          /system/bin/virt_service/vm_engine/stratovirt/stratovirt
                          后端 HMV（/dev/hmv）或 KVM（/dev/kvm，本机无）
                          控制面 QMP over unix socket
                          配套：virtiofsd / vhost-device-gpu / stratovirt-img / virt_fw_vars
```

补充组件：

- `/system/bin/virt_service/rgm_engine/isula/bin/{isulad,lxc-start,lxc-attach}`：
  容器引擎（openEuler 22.03 根文件系统），是另一条"Linux 兼容"路线。
- `profile/vm_manager.json`：SA 描述文件，`name=65621`、`libpath=libvm_manager.z.so`、
  `start-on-demand.bindercall=true`（首次调用按需拉起）。
- 接口描述符（UTF-16 常量）：
  - `OHOS.VmManagerService.IVmManager`
  - `OHOS.VmManager.IVmManagerCallback`

## 3. 调用者白名单（关键）

服务端 `VmmCommonUtils` 对每个请求做身份校验：

```
CheckCallerIdentity
 ├─ IsLegalCalling
 │   ├─ CheckCallingProcName            → IsleaglSystemAbility(uid, 进程名)
 │   └─ 否则回退到 HAP 身份：GetCallerBundleName + GetAppIdentifier
 │        └─ IsLegalAppIdentifier / IsLegal2BAppIdentifier
 ├─ IsLegal2BCalling
 └─ IsITEnterpriseSpace（企业空间限制）
CheckCallingOpenEulerHap                → appId/bundleName 与常量比对
CheckCallingProcNameFromLinuxFusionService → uid==5005 且进程名匹配
```

`IsleaglSystemAbility` 中出现的 uid 分支：**5005（LinuxFusionService）、7005、7700（hwf_service）**。

服务端日志里有一条明确区分三种身份的记录：

```
isLinuxFusionService:%d, isHiShellHap:%d, isOpenEulerHap:%d
```

即 **HiShell HAP 被单独识别并放行**。实测：以 uid 20020085 从 HiShell 终端
启动的进程调用以下接口全部返回 `rc=0`：

| 调用 | 结果 |
|---|---|
| `GetActiveVmName` | rc=0，name=`""`（本机尚无虚拟机） |
| `CheckVmCapability` | rc=0，cap=`true` |
| `GetVmStatus("virtualized_linux")` | rc=0，status=0 |
| `IsProcessExist("stratovirt")` | rc=0 |

被拒绝时服务端会打 `... permission denied` 日志（hilog）。

**结论：本仓库的两个命令都只能在系统自带的 HiShell 终端里运行 —— 因为白名单里
能被用户使用的终端只有 HiShell 一个。** 白名单其余条目（LinuxFusionService uid 5005、
hwf_service uid 7700、openEuler HAP）都是系统服务或专用应用，用户无法在其中开终端；
从 MKCode / BitFun / WorkBuddy / CodeArts Agent 等第三方应用的内置终端运行会被拒绝。

（`openeuler` 走的融合开发引擎通道同样受身份限制：其 `libfusion_pty_common.z.so`
里也带着 `com.huawei.hmos.hishell` 这个包名。）

## 4. 客户端 ABI 注意事项

- **libc++ inline namespace 是 `std::__h`**（不是 `__1`）：
  符号形如 `_ZN4OHOS16VmManagerService22VmManagerClientWrapper15GetActiveVmNameERNSt3__h12basic_string...`。
  用 clang++ 编译时无需特殊处理，只要用 `std::string` 即可（布局一致）。
- `OHOS::sptr<T>` 是单指针智能指针，`GetInstance()` 的返回值可直接当
  成员函数的 `this` 用。
- 该库被 **strip**（只有 `.dynsym`），`CfgInfo` 等辅助类没有导出符号。
- **重要坑**：把 `libvm_manager_kits.z.so` 用 `dlopen()` 加载进*非原生宿主*
  （例如脚本运行时）会在库的静态初始化阶段段错误；
  同一个库在独立可执行文件里加载、调用均正常。
  因此本工具做成**独立可执行文件**，而不是可被任意宿主加载的插件。

## 5. 已知返回码

均为**实测**得到（括号里是 32 位十六进制，便于与 OHOS 错误码风格对照）：

| 场景 | 返回码 | 说明 |
|---|---|---|
| 成功 | `0` | |
| 虚拟机名非法（长度 / `..`） | `401` (`0x191`) | `CreateVm` 首个参数校验 |
| 镜像/参数路径缺失 | `402` (`0x192`) | `CreateVm` 的 imagePath 读失败 |
| `CfgInfo` 缺失或解析失败 | `404` (`0x194`) | |
| `VM_IP_UNAVAILABLE` | `405` (`0x195`) | 查不到客户机 IP；**启动后立即查 IP 也会返回它**（客户机还没起来），不代表启动失败 |
| 无虚拟机（`GetHostSN` / `GetOpenEulerVersion` 等） | `201` (`0xC9`) | |
| 无虚拟机时的快照查询 | `−117506036` (`0xF8FF000C`) | |
| 已有实例在运行（`CreateVm` / `StartVm` 被拒） | `−16842742` (`0xFEFF000A`) | 服务端 `CheckMultipleVmState:1787 not support multiple vm instances`；注意**多台虚拟机可以并存，但同时只能有一台在运行** |
| 强制删除 RGM 镜像时未找到 | `−1000` (`0xFFFFFC18`) | |
| `RecoverUserData` 失败 | `−16842744` (`0xFEFF0008`) | |
| 端口转发：查询对象不是当前运行实例 | `−151060472` / `−151060470` (`0xF6FF0008` / `0xF6FF000A`) | 服务端 `GetPortForwardForNat:1416 not current running instance` |
| 磁盘容量查询失败 | `−234946554` (`0xF1FF0006`) | |
| 本工具本地错误 | `−1001` kit 未加载 / `−1002` 符号缺失 | 与系统返回码区分 |

`GetVmStatus` 的状态码实测：`0` = 无虚拟机运行、`9` = 运行中。
**服务端没有把枚举名编进二进制**，其余取值需在对应状态下逐个观测补全。

## 6. 创建/启动虚拟机：线上格式与引擎侧线索

`CreateVm(vmName, imagePath, sptr<CfgInfo>)` / `StartVm(vmName, sptr<CfgInfo>)`
需要 `CfgInfo`。客户端 kit 被 strip、该类没有导出符号，因此改为按逆向出的
**内联构造序列**手工构造（配方与实测见第 9 节）。

服务端 `HandleCreateVm` 的线上格式（`libvm_manager.z.so`，实测日志核对过）：

```
ReadString  vmName        → 校验长度与 ".."，非法回 401
ReadInt32   是否有 CfgInfo → 0 或反序列化失败回 404
[CfgInfo]   CfgInfo::Unmarshalling
ReadString  imagePath     → 读取失败回 402
→ VmManager::CreateVm(vmName, imagePath, cfg) 的返回值写回
```

`GetCheckPathInfo` / `CheckFiles` 会校验：`CfgInfo+32`（biosPath）与 `imagePath`
必须存在且可读（`access(R_OK)`），随后 `DetectIsoType` / `GetQcowState` 判定
镜像必须是 **ISO 或 qcow2**。

已知服务端由 `CfgInfo` 生成的命令行包含（来自 `libvm_manager.z.so` 字符串）：

```
-object iothread,id=qmp-iothread -qmp unix:<sock>,server,nowait,iothread=qmp-iothread
-machine ...
-device ohos-vhost-vsock-device,guest-cid=1234,id=developer-vsock
viofsd_bin=/system/bin/virt_service/vm_engine/stratovirt/virtiofsd
,log_level=info,passthrough_fs,cmd_sock=<vfs1.sock>
```

开发者场景（`to_developer`）相关路径：

```
/data/virt_service/to_developer/cmd_sock1.sock, cmd_sock2.sock
/data/virt_service/to_developer/console.sock
/data/virt_service/to_developer/vfs1.sock, vfs2.sock
/data/virt_service/virt_comm/hwf/uds/to_developer/stratovirt.sock
```

StratoVirt 的 QMP 扩展命令（含华为私有）：

```
query-ohui-status  query-workloads  query-processes  set-qos  reset-qos
set-viomem  get-viomem  touch-memory  resolution-change  share-dir-add
virtual-dir-add  guest-drives-share  detect-silent-audio  tpm-reconnect
blockdev-snapshot-internal-sync  trace-get-state
```

## 7. 复现调查的命令

```bash
# SA 注册信息：设备上 /system/profile 对本身份不可读（hdc 的 uid 2000 也一样），
# 改从固件解包里查：
#   <unpack_result>/system/system/profile/vm_manager.json   （SA 65621）
# 同类 VM 相关 SA 还有：hwf_service / linux_fusion_service / exfusion_display_service 等
# 客户端导出符号
nm -D --defined-only /system/lib64/libvm_manager_kits.z.so | grep VmManagerClientWrapper
# 接口描述符（UTF-16LE 常量）：转码后过滤
iconv -c -f UTF-16LE -t UTF-8 /system/lib64/libvm_manager.z.so | grep -ao 'OHOS\.[A-Za-z.]*' | sort -u
# 实测调用
./hvm-cli selftest && ./hvm-cli info
```

## 8. fusion PTY 通道（`openeuler` 命令）

HiShell 的"openEuler 标签页 / 连接 openEuler 执行命令"用的不是 vm_manager，而是
**LinuxFusion PTY**（融合开发引擎，内部代号 **RGM**）：

- 引擎内部命名：SA 65601 `rgm_manager`、SA 65604 `rgm_engine_plugin`、
  镜像 `rgm_linux` / `rgm_hmos` / `rgm_openEuler`、开发包
  `com.huawei.developer.rgm.images_openeuler22.03`
- 代码层命名：`OHOS::ContainerEnginePlugin`，源码路径
  `vendor/huawei/virt_service/container_manager/...`
- 运行时：OzoneC 容器（overlay 路径含 `OzoneC/overlay2/rgm_openEuler/lower`）


- HAP 侧 napi 模块：`@ohos:fusion_pty_napi`（`libs` 里还有 `@ohos:linux_developer_napi`）
- **NDK 入口**：`/system/lib64/ndk/libfusion_pty_ndk.so`
- 配套：`libfusion_pty_{common,manager,manager_client,session,session_client}.z.so`
- 传输：virtio-vsock（HiShell 字节码里可见 `VSOCK` / `PTY_SERVER` 字样；
  固件里 stratovirt 的设备串为 `ohos-vhost-vsock-device,guest-cid=1234,id=developer-vsock`）

SDK 不提供头文件，以下 C ABI 与结构是从二进制恢复的
（`libfusion_pty_ndk.so` 反编译 + 实测）：

```c
int  OhGetPtyManager(void **outManager);              // 注意：带出参，不是返回值
int  OhPtyManagerOpenPtySession(void *mgr, void **outSession,
                                OhPtyCallback *cb, OhPtyConfig *cfg, int *outSessionId);
int  OhPtySessionSendData(void *session, const char *data);   // 需以 \0 结尾
int  OhPtySessionSetWinSize(void *session, OhPtyWinSize *ws);
int  OhPtySessionGetSessionId(void *session, int *id);
int  OhPtySessionClose(void *session);
int  OhPtyManagerInstallImage(void *mgr);
int  OhPtyManagerEnableShareFolder(void *mgr);
int  OhPtyManagerGetSharedFolderToggleState(void *mgr, bool *enabled);
```

结构布局（由 `OhPtyManager::GetInnerConfig` 反推）：

```c
typedef struct {                    // 大小 64
    const char *f0;                 // → 内层第 1 个 string（默认 "/bin/bash"）shell 路径
    const char *f1;                 // → 内层第 2 个 string（语义未知）
    const char *f2;                 // → 内层第 3 个 string
    const char *f3;                 // → 内层第 4 个 string
    const char *f4;                 // → 内层第 5 个 string（默认 "openEuler"）
    const char *f5;                 // → 内层第 6 个 string（默认 "root"）登录用户
    uint32_t rows, cols, xpixel, ypixel;   // → PtyWinSize（Parcel 里也是这 4 个 uint32）
} OhPtyConfig;

typedef struct { void *onRecv; void *onSignal; void *onStatus; } OhPtyCallback;
```

回调签名（由 `InnerPtySessionCallback::On*` 的转发代码确认，**均无长度参数**）：

```c
void onRecv  (void *session, int sessionId, const char *data);  // data 为 C 字符串
void onSignal(void *session, int sessionId, int signal);
void onStatus(void *session, int sessionId, int status);        // 1=就绪 2=会话结束
```

三个实测踩坑：

1. `OhGetPtyManager` 是**带出参**的（`int OhGetPtyManager(void**)`）。
   当成无参返回值调用会立刻段错误。
2. 库内部虽然打了 `exitCode` 日志，但**不会把它转发给用户回调**，
   所以远端 `exit N` 拿不到 N，只能知道"会话已结束"。
   同理，用 `echo MARKER$?` 探测结束时要让标记在 shell 侧拼出来
   （如 `M=__X; echo ${M}_START; cmd; echo ${M}__$?`），
   否则命令回显里就会出现同样的字面量，导致误判"命令已结束"。
3. **过早发送会被静默丢弃**：会话刚 `OpenPtySession` 成功时远端还没就绪，
   `SendData` 甚至可能返回 0 但数据进不到 guest。
   可靠的就绪信号是"远端首次产生输出"（欢迎信息/提示符），
   实测 `shell` 需要先等到它再开始透传。

另外，`shell` 只做数据面：本地 tty 切 raw + 双向字节透传 + SIGWINCH 同步窗口尺寸，
不做任何行缓冲/回显/退格处理 —— 行规程属于远端 bash/readline 的职责。
（早期版本在本地实现了残缺的行规程，导致整行重复下发、`uname -r` 被粘连成
`uname -runame -r` 之类的现象。）


## 9. CfgInfo 手工构造（实测记录）

`vm create` 已打通到服务端业务校验，过程与踩坑记录如下。

### 构造序列

`CfgInfo` 没有独立构造函数（被内联展开），需按反汇编逐条复刻，完整配方见
`include/ohos/vm_manager_service/cfg_info.h`。对象大小 `0x300`，关键成员：
`DeviceInfo`（+88，0x260）、`BundleInfo`（+696，内含 RefBase@+736）、
`CfgInfo` 自身的 RefBase（+752）。

### 两个真实踩坑

1. **`Parcelable` 有虚基类**，其构造函数签名是 `Parcelable(this, vtt)`。
   只传 `this` 会因 x1 为垃圾值而崩溃。需要的 VTT：
   CfgInfo → `0xB0498`，BundleInfo → `0xB0658`（均在 [N] 库中）。
2. **[N] 的 `.gnu_debugdata` 里有 "RefBase::RefBase" 之类符号，但它们指向
   `.bss` 槽位而不是代码**，按该地址调用必段错误。真实的 `RefBase`/`Parcelable`
   构造要 `dlsym(RTLD_DEFAULT, "_ZN4OHOS7RefBaseC2Ev" / "_ZN4OHOS10ParcelableC2Ev")`
   （由 libutils 提供）。

### 服务端校验链（实测日志）

```
hvm-cli    [CreateVm:213]        [virtio-mem] write cfgInfo      ← 客户端 marshal 成功
vm_manager [HandleCreateVm:300]  [virtio-mem] Read cfgInfo       ← 服务端 unmarshal 成功
vm_manager [IsValidPath:81]      Standardized path fail!         ← bios/image 路径
vm_manager [CheckParams:110/124] invalid memory size / disk size ← 单位：内存 GB、磁盘 MB
vm_manager [CheckFiles:159]      no such file.                   ← access(path, R_OK)
vm_manager [DetectIsoType:148]   file is not iso.                ← 镜像类型
vm_manager [CheckWinImgPath:175] not image
```

### 调试器：用华为自带的 lldb-server

系统自带 `/data/service/hnp/bin/lldb-server` 在当前身份下会
`ptrace failed: Permission denied`（应用沙箱禁 ptrace），
`hdc shell`（uid 2000）又处在另一个挂载命名空间、且 `/data/local/tmp` 不可执行。

**可行方案**：应用商店里的 **CodeArts IDE**（`com.huawei.codearts`）自带一个只依赖 musl libc 的自包含
`huawei-debug-lldb-server`。它位于 CodeArts IDE 自己的沙箱里，需要在
**CodeArts IDE 的终端**里拷到用户目录：

```console
$ mkdir -p ~/.local/bin
$ cp /data/storage/el2/base/files/huawei-debug-lldb-server ~/.local/bin/
```

之后用 `~/.local/bin/huawei-debug-lldb-server`（即可），
可以正常拉起进程被 lldb 调试。仓库脚本：

```bash
scripts/hwdbg.sh ./hvm-cli 7799                       # 起服务并连上 lldb
scripts/hwdbg.sh ./hvm-cli 7799 -o "b main" -o continue
```

要点：进程由 lldb-server **预先拉起并停在动态链接器入口**，
所以下完断点要用 `continue` 恢复，**不要用 `run`**。

验证示例（断在 `CfgInfoBuilder::setCpuNum`）：

```asm
hvm::CfgInfoBuilder::setCpuNum(int):
  ldr x8, [x0, #0x10]     ; x8 = builder->obj_
  str w1, [x8, #0xc]      ; *(int*)(obj_ + 12) = cpuNum
  ret
```

即运行时写入的偏移与逆向结论 `cpuNum@+12` 完全一致。
调试动态库时可用 `image list -f -o` 取加载基址，
再加逆向出的静态偏移下断点（`br set -a <base+offset>`）。

### 一次成功的创建（实测记录）

用下面这条命令完整走通了 `CreateVm`（返回 rc=0）：

```bash
./hvm-cli vm create --name win11 \
  --image   /data/service/el2/100/hmdfs/account/files/Docs/Download/<app>/Win11_....iso \
  --enhance /data/service/el2/100/hmdfs/account/files/Docs/Download/<app>/oetool.iso \
  --bios /system/opt/virt_service/virtualized_hwf/stratovirt-vars \
  --cpu 6 --mem 8 --disk-gb 128 --apply
```

创建后：

```
活动虚拟机 : win11
磁盘镜像   : /data/service/el0/virt_service/100/vm_manager/177c554b/win11/img/vm.qcow2
状态码     : 9（实测：stratoVirt 进程在跑、vm-info 能取到 PID）
```

服务端校验链的**完整顺序**（日志逐条对应，便于排错）：

```
IsValidPath(81)          路径 realpath（服务端命名空间）
DetectIsoType(144/148)   路径有效 + 必须是 ISO
CheckFiles(154/159)      存在 + access(R_OK)
CheckParams(110/124)     memory(GB) / disk(MB, >=0x10000) / cpu
CheckEnhanceFilePath     enhance 必须非空且扩展名为 .iso
CheckWinImgPath(175)     Windows 安装镜像判定
→ 建目录、写设备 JSON、配网、注册回调 → rc=0
```

**三个坑**（都实测踩过）：

1. 镜像路径必须写成**媒体库视图** `/storage/media/<账号 id>/local/files/Docs/...`
   —— 这是唯一让 `vm_manager` 与 `stratovirt` 两个域都能读的形式，详见第 10 节。
   （`/storage/Users/...` 会让 `realpath()` 直接失败并报 `Standardized path fail!`；
   hmdfs 真实路径虽然能过校验，但 `stratovirt` 打开时被 SELinux 拒绝。）
2. `enhanceFilePath`（CfgInfo+56）**不能为空**且必须是 `.iso` 文件，
   否则 `create vm fail, enhance file path is null`。
3. 磁盘不用自己造：框架按 `diskSize` 生成 `.../img/vm.qcow2`（稀疏，随写增长）。

### 安装介质与客户机通道

- 安装介质路径：见第 10 节（必须写成**媒体库视图**，`hvm-cli` 会自动转换）；
- 客户机串口/通道：见第 12 节。

### 仍未完成

- 提供真实 ISO/qcow2 镜像后即可完成一次完整创建；
- `DeviceInfo`（0x260）内部字段尚未逐个还原（各 `Unwrap*Device` 函数在 [N] 中）；
- 事件回调（`RegisterVmStatusCallback` 等）。

## 10. 安装介质路径：三种写法与正解（实测）

给虚拟机挂安装盘（`--image X.iso`）时，服务端会把该路径交给 `stratovirt` 打开。
实测**只有"媒体库视图"这一种写法同时满足两个进程的可读性**：

| 写法 | `ohos_vm_manager`（校验阶段） | `ohsw_stratovirt`（真正打开光盘） |
|---|---|---|
| `/storage/Users/currentUser/Download/x.iso` | ✗ 它的命名空间里没有这个挂载 | — |
| `/data/service/el2/100/hmdfs/account/files/Docs/Download/x.iso`（hmdfs 真实路径） | ✓ | ✗ `Permission denied` |
| **`/storage/media/100/local/files/Docs/Download/x.iso`（媒体库视图）** | ✓ | **✓** |

### 10.1 正解：媒体库视图

- 发现方式：抓一个**正在安装 Windows 的第三方应用虚拟机**（`com.sanway.ecoengine`）
  的 `stratovirt` 完整命令行，它的两张光盘都是这个形式：

  ```
  if=none,id=disk,format=raw,media=cdrom,file=/storage/media/100/local/files/Docs/Download/<bundle>/Win11_....iso
  if=none,id=unattend,format=raw,media=cdrom,readonly=true,file=/storage/media/100/local/files/Docs/Download/<bundle>/server.iso
  ```

- 换成该写法后：`IsValidPath` / `DetectIsoType` / `CheckFiles` 全部通过、`CreateVm` 返回 0、
  启动后两张光盘都出现在 stratovirt 命令行里、`vmlog` 里 `Permission denied` 计数为 0；
- 服务端库里本来就有对应白名单正则：`^/storage/media/\d+/local/files/Docs/`；
- `hvm-cli` 已内置转换：`/storage/Users/currentUser/<p>` 与
  `file://docs/storage/Users/currentUser/<p>` → `/storage/media/<账号 id>/local/files/Docs/<p>`。

  路径里的那个数字是 **OS 账号 id**，本机 HiShell 终端里就是环境变量 `$USER`（实测 = 100）。
  `hvm-cli` 的取值顺序：

  1. `HVM_USER_ID`（显式覆盖，便于引用别的账号视图下的文件）；
  2. **`$USER`**（要求纯数字 —— 本机就是 `100`）；
  3. 回落到 OpenHarmony 的 uid 编码规则推导：`userId = uid / 200000`
     （HiShell 的 20020085 → 100；第二账号下的应用 202xxxxx → 101）。

  预演时会打印实际取值：`（账号 id=100，取自 $USER（回落到 uid 20020085 / 200000））`。

### 10.2 为什么另外两种写法不行

**SELinux 域隔离**（针对 hmdfs 真实路径）：

```
(typetransition ohos_vm_manager ohsw_stratovirt_exec process ohsw_stratovirt)
```

| 阶段 | 域 | 结果 |
|---|---|---|
| `IsValidPath` / `DetectIsoType` / `CheckFiles` | `ohos_vm_manager` | 能读 hmdfs ✓ |
| `stratovirt` 打开光盘文件 | `ohsw_stratovirt` | hmdfs 一律 `Permission denied` ✗ |

**挂载命名空间隔离**（针对 `/storage/Users/...`）：服务进程看不到该挂载，
`realpath()` 直接失败（日志 `IsValidPath:81 Standardized path fail!`）。
实测应用沙箱 `/data/storage/el2/base/files/...`、`/data/local/tmp`、`/dev/shm`
也都过不了 `IsValidPath`。

### 10.3 记录备查：受限的 sudo 域

`sudo` 得到的是 `u:r:sudo_execv_label:s0`：读不了别的域的 `/proc/<pid>/status`、
读不了 `dmesg`（拿不到 AVC）、连 `/data/log/hwf_service` 都进不去；
`su hwf_service` 不存在，toybox 版 `chcon` 不支持 `--reference`，
且 hmdfs 上 `chmod` 报告成功但权限不变 —— 所以"改标签 / 进服务命名空间"两条路都断了。
**正解不需要 root。**

## 11. MigrationOptions 与 ImportVmDiskImage（磁盘迁移通道，非通用搬运）

### 11.1 类布局与构造配方（[N] = libvmmanager_napi.z.so）

取自 [N] `WindowsFusionNapi::OnImportVmDiskImage` 的内联构造现场（反汇编 0x989E4~0x98A3C）：

```c
p = operator new(0x40); memset(p, 0, 0x40)
RefBase::RefBase(p + 48)
Parcelable::Parcelable(p, baseN + 0xB0698)      // 第 2 参数是 VTT
*(void**)(p + 0)  = baseN + 0xB4790             // 主 vtable 地址点
*(void**)(p + 48) = baseN + 0xB47F0             // = 主 vtable + 96
RefBase::IncStrongRef(p + 48, &holder)
```

字段（取自 [N] `UnwrapMigrationOptions`）：

| 偏移 | 类型 | 名称 |
|---|---|---|
| +10 | bool | `isKeepSnapshots` |
| +11 | bool | `hasCallback` |
| +16 | `std::string` | `password`（24 字节，+16..+39） |
| +40 | bool | `isForceImport` |

已实现：`src/cfginfo.{h,cpp}` 的 `MigrationOptionsBuilder`、`hvm-cli vm import` / `vm export`。

### 11.2 服务端三道闸门（实测，逐步放行）

```
1) [(HandleImportVmDiskImage:1086)]       MigrationOptions is nullptr.            → 传入构造好的对象后消失
2) [(CheckVmStateForMigration:1933)]      the state of virtual machine not stopped → 停机后通过
3) [(CheckBeforeImportVmDisk:1973)]       src disk img was broken                  → 过不去
```

第 3 条的根因（关键）：`VmmCommonUtils::GetGuestType` **并不解析 qcow2**，而是

```c
key = <hwf.* 前缀> + 磁盘路径;
SettingProvider::GetIntValue(instance, key, &type);   // 从系统设置库查"客户机类型"
```

只有服务端自己创建的磁盘才有这条记录 → 用户区的文件必然判 broken；
该检查还带 `IsLegalUosCalling` 分支（UOS 迁移场景）。
`VmAssistantManager::CopyFile` 的调用者**只有** `ImportVmDiskImage` / `ExportVmDiskImage`
（xref 确认），因此服务端**不存在**"用户区 → 服务区"的通用文件搬运通道。

实测：标准 `qemu-img` 空白 qcow2（v3、100 GB 虚拟大小、198 KB 稀疏）与 ISO 都报同样错误。

## 12. 主机 ↔ 客户机通道（ChannelInfo / SendDataToVm / RecvDataFromVm）

### 12.1 ChannelInfo 布局（取自 [S] `ChannelInfo::Unmarshalling` / `Marshalling`）

```c
p = operator new(0x38); memset(p, 0, 0x38)
RefBase::RefBase(p + 40)          // 注意在 +40（不是 +48）
*(void**)p = baseN + 0xB4910      // 主 vtable 地址点（不需要 VTT）
*(uint32*)(p + 12) = 通道类型
new (p + 16) std::string(通道名)
```

通道类型取自 [N] `VmManagerChannelTypeInit`（JS 枚举 `ChannelType`）：`SERIAL = 0`；
napi 侧属性名为 `channelName` / `channelInfo`。

### 12.2 两个接口的实测要点

- `RecvDataFromVm(vm, vector<uint8_t>&, int, ChannelInfo)` **必须预先给足缓冲区**，
  否则服务端报 `RecvDataFromVm:1048 data size invalid.`（402）；预分配后进入正常读取路径；
- 第三个 `int` 参数与缓冲区大小相关（实测传 4096 有效）；
- 已实现：`ChannelInfoBuilder`、`Client::sendDataToVm` / `recvDataFromVm`、
  `hvm-cli vm serial-read` / `vm serial-write`（`--chan/--type/--arg/--data`）。

### 12.3 客户机的两条"串口"

| 通道 | 形态 | 说明 |
|---|---|---|
| 传统串口 | `-serial redirect-to-log` → `/data/log/hwf_service/vmlog` | **只读** ✓（该目录属组 `log`，本工具在组内）。GRUB 菜单与客户机控制台文本都会出现在这里（被包在 `chardev.rs` 的日志行里）——这是**能看到客户机文本界面**的唯一途径 |
| virtio-serial | `virtserialport id=winbox_serial0/1 nr=1/2` → 服务数据区 `uds/serial0.sock` / `serial1.sock` | **双向** ✓，走上面两个 API；实测读报 `RecvDataFromVm:1079 recv data failed` —— 该端口是 HWF 的 Windows 客户机代理通道，Linux 安装器不会打开它 |

`-serial redirect-to-log` 是**单向**的（没有输入路径），kit 里也没有键盘注入接口
（`usb-kbd` / `usb-tablet` / `virtio-multitouch` 都由被沙箱挡住的 `-display ohui` / QMP 驱动），
因此**无法向 GRUB 菜单或安装器发送按键**。

## 13. 能力边界：白名单 / 窗口 / 显示（为什么"画面 + 键鼠"绕不过去）

三层门叠在一起，这是本仓库探索到的最终边界。

### 13.1 调用者白名单（vm_manager 侧）

`VmmCommonUtils::CheckCallerIdentity` → `IsLegalCalling()`，按 **appIdentifier** 判定
（`IsLegalAppIdentifier` / `IsLegal2BAppIdentifier` / `IsLegal2BCalling` / `IsLegal2CCalling`，
2B = 厂商合作应用）。实测：

- 本仓库的 CLI **能过** —— 它跑在 **HiShell 的 uid** 下（终端里的子进程继承该身份）；
- `com.oseasy1.ohvm`、`com.sanway.ecoengine` 能过（厂商合作应用，凭签名身份）；
- **自己写的 HAP 过不了**（appIdentifier 不在名单里，签名无法伪造）。

### 13.2 系统窗口只能由 UIAbility 创建

```cpp
// libwm.z.so 导出
Window::Create(sptr<WindowOption>&, shared_ptr<AbilityRuntime::Context> const&,
               sptr<IRemoteObject> const&, WMError&, string const&, bool)
```

`AbilityRuntime::Context` 只有被 AAFwk 拉起的 UIAbility 才有。NDK 侧
（`native_window/external_window.h`）只有"**从已有 surfaceId 包装**"的接口：

```c
OH_NativeWindow_CreateNativeWindowFromSurfaceId(uint64_t surfaceId, OHNativeWindow** out);
```

即"先有窗口才有 surface，反过来不行"；`oh_window.h` 里没有任何创建接口。
→ **裸 ELF 拿不到窗口令牌，创建不了系统窗口。**

### 13.3 虚拟机画面是"合成进应用窗口"的

VM 的显示后端是私有 `-display ohui,iothread=...,socks-path=<服务数据区>/uds`。
用户四指右滑进全屏时，`vmlog` 里能看到完整证据链：

```
ui/src/ohui_srv/msg_handle.rs:732   WindowInfoExtensionEvent { surface_width: 3120, surface_height: 2080, rotation: 0 }
ui/src/ohui_srv/msg_handle.rs:463   received focus-in event
devices/src/display/svga/processor_dx/svga_dx_backend.rs:444   Swipe in, flush last frame to enable dss composition.
ui/src/ohui_srv/msg_handle.rs:466   received focus-out event
```

即：系统合成器（DSS）把 OHUI 的帧合成进**应用窗口**，窗口全屏由系统手势管理
（所以关掉 HAP 之后仍然可用）。**没有窗口就没有可滑进去的表面** —— 这正是我们自己
启动的虚拟机四指右滑无效的原因。kit 的 121 个方法里也没有截屏接口；QMP socket
（`screendump` / `input-send-event`）只有 hwf_service 域能进。

### 13.4 顺带结论：各家应用的虚拟机互相隔离

- 活动虚拟机名是全局的（能看到 `com.oseasy1.ohvm`）；
- 但**别的应用创建的虚拟机**对我们等于不存在：`disk path` 为空、`disk size` 为 0、
  `net ip` 返回 405、`snapshot list` 报 `the qcow2 does not exist`；
  服务端 `CheckMultipleVmState` 里确实用 `GetAppIdByCallingUid` + MD5 计算调用方身份；
- 多台虚拟机**可以并存**（实测同时存在三台），但**同时只能有一台在运行**。

### 13.5 因此可行的用法只有两种

| 目标 | 做法 |
|---|---|
| 交互式装系统 / 看画面 | 只能借**厂商合作应用的界面**（OSEasy / Sanway）：用它们的 UI 新建一台虚拟机、镜像选下载目录里的 ISO |
| 自动化控制**自己的**虚拟机 | 用本仓库的 `hvm-cli`（借 HiShell 身份过白名单）：create / start / stop / 磁盘 / 快照 / 网络端口转发 / 共享目录 / 通道读写 |

## 14. LinuxFusion / RGM 接口清单与覆盖情况

### 14.1 两条通路的接口面

| 通路 | 入口 | 我们用到 | 备注 |
|---|---|---|---|
| 融合 PTY（NDK） | `libfusion_pty_ndk.so` 的 10 个 `Oh*` 函数 | 9 个 | 只差 `OhDeletePtyManager`（依赖进程退出回收） |
| 融合 napi（应用侧 JS） | `liblinuxfusionservice_napi.z.so` / `liblinuxvmmanager_napi.z.so` | 0 | 见 14.3 |
| vm_manager kit | `VmManagerClientWrapper` 120 个方法 | **78** | `hvm-cli` 借 HiShell 身份调用 |

### 14.2 已接的 78 个方法里，本批新增 23 个

```
PauseVm ResumeVm LockGuest LxOtaHandle DeleteRgmImageFromVm
SetHostGallerySharedEnabled SetGuestDiskShared SetVmHostNetProxyStatus SetProxyAutoSyncEnabled
Get/SetPasteboardEnableState Get/SetPasteboardUsableState Add/RemovePasteboardSharedFolder
VmUniSocPerfRequest VmUniSocPerfRequestEx SysAvailBufferLimit SysLowBufferLimit
ToggleScreenLockTask TabletSwitchChanged
```

对应命令：`pause` / `resume` / `lock-guest` / `lx-ota` / `rgm-image-delete` /
`gallery-share` / `guest-disk-share` / `net proxy-status-on|off` / `net proxy-auto-on|off` /
`pasteboard …` / `perf` / `perf-ex` / `buffer avail|low` / `screen-lock-task` / `tablet`。

实测状态（其余因怕影响他人正在使用的虚拟机，只做了"符号与调用链"冒烟测试）：

| 命令 | 实测 |
|---|---|
| `pasteboard status` | ✅ 返回 `开 / 可用` |
| `buffer avail <n>` | ✅ 返回 0（**语义见 14.4**） |
| `share-volumes` | ❌ 见 14.3（命令保留，明确报错） |
| `linux-path <路径…>` | ❌ 见 14.3 |
| `perf <a> <b>` | ❌ 服务端 `permission denied` |

### 14.3 本批撞到的三条限制（重要）

1. **`GetAllSharedVolume()` 的返回元素是私有类。** 它的参数形状与 `GetSharedFolder()`
   完全相同（都无参），差别在返回类型 —— 实测按 `std::vector<std::string>` 解释会
   **段错误**（析构不匹配）。`hvm-cli share-volumes` 因此改为明确报错（rc=3），
   等还原出元素类型再接。
2. **`GetLinuxPathFromOhPath` 只允许 LinuxFusion 服务调用。** 服务端日志：
   ```
   [(CheckCallingProcNameFromLinuxFusionService:780)] GetNativeTokenInfo failed.
   [(GetUpdatedSharedPath:3065)] GetSharedFolder failed
   ```
   即这条通路不止"调用者白名单"一道门，还有**按服务身份**的限制 —— 我们的 HiShell
   身份也会被拒（rc=-1）。
3. **`VmUniSocPerfRequest` 服务端 `permission denied`。**

### 14.4 `SysAvailBufferLimit` / `SysLowBufferLimit` 的真实语义

反汇编 `VmAssistantManager::SysAvailBufferLimit`：它只是 `*((_QWORD*)this + 179) = 参数`
—— 把值**存进服务端成员**，即这两个接口是**应用向服务端上报自己的内存阈值**，
不是"查询/设置系统值"。
⚠️ 因此不要随意设置：实测时用过 1 字节，随后已恢复为 8 GiB（8589934592）。

### 14.5 仍未接的 33 个方法（清单由代码现状生成）

```
BackgroundChangeEvent           CreateVmApplicationForm      FocusStateChangeEvent
MountUSBToVm                    UnmountUSBFromVm
NotifyDataRecoveryProgress      NotifyHapExitToVm           NotifyRequireBigMemFinish
NotifyUpdateRgmConfigResult     NotifyVirtioMemoryChanged   NotifyVmDiskExported
NotifyVmDiskMigrationProgress   NotifyVmEvent               NotifyVmStatusChanged
RedirectGuestUserProfile        UpdateEulerOSImage
RegVmEventCallback              UnRegVmEventCallback        RegisterDeathRecipient
Register/UnregisterDataRecoveryCallback
Register/UnregisterRequireBigMemCallback
Register/UnregisterVirtioMemoryCallback
Register/UnregisterVmDiskExportedCallback
Register/UnregisterVmDiskMigrationCallback
Register/UnregisterVmStatusCallback
StartAutoPauseMonitor           StopAutoPauseMonitor
```

按组说明卡点：

| 组 | 卡点 |
|---|---|
| 事件回调（12 个 `Register*` / `RegVmEventCallback` / `RegisterDeathRecipient`） | 需要逆向 listener 接口对象的 vtable 并注册（同 `CfgInfo`/`MigrationOptions` 的工作方式） |
| 服务端通知实现（9 个 `Notify*`） | 是服务端 → 客户端的回调实现，配合上面那组一起做才完整 |
| USB 直通（`Mount/UnmountUSBToVm`） | `USBDevice` 布局已还原（大小 `0x80`、RefBase@+56、+24/+48/+72 三个 string、+96/+100/+104 三个 uint32、vtable 地址点 `0xB45F8+0x18`），但六个数值字段语义未定 |
| `UpdateEulerOSImage` | 需要实现 `IUpdateEulerOSImageCb` 回调接口 |
| `CreateVmApplicationForm` / `BackgroundChangeEvent` / `FocusStateChangeEvent` | 视图/表单相关，参数是私有类或视图枚举 |
| `Start/StopAutoPauseMonitor` | 与已接的 `SetAutoPauseTime` 配套 |
| `RedirectGuestUserProfile` | 参数语义未定 |

**已接但待实测**（因当前没有可用的运行中虚拟机，见 §14.2 的说明）：
`Get/SetPortForwardForNat`、`Get/SetLocalhostForwardFromVmToHost` —— 签名与传参已被服务端
接受（错误信息为 `not current running instance`），但条目解析与写入行为尚未端到端验证。

> `GetLinuxPathFromOhPath` 与 `VmUniSocPerfRequest(Ex)` 已接但**服务端按调用者身份拒绝**
> （见 §14.3），不属于"未实现"。
