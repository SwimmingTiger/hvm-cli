/*
 * vm_manager_client_wrapper.h —— 虚拟机管理客户端库 ABI 还原
 *
 * 对应库：/system/lib64/libvm_manager_kits.z.so
 *           OHOS::VmManagerService::VmManagerClientWrapper   （本文件主角，可直接调用）
 *           OHOS::VmManagerService::VmManagerClient
 *           OHOS::VmManagerService::VmManagerProxy           （Binder 客户端）
 *         服务端：SA 65621 vm_manager（/system/lib64/libvm_manager.z.so）
 *
 * 该 API 为华为私有：公开 SDK 无头文件，OpenHarmony/GitCode 无源码，
 * 库被 strip（仅 .dynsym）。本文件由 idalib 反编译 + 设备实测还原。
 *
 * ── ABI 注意 ──────────────────────────────────────────────────────────
 *  1. 该库的 libc++ inline namespace 是 std::__h（不是 __1），
 *     符号形如 ...RKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE。
 *     用 std::string 直接传入即可，布局与标准 libc++ 一致。
 *  2. OHOS::sptr<T> 是单指针智能指针；GetInstance() 的返回值可直接当 this 使用。
 *  3. 库内含单例与静态对象：dlclose() 会导致进程退出阶段段错误，请勿卸载。
 *
 * ── 调用者校验（服务端） ───────────────────────────────────────────────
 *  VmmCommonUtils::CheckCallerIdentity → IsLegalCalling → CheckCallingProcName
 *    → AccessTokenKit::GetNativeTokenInfo → IsleaglSystemAbility(uid, 进程名)
 *      放行 uid：5005(LinuxFusionService) / 7005 / 7700(hwf_service，需匹配进程名)
 *    → 否则回退到 HAP 身份校验：GetCallerBundleName + GetAppIdentifier
 *      比对 openEuler HAP / 2B 应用标识
 *  CheckCallingOpenEulerHap   比对 appId 与 appIdentifier 常量
 *  CheckCallingProcNameFromLinuxFusionService  要求 uid == 5005 且进程名匹配
 *  服务端日志中会出现：isLinuxFusionService:%d, isHiShellHap:%d, isOpenEulerHap:%d
 *  实测：系统自带终端 HiShell（uid 20020085）启动的进程**在白名单内**，
 *        所有只读接口返回 rc=0；第三方应用内置终端会被拒绝。
 */
#ifndef OHOS_VM_MANAGER_SERVICE_VM_MANAGER_CLIENT_WRAPPER_H
#define OHOS_VM_MANAGER_SERVICE_VM_MANAGER_CLIENT_WRAPPER_H

#include <cstdint>

namespace OHOS {
namespace VmManagerService {

/** 该库中 std::string 相关符号的后缀（OHOS libc++ 的 __h 命名空间）。 */
#define OHOS_VM_STRING_CONST \
    "ERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE"
#define OHOS_VM_STRING_REF \
    "ERNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE"
/** 客户端类的符号前缀。 */
#define OHOS_VM_WRAPPER_SYM_PREFIX \
    "_ZN4OHOS16VmManagerService22VmManagerClientWrapper"

/* ------------------------------------------------------------------ 服务 */
/** System Ability ID（/system/profile/vm_manager.json）。 */
constexpr std::int32_t kSystemAbilityIdVmManager = 65621;
/** ZIDL 接口描述符（UTF-16，服务端会用它校验 parcel 首段）。 */
constexpr char16_t kInterfaceDescriptorVmManager[] = u"OHOS.VmManagerService.IVmManager";
/** 回调接口描述符。 */
constexpr char16_t kInterfaceDescriptorVmCallback[] = u"OHOS.VmManager.IVmManagerCallback";

/* ------------------------------------------------------------ 符号名表 */
/**
 * 可直接 dlsym() 的符号名（已用到的部分）。
 * 用法示例：
 *   using GetInstance = void* (*)();
 *   auto getInstance = (GetInstance)dlsym(kit, OHOS_VM_WRAPPER_SYM_PREFIX "11GetInstanceEv");
 *   void* w = getInstance();
 */
#define OHOS_VM_SYM_GET_INSTANCE      OHOS_VM_WRAPPER_SYM_PREFIX "11GetInstanceEv"
#define OHOS_VM_SYM_ACTIVE_NAME       OHOS_VM_WRAPPER_SYM_PREFIX "15GetActiveVmName" OHOS_VM_STRING_REF
#define OHOS_VM_SYM_ACTIVE_STATUS     OHOS_VM_WRAPPER_SYM_PREFIX "17GetActiveVmStatusERi"
#define OHOS_VM_SYM_ACTIVE_STATUS_SD  OHOS_VM_WRAPPER_SYM_PREFIX "28GetActiveVmStatusForShutdownERi"
#define OHOS_VM_SYM_VM_STATUS         OHOS_VM_WRAPPER_SYM_PREFIX "11GetVmStatus" OHOS_VM_STRING_CONST "Ri"
#define OHOS_VM_SYM_IS_PROCESS_EXIST  OHOS_VM_WRAPPER_SYM_PREFIX "14IsProcessExist" OHOS_VM_STRING_CONST
#define OHOS_VM_SYM_CHECK_CAPABILITY  OHOS_VM_WRAPPER_SYM_PREFIX "17CheckVmCapabilityERb"
#define OHOS_VM_SYM_IS_FEATURE        OHOS_VM_WRAPPER_SYM_PREFIX "18IsFeatureSupportedEiRb"
#define OHOS_VM_SYM_GET_HASH_NAME     OHOS_VM_WRAPPER_SYM_PREFIX "11GetHashNameEv"
#define OHOS_VM_SYM_GET_VM_INFO       OHOS_VM_WRAPPER_SYM_PREFIX "9GetVmInfoERjS2_"
#define OHOS_VM_SYM_STRATOVIRT_MEM    OHOS_VM_WRAPPER_SYM_PREFIX "16GetStratovirtMemEv"
#define OHOS_VM_SYM_OPEN_EULER_VER    OHOS_VM_WRAPPER_SYM_PREFIX "19GetOpenEulerVersion" OHOS_VM_STRING_REF
#define OHOS_VM_SYM_HOST_SN           OHOS_VM_WRAPPER_SYM_PREFIX "9GetHostSN" OHOS_VM_STRING_REF
#define OHOS_VM_SYM_QUICK_START       OHOS_VM_WRAPPER_SYM_PREFIX "20IsQuickStartScenarioEv"
#define OHOS_VM_SYM_CHECK_INSTALLING  OHOS_VM_WRAPPER_SYM_PREFIX "17CheckIsInstallingEv"

/** 虚拟机生命周期（需要 CfgInfo，见 cfg_info.h）。 */
#define OHOS_VM_SYM_CREATE_VM         OHOS_VM_WRAPPER_SYM_PREFIX "8CreateVm" OHOS_VM_STRING_CONST "SA_RKNS_4sptrINS0_7CfgInfoEEE"
#define OHOS_VM_SYM_START_VM          OHOS_VM_WRAPPER_SYM_PREFIX "7StartVm" OHOS_VM_STRING_CONST "RKNS_4sptrINS0_7CfgInfoEEE"
#define OHOS_VM_SYM_STOP_VM           OHOS_VM_WRAPPER_SYM_PREFIX "6StopVm" OHOS_VM_STRING_CONST "b"
#define OHOS_VM_SYM_FORCE_STOP_VM     OHOS_VM_WRAPPER_SYM_PREFIX "11ForceStopVm" OHOS_VM_STRING_CONST
#define OHOS_VM_SYM_DESTROY_VM        OHOS_VM_WRAPPER_SYM_PREFIX "9DestroyVm" OHOS_VM_STRING_CONST
#define OHOS_VM_SYM_PAUSE_VM          OHOS_VM_WRAPPER_SYM_PREFIX "7PauseVm" OHOS_VM_STRING_CONST
#define OHOS_VM_SYM_RESUME_VM         OHOS_VM_WRAPPER_SYM_PREFIX "8ResumeVm" OHOS_VM_STRING_CONST

/** 快照。 */
#define OHOS_VM_SYM_SNAPSHOT_LIST     OHOS_VM_WRAPPER_SYM_PREFIX "15GetSnapshotList" OHOS_VM_STRING_CONST "RNS2_3mapIS8_S8_NS2_4lessIS8_EENS6_INS2_4pairIS9_S8_EEEEEE"
#define OHOS_VM_SYM_SNAPSHOT_CREATE   OHOS_VM_WRAPPER_SYM_PREFIX "14CreateSnapshot" OHOS_VM_STRING_CONST "SA_"
#define OHOS_VM_SYM_SNAPSHOT_RESTORE  OHOS_VM_WRAPPER_SYM_PREFIX "15RestoreSnapshot" OHOS_VM_STRING_CONST "SA_"
#define OHOS_VM_SYM_SNAPSHOT_DESTROY  OHOS_VM_WRAPPER_SYM_PREFIX "15DestroySnapshot" OHOS_VM_STRING_CONST "SA_"
#define OHOS_VM_SYM_SNAPSHOT_RENAME   OHOS_VM_WRAPPER_SYM_PREFIX "14RenameSnapshot" OHOS_VM_STRING_CONST "SA_SA_"

/** 共享目录 / 网络 / 磁盘 / 显示。 */
#define OHOS_VM_SYM_SHARED_FOLDER     OHOS_VM_WRAPPER_SYM_PREFIX "15GetSharedFolderEv"
#define OHOS_VM_SYM_SHARED_ENABLED    OHOS_VM_WRAPPER_SYM_PREFIX "22GetSharedFolderEnabledEv"
#define OHOS_VM_SYM_SET_SHARED_EN     OHOS_VM_WRAPPER_SYM_PREFIX "22SetSharedFolderEnabledEb"
#define OHOS_VM_SYM_ADD_SHARED        OHOS_VM_WRAPPER_SYM_PREFIX "15AddSharedFolder" OHOS_VM_STRING_CONST "SA_SA_"
#define OHOS_VM_SYM_RM_SHARED         OHOS_VM_WRAPPER_SYM_PREFIX "18RemoveSharedFolder" OHOS_VM_STRING_CONST "SA_"
#define OHOS_VM_SYM_SETUP_SHARED      OHOS_VM_WRAPPER_SYM_PREFIX "17SetUpSharedFolder" OHOS_VM_STRING_CONST
#define OHOS_VM_SYM_VM_IPV4           OHOS_VM_WRAPPER_SYM_PREFIX "16GetVmIpv4Address" OHOS_VM_STRING_CONST "RS8_"
#define OHOS_VM_SYM_NET_PROXY_STATUS  OHOS_VM_WRAPPER_SYM_PREFIX "23GetVmHostNetProxyStatus" OHOS_VM_STRING_CONST "Rb"
#define OHOS_VM_SYM_NET_SHARE_SWITCH  OHOS_VM_WRAPPER_SYM_PREFIX "20SwitchVmNetworkShare" OHOS_VM_STRING_CONST "b"
#define OHOS_VM_SYM_DNS_AUTO_SYNC     OHOS_VM_WRAPPER_SYM_PREFIX "21SetDnsAutoSyncEnabled" OHOS_VM_STRING_CONST "b"
#define OHOS_VM_SYM_DISK_CAPACITY     OHOS_VM_WRAPPER_SYM_PREFIX "17GetVmDiskCapacity" OHOS_VM_STRING_CONST "Rl"
#define OHOS_VM_SYM_DISK_IMAGE_PATH   OHOS_VM_WRAPPER_SYM_PREFIX "18GetVmDiskImagePath" OHOS_VM_STRING_CONST "RS8_"
#define OHOS_VM_SYM_DISK_IMAGE_SIZE   OHOS_VM_WRAPPER_SYM_PREFIX "22GetVmDiskImageFileSize" OHOS_VM_STRING_CONST "Rl"
#define OHOS_VM_SYM_DISK_EXPAND       OHOS_VM_WRAPPER_SYM_PREFIX "16VmExpandCapacity" OHOS_VM_STRING_CONST "i"
#define OHOS_VM_SYM_DEL_LINUX_DATA    OHOS_VM_WRAPPER_SYM_PREFIX "20DeleteLinuxDataImageEv"
#define OHOS_VM_SYM_MODIFY_RESOLUTION OHOS_VM_WRAPPER_SYM_PREFIX "16ModifyResolutionEjjb"
#define OHOS_VM_SYM_TOUCH_VM_MEM      OHOS_VM_WRAPPER_SYM_PREFIX "10TouchVmMemEj"
#define OHOS_VM_SYM_SET_2D_SWAP       OHOS_VM_WRAPPER_SYM_PREFIX "14Set2DSwapSpaceEi"

/* ------------------------------------------------------------- 返回值 */
/**
 * 部分实测返回码：
 *   0            成功
 *   201          无虚拟机时 GetOpenEulerVersion
 *   401          服务端：虚拟机名非法（长度或含 ".."）
 *   404          服务端：CfgInfo 为空或反序列化失败
 *   405          无虚拟机时 GetVmIpv4Address
 *   0xF8FF000C   无虚拟机时 GetSnapshotList（OHOS 统一错误码风格）
 * 本仓库本地错误码（见 src/hvm_client.cpp）：-1001 kit 未加载 / -1002 符号缺失
 */

}  // namespace VmManagerService
}  // namespace OHOS

#endif /* OHOS_VM_MANAGER_SERVICE_VM_MANAGER_CLIENT_WRAPPER_H */
