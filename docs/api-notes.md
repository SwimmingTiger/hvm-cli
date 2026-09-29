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

**结论：本仓库的两个命令都只能在系统自带的 HiShell 终端里运行。**
（`openeuler` 走的融合开发引擎通道同样受身份限制，其 `libfusion_pty_common.z.so`
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

| 场景 | 返回码 | 说明 |
|---|---|---|
| 成功 | `0` | |
| 无虚拟机时的快照查询 | `0xF8FF000C`（signed −117506036） | OHOS 统一错误码风格 |
| 无虚拟机时的 `GetOpenEulerVersion` | `201` | |
| 本工具本地错误 | `-1001` kit 未加载 / `-1002` 符号缺失 | 与系统返回码区分 |

状态码（`GetVmStatus`）服务端**没有把枚举名编进二进制**，目前只实测到
`0`（无虚拟机运行）。其余取值需在有虚拟机运行时逐个观测补全。

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
# SA 注册信息
cat /system/profile/vm_manager.json
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

**可行方案**：华为随开发者工具提供的
`~/.local/bin/huawei-debug-lldb-server`（只依赖 musl libc 的自包含版本），
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

### 仍未完成

- 提供真实 ISO/qcow2 镜像后即可完成一次完整创建；
- `DeviceInfo`（0x260）内部字段尚未逐个还原（各 `Unwrap*Device` 函数在 [N] 中）；
- 事件回调（`RegisterVmStatusCallback` 等）。
