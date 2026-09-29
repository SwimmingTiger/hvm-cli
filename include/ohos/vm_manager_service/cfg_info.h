/*
 * cfg_info.h —— OHOS::VmManagerService::CfgInfo 结构还原
 *
 * CfgInfo 是虚拟机创建/启动时传递的配置对象。它是华为私有类型：
 *   - 公开 SDK（command-line-tools）无任何相关头文件
 *   - OpenHarmony / GitCode 无源码
 *   - 库被 strip，CfgInfo 的方法没有导出符号
 *
 * 本文件的布局来自 idalib 反编译以下几处调用点：
 *   [N] /system/lib64/module/hms/virtservice/libvmmanager_napi.z.so
 *         UnwrapBaseCfgInfo @0x65898   —— 逐字段读取 JS 属性并写入 CfgInfo
 *         OnCreateVm        @0xA8144   —— 内联展开的 CfgInfo 构造序列（无独立构造函数）
 *         UnwrapCfgInfo     @0x65498   —— deviceInfo / bundleInfo 子对象
 *   [S] /system/lib64/libvm_manager.z.so
 *         CfgInfo::Marshalling   @0x15ABE4
 *         CfgInfo::Unmarshalling @0x155280
 *
 * 所有 "静态偏移" 均指所属 .so 的静态 vaddr；
 * 运行时地址 = dlopen 得到的加载基址 + 静态偏移。
 */
#ifndef OHOS_VM_MANAGER_SERVICE_CFG_INFO_H
#define OHOS_VM_MANAGER_SERVICE_CFG_INFO_H

#include <cstddef>
#include <cstdint>

namespace OHOS {
namespace VmManagerService {

/** 真实定义在华为侧，此处仅作不透明声明。 */
class CfgInfo;
class DeviceInfo;
class BundleInfo;

namespace cfg_info {

/* ------------------------------------------------------------ 对象尺寸 */

/** CfgInfo 分配尺寸：由 [N] OnCreateVm 的 `operator new(0x300, nothrow)` 得出。 */
constexpr std::size_t kSize = 0x300;                  // 768

/** 内嵌 DeviceInfo 值成员起点（[N] `DeviceInfo::DeviceInfo(v16 + 88)`）。 */
constexpr std::size_t kDeviceInfoOffset = 88;

/** DeviceInfo 尺寸（[N] UnwrapCfgInfo 在栈上 `memset(&s, 0, 0x260)`）。 */
constexpr std::size_t kDeviceInfoSize = 0x260;

/** 内嵌 BundleInfo 值成员起点（[N] `Parcelable::Parcelable(v16 + 696)`）。 */
constexpr std::size_t kBundleInfoOffset = 696;

/** BundleInfo 内嵌 RefBase 起点（[N] `RefBase::RefBase(v16 + 736)`）。 */
constexpr std::size_t kBundleRefBaseOffset = 736;

/** CfgInfo 自身内嵌 RefBase 起点（[N] `RefBase::RefBase(v16 + 752)`）。 */
constexpr std::size_t kRefBaseOffset = 752;

/* ------------------------------------------------------------ 字段表 */

/**
 * 基础字段。offset 为字节偏移，jsKey 是该字段在 ArkTS/HAP 侧的属性名
 * （由 [N] UnwrapBaseCfgInfo 调用 AppExecFwk::Unwrap*ByPropertyName 的实参得出）。
 */
struct FieldInfo {
    std::size_t offset;
    const char *jsKey;
    const char *type;
    const char *note;
};

inline constexpr FieldInfo kBaseFields[] = {
    {12,  "cpuNum",          "int32",  "校验 >= 0，非法时报 UnwrapBaseCfgInfo:441"},
    {16,  "memorySize",      "int32",  "单位 MB，校验 >= 0（:449）"},
    {20,  "diskSize",        "int32",  "校验 >= 0（:457）"},
    {24,  "diskPartition",   "bool",   "日志标签 [DISK_PARTITION]（:467）"},
    {25,  "dynamicMemory",   "bool",   ""},
    {32,  "biosPath",        "string", "std::string（24 字节 SSO）"},
    {56,  "enhanceFilePath", "string", "std::string"},
    {80,  "startType",       "int32",  "取值 0..3，默认 -1；非法时报 UnwrapStartType"},
};

/** 子对象（同样是 ArkTS 侧属性名）。 */
inline constexpr FieldInfo kObjectFields[] = {
    {88,  "deviceInfo", "DeviceInfo(值成员)", "见 device_info 相关 Unwrap* 函数"},
    {696, "bundleInfo", "BundleInfo(值成员)", "目前已知含字符串 bundleDeviceId"},
};

/* ------------------------------------------------------------ 构造配方 */

/**
 * CfgInfo 没有独立构造函数（被内联展开），需按下面的序列手工构造。
 * 该序列逐条取自 [N] 中内联构造现场的反汇编（0xA848C~0xA851C）：
 *
 *   p = operator new(0x300); memset(p, 0, 0x300)
 *   RefBase::RefBase(p + 752)                       ; 内嵌 RefBase（libutils 导入）
 *   Parcelable::Parcelable(p, napi::kVttCfgInfo)    ; 注意第 2 参数是 VTT！
 *   *(int64*)(p + 12) = 0
 *   *(void**)p = baseN + napi::kVtableCfgInfo       ; CfgInfo 主 vtable
 *   *(void**)(p + 752) = baseN + napi::kVtableCfgRefBase
 *   *(int64*)(p + 18) = 0 ; 清零 +32..+63 ; 清零 +64..+79
 *   *(int32*)(p + 80) = -1                          ; startType 默认值
 *   DeviceInfo::DeviceInfo(p + 88)                  ; napi 库本地函数，无 VTT 参数
 *   RefBase::RefBase(p + 736)                       ; 内嵌 BundleInfo 的 RefBase
 *   Parcelable::Parcelable(p + 696, napi::kVttBundleInfo)
 *   *(void**)(p + 696) = baseN + napi::kVtableBundleInfo
 *   *(void**)(p + 736) = baseN + napi::kVtableBundleRefBase
 *
 * 两个关键点：
 *  1) 写进对象首字的是 vtable 地址点本身（baseN + 偏移），不是该地址处的内容；
 *     vtable+96 即同一类的"虚基类 RefBase"次虚表（已核对：0xB2F90+96 = 0xB2FF0）。
 *  2) Parcelable 有虚基类，其构造函数签名是 (void* this, void* vtt)；
 *     只传 this 会因 x1 为垃圾值而崩溃 —— 这是本仓库实测踩过的坑。
 */
namespace napi {

/** 用于换算 [N] 运行时基址的锚点：OHOS::HmosWindowsFusion::WindowsClient::delegator_ */
constexpr std::uintptr_t kBaseAnchorDelegator = 0xBD020;

/* --- vtable 地址点（已核验为标准 Itanium 布局：D1/D0/Marshalling…） --- */
constexpr std::uintptr_t kVtableCfgInfo = 0xB2F90;         // CfgInfo 主 vtable
constexpr std::uintptr_t kVtableCfgRefBase = 0xB2FF0;      // = kVtableCfgInfo + 96
constexpr std::uintptr_t kVtableBundleInfo = 0xB4490;      // BundleInfo 主 vtable
constexpr std::uintptr_t kVtableBundleRefBase = 0xB44F0;   // = kVtableBundleInfo + 96
constexpr std::uintptr_t kVtableDeviceInfo = 0xB3110;      // DeviceInfo 主 vtable（供参考）

/* --- 虚基类 VTT：Parcelable::Parcelable(this, vtt) 的第 2 个参数 --- */
constexpr std::uintptr_t kVttCfgInfoParcelable = 0xB0498;
constexpr std::uintptr_t kVttBundleInfoParcelable = 0xB0658;

/* --- [N] 库内的本地函数（位于 .text，可直接按地址调用） --- */
constexpr std::uintptr_t kFnDeviceInfoCtor = 0x65C48;      // void DeviceInfo::DeviceInfo(void*)
constexpr std::uintptr_t kFnDeviceInfoDtor = 0x66E7C;
constexpr std::uintptr_t kFnDeviceInfoAssign = 0x663B4;

/*
 * RefBase / Parcelable 的构造函数不是本库定义的，而是 [N] 的导入符号
 * （由 libutils.z.so 提供），因此用 dlsym 取地址：
 *   _ZN4OHOS7RefBaseC2Ev       void RefBase::RefBase(void* this)
 *   _ZN4OHOS10ParcelableC2Ev   void Parcelable::Parcelable(void* this, void* vtt)
 * 提示：[N] 的 .gnu_debugdata 里也能查到 "OHOS::RefBase::RefBase(void)" 之类名字，
 *       但它们指向 .bss 中的槽位（不是代码），照地址调用会段错误。
 */

/* ======================================================================
 * MigrationOptions —— 同一个 napi 模块里的另一个参数对象
 *
 * 用途：ImportVmDiskImage / ExportVmDiskImage 的第 4 个参数
 *       （const sptr<MigrationOptions> &）。服务端要求它非空
 *       （HandleImportVmDiskImage:1086 "MigrationOptions is nullptr."）。
 *
 * 构造序列取自 [N] 中 WindowsFusionNapi::OnImportVmDiskImage 的内联现场
 * （0x989E4~0x98A3C 的反汇编）：
 *
 *   p = operator new(0x40);  memset(p, 0, 0x40)
 *   RefBase::RefBase(p + 48)                       ; 内嵌 RefBase
 *   Parcelable::Parcelable(p, baseN + 0xB0698)     ; 注意第 2 参数是 VTT
 *   *(void**)(p + 0)  = baseN + 0xB4790            ; 主 vtable 地址点
 *   *(void**)(p + 48) = baseN + 0xB47F0            ; = 主 vtable + 96
 *   RefBase::IncStrongRef(p + 48, &holder)
 *
 * 字段（取自 [N] 的 UnwrapMigrationOptions）：
 *   +10  bool         isKeepSnapshots
 *   +11  bool         hasCallback
 *   +16  std::string  password（24 字节，+16..+39）
 *   +40  bool         isForceImport
 * ====================================================================== */

}  // namespace napi

namespace migration {

constexpr std::size_t kSize = 0x40;
constexpr std::uintptr_t kVtable = 0xB4790;          // 主 vtable 地址点
constexpr std::uintptr_t kVtableRefBase = 0xB47F0;   // = kVtable + 96
constexpr std::uintptr_t kVttParcelable = 0xB0698;   // Parcelable(this, vtt) 的 vtt
constexpr std::size_t kRefBaseOffset = 48;

constexpr std::size_t kIsKeepSnapshots = 10;  // bool
constexpr std::size_t kHasCallback = 11;      // bool
constexpr std::size_t kPassword = 16;         // std::string
constexpr std::size_t kIsForceImport = 40;    // bool

}  // namespace migration

/* ======================================================================
 * ChannelInfo —— 主机与客户机之间的通道描述（SendDataToVm / RecvDataFromVm 用）
 *
 * 布局取自 [S] 的 ChannelInfo::Unmarshalling / Marshalling：
 *   p = operator new(0x38); memset(p, 0, 0x38)
 *   RefBase::RefBase(p + 40)                     ; 内嵌 RefBase（注意在 +40，不是 +48）
 *   *(void**)p = baseN + 0xB4910                 ; 主 vtable 地址点（不需要 VTT）
 *   *(uint32*)(p + 12) = 通道类型
 *   new (p + 16) std::string(通道名)
 *
 * 通道类型取自 [N] 的 VmManagerChannelTypeInit（JS 枚举 ChannelType）：
 *   SERIAL = 0（脚本里只见到 SERIAL 的赋值；SERIAL1 推测为 1）
 *
 * 实测（本机虚拟机命令行）：客户机侧的 virtio-serial 端口是
 *   nr=1 id=winbox_serial0 → chardev socket .../uds/serial0.sock
 *   nr=2 id=winbox_serial1 → chardev socket .../uds/serial1.sock
 * ====================================================================== */
namespace channel {

constexpr std::size_t kSize = 0x38;
constexpr std::uintptr_t kVtable = 0xB4910;   // ChannelInfo 主 vtable 地址点（[N] 库内）
constexpr std::size_t kRefBaseOffset = 40;

constexpr std::size_t kType = 12;  // uint32：通道类型（见下）
constexpr std::size_t kName = 16;  // std::string：通道名

constexpr std::uint32_t kTypeSerial = 0;   // ChannelType.SERIAL
constexpr std::uint32_t kTypeSerial1 = 1;  // ChannelType.SERIAL1（推测）

}  // namespace channel

/* ======================================================================
 * PortInfoList —— NAT / 本机端口转发的条目表
 *
 * 布局取自 [S] PortInfoList::Marshalling / Unmarshalling：
 *   p = operator new(0x38); memset
 *   RefBase::RefBase(p + 40); *(void**)(p + 48) = 0
 *   std::vector<PortInfo> 位于 +16（begin）/ +24（end）/ +32（cap）
 *   *(void**)p = baseN + 0xB4A90        ; 主 vtable 地址点
 *   Marshalling 写：uint32 条目数，然后每条 3 个 uint32
 *   （Unmarshalling 侧限制条目数 <= 0x1E = 30）
 * ====================================================================== */
namespace portinfo {

constexpr std::size_t kSize = 0x38;
constexpr std::uintptr_t kVtable = 0xB4A90;
constexpr std::size_t kRefBaseOffset = 40;
constexpr std::size_t kVectorBegin = 16;   // std::vector<PortInfo>
constexpr std::size_t kPortInfoSize = 12;  // 3 × uint32
constexpr std::uint32_t kMaxEntries = 0x1E;

}  // namespace portinfo

/* ------------------------------------------------------------ 序列化 */

/**
 * 服务端 libvm_manager.z.so 中的序列化入口（供需要自己拼 Parcel 的场景使用）：
 *   CfgInfo::Marshalling(Parcel&) const    @0x15ABE4
 *   CfgInfo::Unmarshalling(Parcel&)        @0x155280   （静态工厂，内部 new CfgInfo）
 * 注意：服务端的 HandleStartVm 先读一个 Int32 标志，为真才读 CfgInfo；
 *       为假或反序列化失败会回 404。名字长度与 ".." 会被校验，非法回 401。
 */

}  // namespace cfg_info
}  // namespace VmManagerService
}  // namespace OHOS

#endif /* OHOS_VM_MANAGER_SERVICE_CFG_INFO_H */
