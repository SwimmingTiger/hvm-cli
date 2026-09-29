// cfginfo.cpp —— 按逆向配方构造 CfgInfo
#include "cfginfo.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <new>

#include "ohos/vm_manager_service/cfg_info.h"

namespace hvm {
namespace {

namespace abi = OHOS::VmManagerService::cfg_info;

// napi 库：设备上在 module/hms/virtservice 下，也接受按名查找
constexpr const char *kNapiLibName = "libvmmanager_napi.z.so";
constexpr const char *kNapiLibPath =
    "/system/lib64/module/hms/virtservice/libvmmanager_napi.z.so";

// 用于换算加载基址的导出数据符号（WindowsClient::delegator_）
constexpr const char *kAnchorSymbol =
    "_ZN4OHOS17HmosWindowsFusion13WindowsClient10delegator_E";

using CtorFn = void (*)(void *);
//: Parcelable 有虚基类，构造函数带 VTT 参数：Parcelable(this, vtt)
using ParcelableCtorFn = void (*)(void *, void *);

struct NapiLib {
    void *handle = nullptr;
    char *base = nullptr;

    void *at(std::uintptr_t off) const { return base + off; }
    bool ok() const { return handle != nullptr && base != nullptr; }
};

NapiLib &napiLib() {
    static NapiLib lib = [] {
        NapiLib l;
        l.handle = dlopen(kNapiLibName, RTLD_NOW | RTLD_GLOBAL);
        if (l.handle == nullptr) {
            l.handle = dlopen(kNapiLibPath, RTLD_NOW | RTLD_GLOBAL);
        }
        if (l.handle == nullptr) return l;
        void *anchor = dlsym(l.handle, kAnchorSymbol);
        if (anchor == nullptr) return l;
        l.base = static_cast<char *>(anchor) - abi::napi::kBaseAnchorDelegator;
        return l;
    }();
    return lib;
}

}  // namespace

CfgInfoBuilder::CfgInfoBuilder() {
    NapiLib &lib = napiLib();
    if (!lib.ok()) {
        const char *e = dlerror();
        error_ = std::string("加载 ") + kNapiLibName + " 失败: " + (e ? e : "未知原因");
        return;
    }
    lib_ = lib.handle;
    base_ = lib.base;

    obj_ = ::operator new(abi::kSize, std::nothrow);
    if (obj_ == nullptr) {
        error_ = "分配 CfgInfo(0x300) 失败";
        return;
    }
    std::memset(obj_, 0, abi::kSize);

    // RefBase / Parcelable 的构造由 libutils 提供（napi 库的导入符号）
    auto refBaseCtor = reinterpret_cast<CtorFn>(dlsym(RTLD_DEFAULT, "_ZN4OHOS7RefBaseC2Ev"));
    auto parcelableCtor =
        reinterpret_cast<ParcelableCtorFn>(dlsym(RTLD_DEFAULT, "_ZN4OHOS10ParcelableC2Ev"));
    auto deviceInfoCtor = reinterpret_cast<CtorFn>(lib.at(abi::napi::kFnDeviceInfoCtor));
    if (refBaseCtor == nullptr || parcelableCtor == nullptr) {
        error_ = "找不到 RefBase/Parcelable 构造函数（libutils 未加载？）";
        return;
    }

    char *p = static_cast<char *>(obj_);
    // 逐条对应 [N] 中内联构造现场的反汇编（0xA848C~0xA851C）
    refBaseCtor(p + abi::kRefBaseOffset);
    parcelableCtor(p, lib.at(abi::napi::kVttCfgInfoParcelable));
    *reinterpret_cast<std::int32_t *>(p + 80) = -1;  // startType 默认值
    *reinterpret_cast<void **>(p) = lib.at(abi::napi::kVtableCfgInfo);
    *reinterpret_cast<void **>(p + abi::kRefBaseOffset) = lib.at(abi::napi::kVtableCfgRefBase);
    deviceInfoCtor(p + abi::kDeviceInfoOffset);
    refBaseCtor(p + abi::kBundleRefBaseOffset);
    parcelableCtor(p + abi::kBundleInfoOffset, lib.at(abi::napi::kVttBundleInfoParcelable));
    *reinterpret_cast<void **>(p + abi::kBundleInfoOffset) = lib.at(abi::napi::kVtableBundleInfo);
    *reinterpret_cast<void **>(p + abi::kBundleRefBaseOffset) =
        lib.at(abi::napi::kVtableBundleRefBase);
}

CfgInfoBuilder::~CfgInfoBuilder() {
    // 故意不释放 obj_、也不 dlclose：对象可能仍被服务端/客户端引用，
    // 且 napi 库含单例，卸载会在进程退出阶段段错误。
    obj_ = nullptr;
}

void CfgInfoBuilder::setCpuNum(int v) {
    if (obj_) *reinterpret_cast<std::int32_t *>(static_cast<char *>(obj_) + 12) = v;
}

void CfgInfoBuilder::setMemorySizeMb(int v) {
    if (obj_) *reinterpret_cast<std::int32_t *>(static_cast<char *>(obj_) + 16) = v;
}

void CfgInfoBuilder::setDiskSizeGb(int v) {
    if (obj_) *reinterpret_cast<std::int32_t *>(static_cast<char *>(obj_) + 20) = v;
}

void CfgInfoBuilder::setDiskPartition(bool v) {
    if (obj_) *reinterpret_cast<bool *>(static_cast<char *>(obj_) + 24) = v;
}

void CfgInfoBuilder::setDynamicMemory(bool v) {
    if (obj_) *reinterpret_cast<bool *>(static_cast<char *>(obj_) + 25) = v;
}

void CfgInfoBuilder::setBiosPath(const std::string &v) {
    if (obj_) new (static_cast<char *>(obj_) + 32) std::string(v);
}

void CfgInfoBuilder::setEnhanceFilePath(const std::string &v) {
    if (obj_) new (static_cast<char *>(obj_) + 56) std::string(v);
}

void CfgInfoBuilder::setStartType(int v) {
    if (obj_) *reinterpret_cast<std::int32_t *>(static_cast<char *>(obj_) + 80) = v;
}

std::string CfgInfoBuilder::dump() const {
    if (!obj_) return error_.empty() ? "(未构造)" : error_;
    const char *p = static_cast<const char *>(obj_);
    auto i32 = [&](std::size_t off) { return *reinterpret_cast<const std::int32_t *>(p + off); };
    auto str = [&](std::size_t off) {
        return *reinterpret_cast<const std::string *>(p + off);
    };
    char buf[512];
    snprintf(buf, sizeof buf,
             "CfgInfo@%p base=%p vptr=%p\n"
             "  cpuNum(+12)        = %d\n"
             "  memorySize(+16)    = %d MB\n"
             "  diskSize(+20)      = %d GB\n"
             "  diskPartition(+24) = %d\n"
             "  dynamicMemory(+25) = %d\n"
             "  biosPath(+32)      = \"%s\"\n"
             "  enhanceFile(+56)   = \"%s\"\n"
             "  startType(+80)     = %d",
             obj_, base_, *reinterpret_cast<void *const *>(p), i32(12), i32(16), i32(20),
             static_cast<int>(p[24]), static_cast<int>(p[25]), str(32).c_str(),
             str(56).c_str(), i32(80));
    return buf;
}

}  // namespace hvm
