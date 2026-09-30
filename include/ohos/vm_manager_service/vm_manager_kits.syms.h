// 本文件由 scripts/gen-wrapper-api.py 生成：mangled 名由**编译器**从
// vm_manager_kits.h 的声明产出，并与设备符号快照逐一核对通过。
// 请勿手工编辑。
#ifndef OHOS_VM_MANAGER_SERVICE_VM_MANAGER_KITS_SYMS_H
#define OHOS_VM_MANAGER_SERVICE_VM_MANAGER_KITS_SYMS_H

namespace OHOS {
namespace VmManagerService {
namespace abi {

struct SymEntry {
    const char *name;
    const char *mangled;
};

//: 由方法名查 mangled 名（dlopen + dlsym 用）
inline constexpr SymEntry kWrapSyms[] = {
    {"AddPasteboardSharedFolder", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper25AddPasteboardSharedFolderERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_"},
    {"AddSharedFolder", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper15AddSharedFolderERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_SA_"},
    {"BackgroundChangeEvent", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper21BackgroundChangeEventERKNS0_15BackgroundStateE"},
    {"CheckIsInstalling", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper17CheckIsInstallingEv"},
    {"CheckVmCapability", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper17CheckVmCapabilityERb"},
    {"CreateSnapshot", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper14CreateSnapshotERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_"},
    {"CreateVm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper8CreateVmERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_RKNS_4sptrINS0_7CfgInfoEEE"},
    {"CreateVmApplicationForm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper23CreateVmApplicationFormERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_SA_NS0_13FormDimensionE"},
    {"DeleteLinuxDataImage", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper20DeleteLinuxDataImageEv"},
    {"DeleteRgmImageFromVm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper20DeleteRgmImageFromVmERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE"},
    {"DestroySnapshot", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper15DestroySnapshotERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_"},
    {"DestroyVm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper9DestroyVmERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE"},
    {"DisplaysNumber", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper14DisplaysNumberERKNSt3__h6vectorImNS2_9allocatorImEEEE"},
    {"ExportVmDiskImage", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper17ExportVmDiskImageERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_SA_bRKNS_4sptrINS0_16MigrationOptionsEEE"},
    {"FocusStateChangeEvent", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper21FocusStateChangeEventENS0_10FocusStateE"},
    {"ForceStopVm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper11ForceStopVmERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE"},
    {"GetActiveVmName", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper15GetActiveVmNameERNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE"},
    {"GetActiveVmStatus", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper17GetActiveVmStatusERi"},
    {"GetActiveVmStatusForShutdown", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper28GetActiveVmStatusForShutdownERi"},
    {"GetAllSharedVolume", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper18GetAllSharedVolumeEv"},
    {"GetHashName", "_ZN4OHOS16VmManagerService14VmManagerProxy11GetHashNameEv"},
    {"GetHostSN", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper9GetHostSNERNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE"},
    {"GetInstance", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper11GetInstanceEv"},
    {"GetLinuxPathFromOhPath", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper22GetLinuxPathFromOhPathERKNSt3__h6vectorINS2_12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEENS7_IS9_EEEERSB_"},
    {"GetLocalhostForwardFromVmToHost", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper31GetLocalhostForwardFromVmToHostERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEERNS_4sptrINS0_12PortInfoListEEE"},
    {"GetOpenEulerVersion", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper19GetOpenEulerVersionERNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE"},
    {"GetPasteboardEnableState", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper24GetPasteboardEnableStateEv"},
    {"GetPasteboardUsableState", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper24GetPasteboardUsableStateEv"},
    {"GetPortForwardForNat", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper20GetPortForwardForNatERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEERNS_4sptrINS0_12PortInfoListEEE"},
    {"GetRgmImageStatusFromVm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper23GetRgmImageStatusFromVmERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE"},
    {"GetSharedFolder", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper15GetSharedFolderEv"},
    {"GetSharedFolderEnabled", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper22GetSharedFolderEnabledEv"},
    {"GetSnapshotList", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper15GetSnapshotListERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEERNS2_3mapIS8_S8_NS2_4lessIS8_EENS6_INS2_4pairIS9_S8_EEEEEE"},
    {"GetStratovirtMem", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper16GetStratovirtMemEv"},
    {"GetVmAvailableCpuNumRange", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper25GetVmAvailableCpuNumRangeERjS2_"},
    {"GetVmAvailableMemorySizeRange", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper29GetVmAvailableMemorySizeRangeERjS2_"},
    {"GetVmDiskCapacity", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper17GetVmDiskCapacityERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEERl"},
    {"GetVmDiskImageFileSize", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper22GetVmDiskImageFileSizeERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEERl"},
    {"GetVmDiskImagePath", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper18GetVmDiskImagePathERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEERS8_"},
    {"GetVmHostNetProxyStatus", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper23GetVmHostNetProxyStatusERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEERb"},
    {"GetVmInfo", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper9GetVmInfoERjS2_"},
    {"GetVmIpv4Address", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper16GetVmIpv4AddressERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEERS8_"},
    {"GetVmStatus", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper11GetVmStatusERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEERi"},
    {"HandleLxSnapshot", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper16HandleLxSnapshotERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_i"},
    {"ImportVmDiskImage", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper17ImportVmDiskImageERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_SA_RKNS_4sptrINS0_16MigrationOptionsEEE"},
    {"IsFeatureSupported", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper18IsFeatureSupportedEiRb"},
    {"IsProcessExist", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper14IsProcessExistERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE"},
    {"IsQuickStartScenario", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper20IsQuickStartScenarioEv"},
    {"LockGuest", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper9LockGuestEv"},
    {"LxOtaHandle", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper11LxOtaHandleEv"},
    {"ModifyResolution", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper16ModifyResolutionEjjb"},
    {"MountCDDriveToVm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper16MountCDDriveToVmERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_bRS8_"},
    {"MountUSBToVm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper12MountUSBToVmERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEERKNS_4sptrINS0_9USBDeviceEEE"},
    {"NotifyDataRecoveryProgress", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper26NotifyDataRecoveryProgressERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEElll"},
    {"NotifyHapExitToVm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper17NotifyHapExitToVmERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE"},
    {"NotifyRequireBigMemFinish", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper25NotifyRequireBigMemFinishEi"},
    {"NotifyUpdateRgmConfigResult", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper27NotifyUpdateRgmConfigResultEi"},
    {"NotifyVirtioMemoryChanged", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper25NotifyVirtioMemoryChangedERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEb"},
    {"NotifyVmDiskExported", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper20NotifyVmDiskExportedERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_"},
    {"NotifyVmDiskMigrationProgress", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper29NotifyVmDiskMigrationProgressERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEll"},
    {"NotifyVmEvent", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper13NotifyVmEventENS0_9EventTypeERKNSt3__h12basic_stringIcNS3_11char_traitsIcEENS3_9allocatorIcEEEE"},
    {"NotifyVmStatusChanged", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper21NotifyVmStatusChangedERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEiSA_"},
    {"PauseVm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper7PauseVmEv"},
    {"PauseVm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper7PauseVmERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE"},
    {"ProgressDiedStateToVm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper21ProgressDiedStateToVmERKNS0_12HapViewStateE"},
    {"RecoverUserData", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper15RecoverUserDataERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_"},
    {"RecvDataFromVm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper14RecvDataFromVmERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEERNS2_6vectorIhNS6_IhEEEEiRKNS0_11ChannelInfoE"},
    {"RedirectGuestUserProfile", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper24RedirectGuestUserProfileERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEi"},
    {"RegVmEventCallback", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper18RegVmEventCallbackENS0_9EventTypeENSt3__h10shared_ptrINS0_16IVmEventListenerEEE"},
    {"RegisterDataRecoveryCallback", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper28RegisterDataRecoveryCallbackENSt3__h10shared_ptrINS0_23IVmDataRecoveryListenerEEE"},
    {"RegisterDeathRecipient", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper22RegisterDeathRecipientENS_4sptrINS0_23VmManagerDeathRecipientEEE"},
    {"RegisterRequireBigMemCallback", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper29RegisterRequireBigMemCallbackENSt3__h10shared_ptrINS0_22IRequireBigMemListenerEEE"},
    {"RegisterVirtioMemoryCallback", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper28RegisterVirtioMemoryCallbackENS_4sptrINS0_21IVirtioMemoryListenerEEE"},
    {"RegisterVmDiskExportedCallback", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper30RegisterVmDiskExportedCallbackENSt3__h10shared_ptrINS0_23IVmDiskExportedListenerEEE"},
    {"RegisterVmDiskMigrationCallback", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper31RegisterVmDiskMigrationCallbackENSt3__h10shared_ptrINS0_24IVmDiskMigrationListenerEEE"},
    {"RegisterVmStatusCallback", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper24RegisterVmStatusCallbackENSt3__h10shared_ptrINS0_17IVmStatusListenerEEE"},
    {"RemovePasteboardSharedFolder", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper28RemovePasteboardSharedFolderERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE"},
    {"RemoveSharedFolder", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper18RemoveSharedFolderERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_"},
    {"RenameSnapshot", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper14RenameSnapshotERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_SA_"},
    {"RequireBigMem", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper13RequireBigMemEv"},
    {"RestoreSnapshot", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper15RestoreSnapshotERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_"},
    {"ResumeVm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper8ResumeVmERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE"},
    {"SendDataToVm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper12SendDataToVmERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEERKNS2_6vectorIhNS6_IhEEEEiRKNS0_11ChannelInfoE"},
    {"Set2DSwapSpace", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper14Set2DSwapSpaceEi"},
    {"SetAutoPauseTime", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper16SetAutoPauseTimeERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEERKNS0_13AutoPauseTimeE"},
    {"SetDnsAutoSyncEnabled", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper21SetDnsAutoSyncEnabledERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEb"},
    {"SetGuestDiskShared", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper18SetGuestDiskSharedERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_b"},
    {"SetHostGallerySharedEnabled", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper27SetHostGallerySharedEnabledERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEb"},
    {"SetLocalhostForwardFromVmToHost", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper31SetLocalhostForwardFromVmToHostERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEjRKNS_4sptrINS0_12PortInfoListEEE"},
    {"SetPasteboardEnableState", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper24SetPasteboardEnableStateEb"},
    {"SetPasteboardUsableState", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper24SetPasteboardUsableStateEb"},
    {"SetPortForwardForNat", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper20SetPortForwardForNatERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEjRKNS_4sptrINS0_12PortInfoListEEE"},
    {"SetProxyAutoSyncEnabled", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper23SetProxyAutoSyncEnabledERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEb"},
    {"SetSharedFolderEnabled", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper22SetSharedFolderEnabledEb"},
    {"SetUpSharedFolder", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper17SetUpSharedFolderERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE"},
    {"SetVmHostNetProxyStatus", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper23SetVmHostNetProxyStatusERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEb"},
    {"SetVmNetMode", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper12SetVmNetModeERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEERKNS0_7NetModeESA_"},
    {"StartAutoPauseMonitor", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper21StartAutoPauseMonitorERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE"},
    {"StartVm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper7StartVmERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEERKNS_4sptrINS0_7CfgInfoEEE"},
    {"StopAutoPauseMonitor", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper20StopAutoPauseMonitorERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE"},
    {"StopVm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper6StopVmERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEb"},
    {"SwitchVmNetworkShare", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper20SwitchVmNetworkShareERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEb"},
    {"SysAvailBufferLimit", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper19SysAvailBufferLimitEm"},
    {"SysLowBufferLimit", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper17SysLowBufferLimitEm"},
    {"TabletSwitchChanged", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper19TabletSwitchChangedEi"},
    {"ToggleScreenLockTask", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper20ToggleScreenLockTaskEb"},
    {"TouchVmMem", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper10TouchVmMemEj"},
    {"UnRegVmEventCallback", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper20UnRegVmEventCallbackENS0_9EventTypeENSt3__h10shared_ptrINS0_16IVmEventListenerEEE"},
    {"UnmountCDDriveFromVm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper20UnmountCDDriveFromVmERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_"},
    {"UnmountUSBFromVm", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper16UnmountUSBFromVmERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEERKNS_4sptrINS0_9USBDeviceEEE"},
    {"UnregisterDataRecoveryCallback", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper30UnregisterDataRecoveryCallbackENSt3__h10shared_ptrINS0_23IVmDataRecoveryListenerEEE"},
    {"UnregisterRequireBigMemCallback", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper31UnregisterRequireBigMemCallbackENSt3__h10shared_ptrINS0_22IRequireBigMemListenerEEE"},
    {"UnregisterVirtioMemoryCallback", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper30UnregisterVirtioMemoryCallbackENS_4sptrINS0_21IVirtioMemoryListenerEEE"},
    {"UnregisterVmDiskExportedCallback", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper32UnregisterVmDiskExportedCallbackENSt3__h10shared_ptrINS0_23IVmDiskExportedListenerEEE"},
    {"UnregisterVmDiskMigrationCallback", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper33UnregisterVmDiskMigrationCallbackENSt3__h10shared_ptrINS0_24IVmDiskMigrationListenerEEE"},
    {"UnregisterVmStatusCallback", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper26UnregisterVmStatusCallbackENSt3__h10shared_ptrINS0_17IVmStatusListenerEEE"},
    {"UpdateEulerOSImage", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper18UpdateEulerOSImageERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEiRKNS2_10shared_ptrINS0_21IUpdateEulerOSImageCbEEE"},
    {"VmExpandCapacity", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper16VmExpandCapacityERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEi"},
    {"VmQuitByRebootHost", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper18VmQuitByRebootHostEv"},
    {"VmUniSocPerfRequest", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper19VmUniSocPerfRequestERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_"},
    {"VmUniSocPerfRequestEx", "_ZN4OHOS16VmManagerService22VmManagerClientWrapper21VmUniSocPerfRequestExERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEbSA_"},
};
constexpr std::size_t kWrapSymCount = sizeof(kWrapSyms) / sizeof(kWrapSyms[0]);

inline const char *FindSym(const char *name) {
    for (std::size_t i = 0; i < kWrapSymCount; ++i) {
        const char *a = kWrapSyms[i].name;
        const char *b = name;
        while (*a != 0 && *a == *b) { ++a; ++b; }
        if (*a == 0 && *b == 0) return kWrapSyms[i].mangled;
    }
    return nullptr;
}

}  // namespace abi
}  // namespace VmManagerService
}  // namespace OHOS

#endif  // OHOS_VM_MANAGER_SERVICE_VM_MANAGER_KITS_SYMS_H
