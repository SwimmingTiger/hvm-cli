# include/ —— 逆向还原的公共头文件

这里放的是**被逆向目标库自身的 ABI**（结构布局、函数原型、符号名），
不是本仓库自己的封装 —— 目的是让其他开发者可以直接对着头文件调用这些私有库。

| 头文件 | 目标库 | 内容 |
|---|---|---|
| `ohos/linux_fusion/fusion_pty_ndk.h` | `/system/lib64/ndk/libfusion_pty_ndk.so` | 融合开发引擎 openEuler 终端通道的完整 C API（结构 + 函数原型，可直接 `#include` 使用） |
| `ohos/vm_manager_service/cfg_info.h` | `libvmmanager_napi.z.so` / `libvm_manager.z.so` | 手工构造这些私有类所需的全部偏移与配方：`CfgInfo`（`cfg_info`）、`DeviceInfo`/`BundleInfo` 的 napi 侧偏移（`napi`）、`MigrationOptions`（`migration`）、`ChannelInfo`（`channel`）、`PortInfoList`（`portinfo`）。每个命名空间都注明配方来自哪个库的哪个函数 |
| `ohos/vm_manager_service/vm_manager_kits.h` | `/system/lib64/libvm_manager_kits.z.so` | **`VmManagerClientWrapper` 的完整 C++ 声明（121 个方法）+ SA/接口描述符**；`#include` 即可直接写代码。由 `scripts/gen-wrapper-api.py` 生成 |
| `ohos/vm_manager_service/vm_manager_kits.syms.h` | 同上 | 生成物：方法名 → mangled 名 的名字表 + `abi::FindSym()`，供 dlopen + dlsym 使用。mangled 名由**编译器**从上层声明产出并与设备符号逐一核对 |
| `ohos/vm_manager_service/vm_manager_errcode.h` | 服务端返回码 | 实测返回码的命名常量与 `ErrorName()`：`OHOS_VM_OK` / `OHOS_VM_ERR_NO_VIRTUAL_MACHINE` / `OHOS_VM_ERR_INVALID_VM_NAME` / `OHOS_VM_ERR_CFG_INFO_UNAVAILABLE` / `OHOS_VM_ERR_VM_IP_UNAVAILABLE` / `OHOS_VM_ERR_SNAPSHOT_UNAVAILABLE` 等 |

## 来源与可信度

全部由 [idalib](https://hex-rays.com/ida-pro) 反编译 + 设备实测得出；头文件里对每条
重要结论都标了出处（IDA 地址或实测行为）。三个符号来源库：

| 记号 | 路径 |
|---|---|
| `[K]` | `/system/lib64/libvm_manager_kits.z.so`（客户端 kit，本仓库直接调用） |
| `[S]` | `/system/lib64/libvm_manager.z.so`（服务端，SA 65621） |
| `[N]` | `/system/lib64/module/hms/virtservice/libvmmanager_napi.z.so`（HAP 的 napi 层，含 CfgInfo 构造序列） |
| `[P]` | `/system/lib64/ndk/libfusion_pty_ndk.so`（融合开发引擎 PTY） |

## 为什么需要这些头文件

这套 API 是**华为私有**的：

- 公开 SDK（HarmonyOS command-line-tools）里 `VmManager` / `CfgInfo` / `vm_manager`
  零命中，连 `system_ability_definition.h` 都没有；
- OpenHarmony 已迁至 GitCode，但在 GitCode / Gitee 上搜 `IVmManager`、`VmManagerService`
  均为 0 条；
- 相关 `.so` 被 strip，只有 `.dynsym`，`CfgInfo` 等方法没有导出符号。

只有 VMM 引擎层（openEuler 的 StratoVirt）是开源的，管理面完全是闭源实现，
所以这里把逆向结果沉淀下来。

## 用法

```cpp
#include "ohos/vm_manager_service/vm_manager_kits.h"        // 121 个方法的真实 C++ 声明
#include "ohos/vm_manager_service/vm_manager_kits.syms.h"   // 名字表 + abi::FindSym()

using GetInstance = sptr<VmManagerClientWrapper> (*)();
auto* kit = dlopen("libvm_manager_kits.z.so", RTLD_NOW | RTLD_GLOBAL);
auto getInstance = (GetInstance)dlsym(kit, abi::FindSym("GetInstance"));
sptr<VmManagerClientWrapper> wrapper = getInstance();

// 其余方法按声明取出函数指针即可，例如：
using CreateVm = int32_t (*)(VmManagerClientWrapper*, const std::string&,
                             const std::string&, const sptr<CfgInfo>&);
auto createVm = (CreateVm)dlsym(kit, abi::FindSym("CreateVm"));
```

```cpp
#include "ohos/linux_fusion/fusion_pty_ndk.h"

OhPtyManager* mgr = nullptr;
OhGetPtyManager(&mgr);              // 注意是出参形式
```

## 编译校验

```bash
make check-headers      # 单独语法检查这些头文件
```

## 注意

- 所有“静态偏移”都是所属 `.so` 的静态 vaddr，运行时地址 = `dlopen` 基址 + 偏移；
  版本升级后偏移可能变化，使用前建议校验库的 md5。
- `libvm_manager_kits.z.so` / `libfusion_pty_ndk.so` 内含单例与静态对象，
  **不要 `dlclose`**，否则进程退出阶段可能段错误。
