// 本文件由 scripts/gen-wrapper-api.py 生成（符号来自设备快照 + 编译器核对）。
// 请勿手工编辑；要增删接口请改脚本里的数据表后重新生成。
//
// 用法：这是华为私有的客户端 SDK（系统内置、无公开头文件）。
//   * 设备侧构建：直接 #include 本头文件并用 VmManagerClientWrapper 的声明；
//   * dlopen 场景：用生成的符号表 abi::FindSym("方法名") 取 mangled 名再 dlsym。
//
// 返回类型：Itanium 改编不含返回类型。带出参的方法一律 ErrCode(int32_t)（实测）；
// 少数按值返回的见下方逐条注释。
#ifndef OHOS_VM_MANAGER_SERVICE_VM_MANAGER_KITS_H
#define OHOS_VM_MANAGER_SERVICE_VM_MANAGER_KITS_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ohos/vm_manager_service/vm_manager_errcode.h"

namespace OHOS {

//: 单指针智能指针（对象布局就是一个指针；本仓库按引用传递即可）
template <typename T> class sptr;

namespace VmManagerService {

// ---- 被引用类型的声明 ----
class AutoPauseTime;
enum BackgroundState : int;
class CfgInfo;
class ChannelInfo;
enum EventType : int;
enum FocusState : int;
enum FormDimension : int;
class HapViewState;
class IRequireBigMemListener;
class IUpdateEulerOSImageCb;
class IVirtioMemoryListener;
class IVmDataRecoveryListener;
class IVmDiskExportedListener;
class IVmDiskMigrationListener;
class IVmEventListener;
class IVmStatusListener;
class MigrationOptions;
class NetMode;
class PortInfoList;
class USBDevice;
class VmManagerDeathRecipient;

//: 系统能力 id 与接口描述符（IPC 用）
constexpr int kSystemAbilityId = 65621;
constexpr char kInterfaceDescriptor[] = "OHOS.VmManagerService.IVmManager";
constexpr char kCallbackDescriptor[] = "OHOS.VmManager.IVmManagerCallback";

class VmManagerClientWrapper {
  public:
    int32_t AddPasteboardSharedFolder(const std::string &, const std::string &);
    int32_t AddSharedFolder(const std::string &, const std::string &, const std::string &);
    int32_t BackgroundChangeEvent(const BackgroundState &);
    bool CheckIsInstalling();  // 推断：Check* 语义
    int32_t CheckVmCapability(bool &);
    int32_t CreateSnapshot(const std::string &, const std::string &);
    int32_t CreateVm(const std::string &, const std::string &, const sptr<CfgInfo> &);
    int32_t CreateVmApplicationForm(const std::string &, const std::string &, const std::string &, FormDimension);
    int32_t DeleteLinuxDataImage();
    int32_t DeleteRgmImageFromVm(const std::string &);
    int32_t DestroySnapshot(const std::string &, const std::string &);
    int32_t DestroyVm(const std::string &);
    int32_t DisplaysNumber(const std::vector<uint64_t> &);
    int32_t ExportVmDiskImage(const std::string &, const std::string &, const std::string &, bool, const sptr<MigrationOptions> &);
    int32_t FocusStateChangeEvent(FocusState);
    int32_t ForceStopVm(const std::string &);
    int32_t GetActiveVmName(std::string &);
    int32_t GetActiveVmStatus(int32_t &);
    int32_t GetActiveVmStatusForShutdown(int32_t &);
    std::vector<std::string> GetAllSharedVolume();  // 推断：与 GetSharedFolder 同类
    std::string GetHashName();  // 实测：sret 返回 std::string
    int32_t GetHostSN(std::string &);
    static sptr<VmManagerClientWrapper> GetInstance();  // 实测：sret 返回 sptr
    int32_t GetLinuxPathFromOhPath(const std::vector<std::string> &, std::vector<std::string> &);
    int32_t GetLocalhostForwardFromVmToHost(const std::string &, sptr<PortInfoList> &);
    int32_t GetOpenEulerVersion(std::string &);
    bool GetPasteboardEnableState();  // 推断：Get*State 语义
    bool GetPasteboardUsableState();  // 推断：Get*State 语义
    int32_t GetPortForwardForNat(const std::string &, sptr<PortInfoList> &);
    int32_t GetRgmImageStatusFromVm(const std::string &);
    std::string GetSharedFolder();  // 实测：sret 返回 std::string
    bool GetSharedFolderEnabled();  // 实测：返回 bool
    int32_t GetSnapshotList(const std::string &, std::map<std::string, std::string> &);
    int64_t GetStratovirtMem();  // 推断：内存用量（实测 CLI 打印过 MB 数值）
    int32_t GetVmAvailableCpuNumRange(uint32_t &, uint32_t &);
    int32_t GetVmAvailableMemorySizeRange(uint32_t &, uint32_t &);
    int32_t GetVmDiskCapacity(const std::string &, int64_t &);
    int32_t GetVmDiskImageFileSize(const std::string &, int64_t &);
    int32_t GetVmDiskImagePath(const std::string &, std::string &);
    int32_t GetVmHostNetProxyStatus(const std::string &, bool &);
    int32_t GetVmInfo(uint32_t &, uint32_t &);
    int32_t GetVmIpv4Address(const std::string &, std::string &);
    int32_t GetVmStatus(const std::string &, int32_t &);
    int32_t HandleLxSnapshot(const std::string &, const std::string &, int32_t);
    int32_t ImportVmDiskImage(const std::string &, const std::string &, const std::string &, const sptr<MigrationOptions> &);
    int32_t IsFeatureSupported(int32_t, bool &);
    int32_t IsProcessExist(const std::string &);
    bool IsQuickStartScenario();  // 推断：Is* 语义
    int32_t LockGuest();
    int32_t LxOtaHandle();
    int32_t ModifyResolution(uint32_t, uint32_t, bool);
    int32_t MountCDDriveToVm(const std::string &, const std::string &, bool, std::string &);
    int32_t MountUSBToVm(const std::string &, const sptr<USBDevice> &);
    int32_t NotifyDataRecoveryProgress(const std::string &, int64_t, int64_t, int64_t);
    int32_t NotifyHapExitToVm(const std::string &);
    int32_t NotifyRequireBigMemFinish(int32_t);
    int32_t NotifyUpdateRgmConfigResult(int32_t);
    int32_t NotifyVirtioMemoryChanged(const std::string &, bool);
    int32_t NotifyVmDiskExported(const std::string &, const std::string &);
    int32_t NotifyVmDiskMigrationProgress(const std::string &, int64_t, int64_t);
    int32_t NotifyVmEvent(EventType, const std::string &);
    int32_t NotifyVmStatusChanged(const std::string &, int32_t, const std::string &);
    int32_t PauseVm();
    int32_t PauseVm(const std::string &);
    int32_t ProgressDiedStateToVm(const HapViewState &);
    int32_t RecoverUserData(const std::string &, const std::string &);
    int32_t RecvDataFromVm(const std::string &, std::vector<unsigned char> &, int32_t, const ChannelInfo &);
    int32_t RedirectGuestUserProfile(const std::string &, int32_t);
    int32_t RegVmEventCallback(EventType, std::shared_ptr<IVmEventListener>);
    int32_t RegisterDataRecoveryCallback(std::shared_ptr<IVmDataRecoveryListener>);
    int32_t RegisterDeathRecipient(sptr<VmManagerDeathRecipient>);
    int32_t RegisterRequireBigMemCallback(std::shared_ptr<IRequireBigMemListener>);
    int32_t RegisterVirtioMemoryCallback(sptr<IVirtioMemoryListener>);
    int32_t RegisterVmDiskExportedCallback(std::shared_ptr<IVmDiskExportedListener>);
    int32_t RegisterVmDiskMigrationCallback(std::shared_ptr<IVmDiskMigrationListener>);
    int32_t RegisterVmStatusCallback(std::shared_ptr<IVmStatusListener>);
    int32_t RemovePasteboardSharedFolder(const std::string &);
    int32_t RemoveSharedFolder(const std::string &, const std::string &);
    int32_t RenameSnapshot(const std::string &, const std::string &, const std::string &);
    int32_t RequireBigMem();
    int32_t RestoreSnapshot(const std::string &, const std::string &);
    int32_t ResumeVm(const std::string &);
    int32_t SendDataToVm(const std::string &, const std::vector<unsigned char> &, int32_t, const ChannelInfo &);
    int32_t Set2DSwapSpace(int32_t);
    int32_t SetAutoPauseTime(const std::string &, const AutoPauseTime &);
    int32_t SetDnsAutoSyncEnabled(const std::string &, bool);
    int32_t SetGuestDiskShared(const std::string &, const std::string &, bool);
    int32_t SetHostGallerySharedEnabled(const std::string &, bool);
    int32_t SetLocalhostForwardFromVmToHost(const std::string &, uint32_t, const sptr<PortInfoList> &);
    int32_t SetPasteboardEnableState(bool);
    int32_t SetPasteboardUsableState(bool);
    int32_t SetPortForwardForNat(const std::string &, uint32_t, const sptr<PortInfoList> &);
    int32_t SetProxyAutoSyncEnabled(const std::string &, bool);
    int32_t SetSharedFolderEnabled(bool);
    int32_t SetUpSharedFolder(const std::string &);
    int32_t SetVmHostNetProxyStatus(const std::string &, bool);
    int32_t SetVmNetMode(const std::string &, const NetMode &, const std::string &);
    int32_t StartAutoPauseMonitor(const std::string &);
    int32_t StartVm(const std::string &, const sptr<CfgInfo> &);
    int32_t StopAutoPauseMonitor(const std::string &);
    int32_t StopVm(const std::string &, bool);
    int32_t SwitchVmNetworkShare(const std::string &, bool);
    int32_t SysAvailBufferLimit(uint64_t);
    int32_t SysLowBufferLimit(uint64_t);
    int32_t TabletSwitchChanged(int32_t);
    int32_t ToggleScreenLockTask(bool);
    int32_t TouchVmMem(uint32_t);
    int32_t UnRegVmEventCallback(EventType, std::shared_ptr<IVmEventListener>);
    int32_t UnmountCDDriveFromVm(const std::string &, const std::string &);
    int32_t UnmountUSBFromVm(const std::string &, const sptr<USBDevice> &);
    int32_t UnregisterDataRecoveryCallback(std::shared_ptr<IVmDataRecoveryListener>);
    int32_t UnregisterRequireBigMemCallback(std::shared_ptr<IRequireBigMemListener>);
    int32_t UnregisterVirtioMemoryCallback(sptr<IVirtioMemoryListener>);
    int32_t UnregisterVmDiskExportedCallback(std::shared_ptr<IVmDiskExportedListener>);
    int32_t UnregisterVmDiskMigrationCallback(std::shared_ptr<IVmDiskMigrationListener>);
    int32_t UnregisterVmStatusCallback(std::shared_ptr<IVmStatusListener>);
    int32_t UpdateEulerOSImage(const std::string &, int32_t, const std::shared_ptr<IUpdateEulerOSImageCb> &);
    int32_t VmExpandCapacity(const std::string &, int32_t);
    int32_t VmQuitByRebootHost();
    int32_t VmUniSocPerfRequest(const std::string &, const std::string &);
    int32_t VmUniSocPerfRequestEx(const std::string &, bool, const std::string &);
};

}  // namespace VmManagerService
}  // namespace OHOS

#include "ohos/vm_manager_service/vm_manager_kits.syms.h"

#endif  // OHOS_VM_MANAGER_SERVICE_VM_MANAGER_KITS_H
