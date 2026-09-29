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
 * CfgInfo 没有独立构造函数（被内联展开），因此需要按 [N] OnCreateVm 的原始序列手工构造：
 *
 *   p = operator new(0x300)            ; memset(p, 0, 0x300)
 *   RefBase::RefBase(p + 752)          ; 内嵌 RefBase
 *   Parcelable::Parcelable(p)          ; 基类
 *   *(int32*)(p + 80) = -1             ; startType 默认值
 *   *(void**)p        = *(void**)(baseN + napi::kVtableSlotCfgInfo)      ; CfgInfo vptr
 *   *(void**)(p + 752)= *(void**)(baseN + napi::kVtableSlotCfgRefBase)   ; RefBase vptr
 *   DeviceInfo::DeviceInfo(p + 88)     ; 内嵌 DeviceInfo
 *   Parcelable::Parcelable(p + 696)    ; 内嵌 BundleInfo 的 Parcelable 基类
 *   RefBase::RefBase(p + 736)          ; 内嵌 BundleInfo 的 RefBase
 *   *(void**)(p + 696)= *(void**)(baseN + napi::kVtableSlotBundleRefBase)
 *   *(void**)(p + 736)= &typeid(BundleInfo)      ; 见 napi::kTypeidBundleInfo
 *   RefBase::IncStrongRef(p + 752, &holder)      ; 交给 sptr 持有
 *
 * 其中 3 个 vptr 槽位是数据槽（.data.rel.ro 中的指针），
 * 必须从「已加载的库内存」里取值（磁盘上是 ANDROID_RELA 压缩重定位，静态读不到）。
 */
namespace napi {

/** 用于换算 [N] 运行时基址的锚点：OHOS::HmosWindowsFusion::WindowsClient::delegator_ */
constexpr std::uintptr_t kBaseAnchorDelegator = 0xBD020;

/* --- 数据槽（取其中的指针值） --- */
constexpr std::uintptr_t kVtableSlotCfgInfo = 0xB2F90;
constexpr std::uintptr_t kVtableSlotCfgRefBase = 0xB2FF0;
constexpr std::uintptr_t kVtableSlotBundleRefBase = 0xB44F0;
constexpr std::uintptr_t kTypeidDeviceInfo = 0xB3110;
constexpr std::uintptr_t kTypeidBundleInfo = 0xB4490;

/* --- 函数（可直接按地址调用，签名见下） --- */
constexpr std::uintptr_t kFnRefBaseCtor = 0xBDCB0;         // void RefBase(void* this)
constexpr std::uintptr_t kFnParcelableCtor = 0xBDCB8;      // void Parcelable(void* this)
constexpr std::uintptr_t kFnRefBaseIncStrong = 0xBDE68;    // void IncStrongRef(void* this, void** out)
constexpr std::uintptr_t kFnDeviceInfoCtor = 0x65C48;      // void DeviceInfo::DeviceInfo(void* this)
constexpr std::uintptr_t kFnDeviceInfoDtor = 0x66E7C;
constexpr std::uintptr_t kFnDeviceInfoAssign = 0x663B4;

}  // namespace napi

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
