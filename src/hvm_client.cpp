// hvm_client.cpp —— 虚拟机客户端封装实现
#include "hvm_client.h"
#include "cfginfo.h"   // PortInfoListBuilder
#include "ohos/vm_manager_service/cfg_info.h"

namespace abi = OHOS::VmManagerService::cfg_info;

#include "ohos/vm_manager_service/vm_manager_kits.syms.h"

#include "ohos/vm_manager_service/vm_manager_errcode.h"

#include <dlfcn.h>

#include <map>

namespace hvm {
namespace {


// 本地错误码统一使用 include/ohos/vm_manager_service/vm_manager_errcode.h 中的定义
constexpr int kErrNoSymbol = OHOS_VM_ERR_SYMBOL_MISSING;
// 调用方应先检查 ready()；此处保留常量以便按需返回
[[maybe_unused]] constexpr int kErrNotReady = OHOS_VM_ERR_KIT_NOT_LOADED;

}  // namespace

const char *statusName(int status) {
    switch (static_cast<VmStatus>(status)) {
        case VmStatus::None:
            return "none";
        case VmStatus::Unknown:
            return "unknown";
        default:
            return "?";
    }
}

template <typename T>
T Client::resolve(const char *methodName) const {
    // 符号名由 scripts/gen-wrapper-api.py 从声明生成并与设备符号核对过，
    // 不在代码里手写 mangled 字符串（手抄极易出错）。
    const char *mangled = OHOS::VmManagerService::abi::FindSym(methodName);
    if (kit_ == nullptr || mangled == nullptr) return nullptr;
    return reinterpret_cast<T>(dlsym(kit_, mangled));
}

Client::Client() {
    kit_ = dlopen(kKitName, RTLD_NOW | RTLD_GLOBAL);
    if (kit_ == nullptr) {
        const char *err = dlerror();
        error_ = std::string("dlopen 失败: ") + (err ? err : "未知原因");
        return;
    }
    using GetInstance = void *(*)();
    auto getInstance =
        resolve<GetInstance>("GetInstance");
    if (getInstance == nullptr) {
        error_ = "找不到 VmManagerClientWrapper::GetInstance 符号";
        return;
    }
    instance_ = getInstance();
    if (instance_ == nullptr) {
        error_ = "GetInstance() 返回空";
    }
}

Client::~Client() {
    // 故意不 dlclose：客户端 kit 里有单例与静态对象，卸载后其析构仍会执行，
    // 会在进程退出阶段段错误。进程退出时由内核回收映射即可。
    instance_ = nullptr;
}

std::string Client::selfTest() const {
    std::string s = "dlopen=";
    s += (kit_ != nullptr) ? "ok" : "fail";
    s += ";instance=";
    s += (instance_ != nullptr) ? "ok" : "null";
    if (!error_.empty()) {
        s += ";error=";
        s += error_;
    }
    return s;
}

// ---------------------------------------------------------------------- 状态
int Client::activeVmName(std::string &out) {
    using Fn = int (*)(void *, std::string &);
    auto f = resolve<Fn>("GetActiveVmName");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::activeVmStatus(int &out) {
    using Fn = int (*)(void *, int &);
    auto f = resolve<Fn>("GetActiveVmStatus");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::vmStatus(const std::string &vm, int &out) {
    using Fn = int (*)(void *, const std::string &, int &);
    auto f = resolve<Fn>("GetVmStatus");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::checkVmCapability(bool &out) {
    using Fn = int (*)(void *, bool &);
    auto f = resolve<Fn>("CheckVmCapability");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::isProcessExist(const std::string &name) {
    using Fn = int (*)(void *, const std::string &);
    auto f = resolve<Fn>("IsProcessExist");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, name);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::isFeatureSupported(int featureId, bool &out) {
    using Fn = int (*)(void *, int, bool &);
    auto f = resolve<Fn>("IsFeatureSupported");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, featureId, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::availableCpuRange(uint32_t &minVal, uint32_t &maxVal) {
    using Fn = int (*)(void *, unsigned int &, unsigned int &);
    auto f = resolve<Fn>("GetVmAvailableCpuNumRange");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, minVal, maxVal);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::availableMemoryRange(uint32_t &minVal, uint32_t &maxVal) {
    using Fn = int (*)(void *, unsigned int &, unsigned int &);
    auto f = resolve<Fn>("GetVmAvailableMemorySizeRange");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, minVal, maxVal);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::openEulerVersion(std::string &out) {
    using Fn = int (*)(void *, std::string &);
    auto f = resolve<Fn>("GetOpenEulerVersion");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::hashName(std::string &out) {
    // 该接口按值返回 std::string（sret）
    using Fn = std::string (*)(void *);
    auto f = resolve<Fn>("GetHashName");
    if (f == nullptr) return kErrNoSymbol;
    try {
        out = f(instance_);
        return 0;
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::isQuickStartScenario() {
    using Fn = int (*)(void *);
    auto f = resolve<Fn>("IsQuickStartScenario");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::isInstalling() {
    using Fn = int (*)(void *);
    auto f = resolve<Fn>("CheckIsInstalling");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_);
    } catch (...) {
        return kErrNoSymbol;
    }
}

VmInfo Client::info() {
    VmInfo vi;
    bool cap = false;
    vi.rcCapability = checkVmCapability(cap);
    vi.capable = cap;
    vi.rcActiveName = activeVmName(vi.activeVm);
    vi.rcActiveStatus = activeVmStatus(vi.activeStatus);
    vi.rcOpenEulerVersion = openEulerVersion(vi.openEulerVersion);
    bool enabled = false;
    vi.rcSharedFolderEnabled = sharedFolderEnabled(enabled);
    vi.sharedFolderEnabled = enabled;
    return vi;
}

// ---------------------------------------------------------------------- 生命周期
int Client::createVm(const std::string &name, const std::string &imagePath, void *cfgObj) {
    // 符号：...8CreateVmERKNSt3__h...EESA_RKNS_4sptrINS0_7CfgInfoEEE
    // 第 4 参是 const sptr<CfgInfo>&，而 sptr 的内存布局就是单个指针，
    // 因此传入「指向该指针的指针」即 ABI 等价。
    using Fn = int (*)(void *, const std::string &, const std::string &, const void *);
    auto f = resolve<Fn>("CreateVm");
    if (f == nullptr) return kErrNoSymbol;
    void *holder = cfgObj;
    try {
        return f(instance_, name, imagePath, &holder);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::startVm(const std::string &name, void *cfgObj) {
    using Fn = int (*)(void *, const std::string &, const void *);
    auto f = resolve<Fn>("StartVm");
    if (f == nullptr) return kErrNoSymbol;
    void *holder = cfgObj;
    try {
        return f(instance_, name, &holder);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::destroyVm(const std::string &name) {
    using Fn = int (*)(void *, const std::string &);
    auto f = resolve<Fn>("DestroyVm");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, name);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::mountCdDrive(const std::string &name, const std::string &path, bool insert,
                         std::string &out) {
    using Fn = int (*)(void *, const std::string &, const std::string &, bool, std::string &);
    auto f = resolve<Fn>("MountCDDriveToVm");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, name, path, insert, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::unmountCdDrive(const std::string &name, const std::string &path) {
    using Fn = int (*)(void *, const std::string &, const std::string &);
    auto f = resolve<Fn>("UnmountCDDriveFromVm");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, name, path);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::importVmDiskImage(const std::string &name, const std::string &src,
                              const std::string &dst, void *opts) {
    // 符号：...17ImportVmDiskImageERKNSt3__h...EESA_SA_RKNS_4sptrINS_16MigrationOptionsEEE
    using Fn = int (*)(void *, const std::string &, const std::string &, const std::string &,
                       const void *);
    auto f = resolve<Fn>("ImportVmDiskImage");
    if (f == nullptr) return kErrNoSymbol;
    // 第 5 个参数是 const sptr<MigrationOptions>&；sptr 的内存布局就是一个指针，
    // 因此传入「指向该指针的指针」即 ABI 等价（与 CreateVm/StartVm 的 sptr 同理）。
    void *holder = opts;
    try {
        return f(instance_, name, src, dst, &holder);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::exportVmDiskImage(const std::string &name, const std::string &src,
                              const std::string &dst, bool isRaw, void *opts) {
    // 符号：...17ExportVmDiskImageERKNSt3__h...EESA_SA_bRKNS_4sptrINS0_16MigrationOptionsEEE
    using Fn = int (*)(void *, const std::string &, const std::string &, const std::string &,
                       bool, const void *);
    auto f = resolve<Fn>("ExportVmDiskImage");
    if (f == nullptr) return kErrNoSymbol;
    void *holder = opts;
    try {
        return f(instance_, name, src, dst, isRaw, &holder);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::sendDataToVm(const std::string &vm, const std::vector<uint8_t> &data, int arg,
                         const void *ch) {
    using Fn = int (*)(void *, const std::string &, const std::vector<uint8_t> &, int,
                       const void *);
    auto f = resolve<Fn>("SendDataToVm");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, data, arg, ch);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::recvDataFromVm(const std::string &vm, std::vector<uint8_t> &data, int arg,
                           const void *ch) {
    using Fn = int (*)(void *, const std::string &, std::vector<uint8_t> &, int, const void *);
    auto f = resolve<Fn>("RecvDataFromVm");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, data, arg, ch);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::progressDiedState(int viewState) {
    using Fn = int (*)(void *, const int32_t *);
    auto f = resolve<Fn>("ProgressDiedStateToVm");
    if (f == nullptr) return kErrNoSymbol;
    int32_t state = viewState;
    try {
        return f(instance_, &state);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::displaysNumber(const std::vector<uint64_t> &displayIds) {
    using Fn = int (*)(void *, const std::vector<uint64_t> &);
    auto f = resolve<Fn>("DisplaysNumber");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, displayIds);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::stopVm(const std::string &name, bool clean) {
    using Fn = int (*)(void *, const std::string &, bool);
    auto f = resolve<Fn>("StopVm");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, name, clean);
    } catch (...) {
        return kErrNoSymbol;
    }
}


// ------------------------------------------------- LinuxFusion / RGM 运维
int Client::pauseVm() {
    using Fn = int (*)(void *);
    auto f = resolve<Fn>("PauseVm");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_); } catch (...) { return kErrNoSymbol; }
}

int Client::resumeVm(const std::string &vm) {
    using Fn = int (*)(void *, const std::string &);
    auto f = resolve<Fn>("ResumeVm");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, vm); } catch (...) { return kErrNoSymbol; }
}

int Client::allSharedVolume(std::vector<std::string> &out) {
    // 按值返回 std::vector<std::string>（sret）
    using Fn = std::vector<std::string> (*)(void *);
    auto f = resolve<Fn>("GetAllSharedVolume");
    if (f == nullptr) return kErrNoSymbol;
    try { out = f(instance_); return 0; } catch (...) { return kErrNoSymbol; }
}

int Client::linuxPathFromOhPath(const std::vector<std::string> &in,
                                std::vector<std::string> &out) {
    using Fn = int (*)(void *, const std::vector<std::string> &, std::vector<std::string> &);
    auto f = resolve<Fn>("GetLinuxPathFromOhPath");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, in, out); } catch (...) { return kErrNoSymbol; }
}

int Client::deleteRgmImageFromVm(const std::string &name) {
    using Fn = int (*)(void *, const std::string &);
    auto f = resolve<Fn>("DeleteRgmImageFromVm");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, name); } catch (...) { return kErrNoSymbol; }
}

int Client::setHostGalleryShared(const std::string &vm, bool on) {
    using Fn = int (*)(void *, const std::string &, bool);
    auto f = resolve<Fn>("SetHostGallerySharedEnabled");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, vm, on); } catch (...) { return kErrNoSymbol; }
}

int Client::pasteboardEnableState(bool &out) {
    using Fn = bool (*)(void *);
    auto f = resolve<Fn>("GetPasteboardEnableState");
    if (f == nullptr) return kErrNoSymbol;
    try { out = f(instance_); return 0; } catch (...) { return kErrNoSymbol; }
}

int Client::setPasteboardEnableState(bool on) {
    using Fn = int (*)(void *, bool);
    auto f = resolve<Fn>("SetPasteboardEnableState");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, on); } catch (...) { return kErrNoSymbol; }
}

int Client::pasteboardUsableState(bool &out) {
    using Fn = bool (*)(void *);
    auto f = resolve<Fn>("GetPasteboardUsableState");
    if (f == nullptr) return kErrNoSymbol;
    try { out = f(instance_); return 0; } catch (...) { return kErrNoSymbol; }
}

int Client::setPasteboardUsableState(bool on) {
    using Fn = int (*)(void *, bool);
    auto f = resolve<Fn>("SetPasteboardUsableState");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, on); } catch (...) { return kErrNoSymbol; }
}

int Client::addPasteboardSharedFolder(const std::string &a, const std::string &b) {
    using Fn = int (*)(void *, const std::string &, const std::string &);
    auto f = resolve<Fn>("AddPasteboardSharedFolder");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, a, b); } catch (...) { return kErrNoSymbol; }
}

int Client::removePasteboardSharedFolder(const std::string &a) {
    using Fn = int (*)(void *, const std::string &);
    auto f = resolve<Fn>("RemovePasteboardSharedFolder");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, a); } catch (...) { return kErrNoSymbol; }
}

int Client::setVmHostNetProxyStatus(const std::string &vm, bool on) {
    using Fn = int (*)(void *, const std::string &, bool);
    auto f = resolve<Fn>("SetVmHostNetProxyStatus");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, vm, on); } catch (...) { return kErrNoSymbol; }
}

int Client::setProxyAutoSyncEnabled(const std::string &vm, bool on) {
    using Fn = int (*)(void *, const std::string &, bool);
    auto f = resolve<Fn>("SetProxyAutoSyncEnabled");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, vm, on); } catch (...) { return kErrNoSymbol; }
}

int Client::setGuestDiskShared(const std::string &vm, const std::string &path, bool on) {
    using Fn = int (*)(void *, const std::string &, const std::string &, bool);
    auto f = resolve<Fn>("SetGuestDiskShared");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, vm, path, on); } catch (...) { return kErrNoSymbol; }
}

int Client::vmUniSocPerfRequest(const std::string &a, const std::string &b) {
    using Fn = int (*)(void *, const std::string &, const std::string &);
    auto f = resolve<Fn>("VmUniSocPerfRequest");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, a, b); } catch (...) { return kErrNoSymbol; }
}

int Client::vmUniSocPerfRequestEx(const std::string &a, bool flag, const std::string &b) {
    using Fn = int (*)(void *, const std::string &, bool, const std::string &);
    auto f = resolve<Fn>("VmUniSocPerfRequestEx");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, a, flag, b); } catch (...) { return kErrNoSymbol; }
}

int Client::sysAvailBufferLimit(uint64_t v) {
    using Fn = int (*)(void *, uint64_t);
    auto f = resolve<Fn>("SysAvailBufferLimit");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, v); } catch (...) { return kErrNoSymbol; }
}

int Client::sysLowBufferLimit(uint64_t v) {
    using Fn = int (*)(void *, uint64_t);
    auto f = resolve<Fn>("SysLowBufferLimit");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, v); } catch (...) { return kErrNoSymbol; }
}

int Client::toggleScreenLockTask(bool on) {
    using Fn = int (*)(void *, bool);
    auto f = resolve<Fn>("ToggleScreenLockTask");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, on); } catch (...) { return kErrNoSymbol; }
}

int Client::tabletSwitchChanged(int v) {
    using Fn = int (*)(void *, int32_t);
    auto f = resolve<Fn>("TabletSwitchChanged");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, v); } catch (...) { return kErrNoSymbol; }
}

int Client::lockGuest() {
    using Fn = int (*)(void *);
    auto f = resolve<Fn>("LockGuest");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_); } catch (...) { return kErrNoSymbol; }
}

int Client::lxOtaHandle() {
    using Fn = int (*)(void *);
    auto f = resolve<Fn>("LxOtaHandle");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_); } catch (...) { return kErrNoSymbol; }
}

int Client::handleLxSnapshot(const std::string &vm, const std::string &name, int32_t op) {
    using Fn = int (*)(void *, const std::string &, const std::string &, int32_t);
    auto f = resolve<Fn>("HandleLxSnapshot");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, vm, name, op); } catch (...) { return kErrNoSymbol; }
}

int Client::rgmImageStatusFromVm(const std::string &name) {
    using Fn = int (*)(void *, const std::string &);
    auto f = resolve<Fn>("GetRgmImageStatusFromVm");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, name); } catch (...) { return kErrNoSymbol; }
}

int Client::recoverUserData(const std::string &vm, const std::string &path) {
    using Fn = int (*)(void *, const std::string &, const std::string &);
    auto f = resolve<Fn>("RecoverUserData");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, vm, path); } catch (...) { return kErrNoSymbol; }
}

int Client::setAutoPauseTime(const std::string &vm, int32_t minutes) {
    // AutoPauseTime 是 32 位枚举，按 const& 传递 → 直接给地址
    using Fn = int (*)(void *, const std::string &, const int32_t *);
    auto f = resolve<Fn>("SetAutoPauseTime");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, vm, &minutes); } catch (...) { return kErrNoSymbol; }
}

int Client::setVmNetMode(const std::string &vm, int32_t mode, const std::string &iface) {
    using Fn = int (*)(void *, const std::string &, const int32_t *, const std::string &);
    auto f = resolve<Fn>("SetVmNetMode");
    if (f == nullptr) return kErrNoSymbol;
    try { return f(instance_, vm, &mode, iface); } catch (...) { return kErrNoSymbol; }
}

//: 从 GetXxx 返回的 sptr<PortInfoList> 里取出条目（vector 位于对象 +16）
static std::vector<std::array<std::uint32_t, 3>> readPortInfoVector(void *listObj) {
    std::vector<std::array<std::uint32_t, 3>> out;
    if (listObj == nullptr) return out;
    const char *p = static_cast<const char *>(listObj);
    using Vec = std::vector<std::array<std::uint32_t, 3>>;
    const auto *vec = *reinterpret_cast<Vec *const *>(p + abi::portinfo::kVectorBegin);
    if (vec == nullptr) return out;
    out = *vec;
    return out;
}

int Client::getPortForwardForNat(const std::string &vm,
                                 std::vector<std::array<std::uint32_t, 3>> &out) {
    using Fn = int (*)(void *, const std::string &, void **);
    auto f = resolve<Fn>("GetPortForwardForNat");
    if (f == nullptr) return kErrNoSymbol;
    void *slot = nullptr;   // sptr<PortInfoList>& —— 传槽位地址
    int rc = kErrNoSymbol;
    try {
        rc = f(instance_, vm, &slot);
    } catch (...) {
        return kErrNoSymbol;
    }
    if (rc != 0) return rc;
    out = readPortInfoVector(slot);
    return 0;
}

int Client::setPortForwardForNat(const std::string &vm, uint32_t arg,
                                 const std::vector<std::array<std::uint32_t, 3>> &entries) {
    hvm::PortInfoListBuilder b(entries);
    if (!b.ok()) return kErrNoSymbol;
    using Fn = int (*)(void *, const std::string &, uint32_t, void *);
    auto f = resolve<Fn>("SetPortForwardForNat");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, arg, b.sptrValue());
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::getLocalhostForwardFromVmToHost(
    const std::string &vm, std::vector<std::array<std::uint32_t, 3>> &out) {
    using Fn = int (*)(void *, const std::string &, void **);
    auto f = resolve<Fn>("GetLocalhostForwardFromVmToHost");
    if (f == nullptr) return kErrNoSymbol;
    void *slot = nullptr;
    int rc = kErrNoSymbol;
    try {
        rc = f(instance_, vm, &slot);
    } catch (...) {
        return kErrNoSymbol;
    }
    if (rc != 0) return rc;
    out = readPortInfoVector(slot);
    return 0;
}

int Client::setLocalhostForwardFromVmToHost(
    const std::string &vm, uint32_t arg,
    const std::vector<std::array<std::uint32_t, 3>> &entries) {
    hvm::PortInfoListBuilder b(entries);
    if (!b.ok()) return kErrNoSymbol;
    using Fn = int (*)(void *, const std::string &, uint32_t, void *);
    auto f = resolve<Fn>("SetLocalhostForwardFromVmToHost");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, arg, b.sptrValue());
    } catch (...) {
        return kErrNoSymbol;
    }
}

// ---------------------------------------------------------------------- 电源
int Client::forceStop(const std::string &vm) {
    using Fn = int (*)(void *, const std::string &);
    auto f = resolve<Fn>("ForceStopVm");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::quitByRebootHost() {
    using Fn = int (*)(void *);
    auto f = resolve<Fn>("VmQuitByRebootHost");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::requireBigMem() {
    using Fn = int (*)(void *);
    auto f = resolve<Fn>("RequireBigMem");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_);
    } catch (...) {
        return kErrNoSymbol;
    }
}

// ---------------------------------------------------------------------- 快照
int Client::snapshotList(const std::string &vm,
                         std::vector<std::pair<std::string, std::string>> &out) {
    using Fn = int (*)(void *, const std::string &, std::map<std::string, std::string> &);
    auto f = resolve<Fn>("GetSnapshotList");
    if (f == nullptr) return kErrNoSymbol;
    try {
        std::map<std::string, std::string> m;
        int rc = f(instance_, vm, m);
        if (rc != 0) return rc;
        out.assign(m.begin(), m.end());
        return 0;
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::snapshotCreate(const std::string &vm, const std::string &name) {
    using Fn = int (*)(void *, const std::string &, const std::string &);
    auto f = resolve<Fn>("CreateSnapshot");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, name);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::snapshotRestore(const std::string &vm, const std::string &name) {
    using Fn = int (*)(void *, const std::string &, const std::string &);
    auto f = resolve<Fn>("RestoreSnapshot");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, name);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::snapshotDestroy(const std::string &vm, const std::string &name) {
    using Fn = int (*)(void *, const std::string &, const std::string &);
    auto f = resolve<Fn>("DestroySnapshot");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, name);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::snapshotRename(const std::string &vm, const std::string &from,
                           const std::string &to) {
    using Fn = int (*)(void *, const std::string &, const std::string &, const std::string &);
    auto f = resolve<Fn>("RenameSnapshot");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, from, to);
    } catch (...) {
        return kErrNoSymbol;
    }
}

// ---------------------------------------------------------------------- 共享目录
int Client::sharedFolder(std::string &out) {
    using Fn = std::string (*)(void *);
    auto f = resolve<Fn>("GetSharedFolder");
    if (f == nullptr) return kErrNoSymbol;
    try {
        out = f(instance_);
        return 0;
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::sharedFolderEnabled(bool &out) {
    using Fn = bool (*)(void *);
    auto f = resolve<Fn>("GetSharedFolderEnabled");
    if (f == nullptr) return kErrNoSymbol;
    try {
        out = f(instance_);
        return 0;
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::setSharedFolderEnabled(bool enabled) {
    using Fn = void (*)(void *, bool);
    auto f = resolve<Fn>("SetSharedFolderEnabled");
    if (f == nullptr) return kErrNoSymbol;
    try {
        f(instance_, enabled);
        return 0;
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::addSharedFolder(const std::string &vm, const std::string &host,
                            const std::string &guest) {
    using Fn = int (*)(void *, const std::string &, const std::string &, const std::string &);
    auto f = resolve<Fn>("AddSharedFolder");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, host, guest);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::removeSharedFolder(const std::string &vm, const std::string &host) {
    using Fn = int (*)(void *, const std::string &, const std::string &);
    auto f = resolve<Fn>("RemoveSharedFolder");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, host);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::setupSharedFolder(const std::string &vm) {
    using Fn = int (*)(void *, const std::string &);
    auto f = resolve<Fn>("SetUpSharedFolder");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm);
    } catch (...) {
        return kErrNoSymbol;
    }
}

// ---------------------------------------------------------------------- 网络
int Client::vmIpv4Address(const std::string &vm, std::string &out) {
    using Fn = int (*)(void *, const std::string &, std::string &);
    auto f = resolve<Fn>("GetVmIpv4Address");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::hostNetProxyStatus(const std::string &vm, bool &out) {
    using Fn = int (*)(void *, const std::string &, bool &);
    auto f = resolve<Fn>("GetVmHostNetProxyStatus");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::switchNetworkShare(const std::string &vm, bool on) {
    using Fn = int (*)(void *, const std::string &, bool);
    auto f = resolve<Fn>("SwitchVmNetworkShare");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, on);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::setDnsAutoSync(const std::string &vm, bool on) {
    using Fn = int (*)(void *, const std::string &, bool);
    auto f = resolve<Fn>("SetDnsAutoSyncEnabled");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, on);
    } catch (...) {
        return kErrNoSymbol;
    }
}

// ---------------------------------------------------------------------- 磁盘
int Client::diskCapacity(const std::string &vm, int64_t &bytes) {
    using Fn = int (*)(void *, const std::string &, int64_t &);
    auto f = resolve<Fn>("GetVmDiskCapacity");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, bytes);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::diskImagePath(const std::string &vm, std::string &out) {
    using Fn = int (*)(void *, const std::string &, std::string &);
    auto f = resolve<Fn>("GetVmDiskImagePath");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::diskImageFileSize(const std::string &vm, int64_t &bytes) {
    using Fn = int (*)(void *, const std::string &, int64_t &);
    auto f = resolve<Fn>("GetVmDiskImageFileSize");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, bytes);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::expandCapacity(const std::string &vm, int sizeGb) {
    using Fn = int (*)(void *, const std::string &, int);
    auto f = resolve<Fn>("VmExpandCapacity");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, sizeGb);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::deleteLinuxDataImage() {
    using Fn = int (*)(void *);
    auto f = resolve<Fn>("DeleteLinuxDataImage");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_);
    } catch (...) {
        return kErrNoSymbol;
    }
}

// ---------------------------------------------------------------------- 虚拟机信息
int Client::getVmInfo(uint32_t &ddrSizeMb, uint32_t &vmPid) {
    using Fn = int (*)(void *, unsigned int &, unsigned int &);
    auto f = resolve<Fn>("GetVmInfo");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, ddrSizeMb, vmPid);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::stratovirtMem(int &memMb) {
    using Fn = int (*)(void *);
    auto f = resolve<Fn>("GetStratovirtMem");
    if (f == nullptr) return kErrNoSymbol;
    try {
        memMb = f(instance_);
        return 0;
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::activeVmStatusForShutdown(int &out) {
    using Fn = int (*)(void *, int &);
    auto f = resolve<Fn>("GetActiveVmStatusForShutdown");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::hostSn(std::string &out) {
    using Fn = int (*)(void *, std::string &);
    auto f = resolve<Fn>("GetHostSN");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

// ---------------------------------------------------------------------- 显示 / 内存
int Client::modifyResolution(uint32_t width, uint32_t height, bool fullScreen) {
    using Fn = int (*)(void *, uint32_t, uint32_t, bool);
    auto f = resolve<Fn>("ModifyResolution");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, width, height, fullScreen);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::touchVmMem(uint32_t size) {
    using Fn = int (*)(void *, uint32_t);
    auto f = resolve<Fn>("TouchVmMem");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, size);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::set2dSwapSpace(int size) {
    using Fn = int (*)(void *, int);
    auto f = resolve<Fn>("Set2DSwapSpace");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, size);
    } catch (...) {
        return kErrNoSymbol;
    }
}

}  // namespace hvm
