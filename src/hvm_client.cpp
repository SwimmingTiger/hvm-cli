// hvm_client.cpp —— 虚拟机客户端封装实现
#include "hvm_client.h"

#include <dlfcn.h>

#include <map>

namespace hvm {
namespace {

// 客户端 kit 的 C++ 符号（std::string 使用 OHOS libc++ 的 inline namespace __h）
constexpr const char *kWrapper = "_ZN4OHOS16VmManagerService22VmManagerClientWrapper";
constexpr const char *kStringConst =
    "ERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE";
constexpr const char *kStringRef =
    "ERNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE";

// 本地错误码（与服务端返回码区分开）
[[maybe_unused]] constexpr int kErrNotReady = -1001;  // kit 未加载
constexpr int kErrNoSymbol = -1002;  // 符号不存在（版本不匹配）

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
T Client::resolve(const std::string &symbol) const {
    return reinterpret_cast<T>(kit_ ? dlsym(kit_, symbol.c_str()) : nullptr);
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
        resolve<GetInstance>(std::string(kWrapper) + "11GetInstanceEv");
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
    auto f = resolve<Fn>(std::string(kWrapper) + "15GetActiveVmName" + kStringRef);
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::activeVmStatus(int &out) {
    using Fn = int (*)(void *, int &);
    auto f = resolve<Fn>(std::string(kWrapper) + "17GetActiveVmStatusERi");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::vmStatus(const std::string &vm, int &out) {
    using Fn = int (*)(void *, const std::string &, int &);
    auto f = resolve<Fn>(std::string(kWrapper) + "11GetVmStatus" + kStringConst + "Ri");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::checkVmCapability(bool &out) {
    using Fn = int (*)(void *, bool &);
    auto f = resolve<Fn>(std::string(kWrapper) + "17CheckVmCapabilityERb");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::isProcessExist(const std::string &name) {
    using Fn = int (*)(void *, const std::string &);
    auto f = resolve<Fn>(std::string(kWrapper) + "14IsProcessExist" + kStringConst);
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, name);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::isFeatureSupported(int featureId, bool &out) {
    using Fn = int (*)(void *, int, bool &);
    auto f = resolve<Fn>(std::string(kWrapper) + "18IsFeatureSupportedEiRb");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, featureId, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::openEulerVersion(std::string &out) {
    using Fn = int (*)(void *, std::string &);
    auto f = resolve<Fn>(std::string(kWrapper) + "19GetOpenEulerVersion" + kStringRef);
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
    auto f = resolve<Fn>(std::string(kWrapper) + "11GetHashNameEv");
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
    auto f = resolve<Fn>(std::string(kWrapper) + "20IsQuickStartScenarioEv");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::isInstalling() {
    using Fn = int (*)(void *);
    auto f = resolve<Fn>(std::string(kWrapper) + "17CheckIsInstallingEv");
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

// ---------------------------------------------------------------------- 电源
int Client::forceStop(const std::string &vm) {
    using Fn = int (*)(void *, const std::string &);
    auto f = resolve<Fn>(std::string(kWrapper) + "11ForceStopVm" + kStringConst);
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::quitByRebootHost() {
    using Fn = int (*)(void *);
    auto f = resolve<Fn>(std::string(kWrapper) + "18VmQuitByRebootHostEv");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::requireBigMem() {
    using Fn = int (*)(void *);
    auto f = resolve<Fn>(std::string(kWrapper) + "13RequireBigMemEv");
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
    auto f = resolve<Fn>(std::string(kWrapper) + "15GetSnapshotList" + kStringConst +
                         "RNS2_3mapIS8_S8_NS2_4lessIS8_EENS6_INS2_4pairIS9_S8_EEEEEE");
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
    auto f = resolve<Fn>(std::string(kWrapper) + "14CreateSnapshot" + kStringConst + "SA_");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, name);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::snapshotRestore(const std::string &vm, const std::string &name) {
    using Fn = int (*)(void *, const std::string &, const std::string &);
    auto f = resolve<Fn>(std::string(kWrapper) + "15RestoreSnapshot" + kStringConst + "SA_");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, name);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::snapshotDestroy(const std::string &vm, const std::string &name) {
    using Fn = int (*)(void *, const std::string &, const std::string &);
    auto f = resolve<Fn>(std::string(kWrapper) + "15DestroySnapshot" + kStringConst + "SA_");
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
    auto f = resolve<Fn>(std::string(kWrapper) + "14RenameSnapshot" + kStringConst + "SA_SA_");
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
    auto f = resolve<Fn>(std::string(kWrapper) + "15GetSharedFolderEv");
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
    auto f = resolve<Fn>(std::string(kWrapper) + "22GetSharedFolderEnabledEv");
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
    auto f = resolve<Fn>(std::string(kWrapper) + "22SetSharedFolderEnabledEb");
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
    auto f = resolve<Fn>(std::string(kWrapper) + "15AddSharedFolder" + kStringConst + "SA_SA_");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, host, guest);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::removeSharedFolder(const std::string &vm, const std::string &host) {
    using Fn = int (*)(void *, const std::string &, const std::string &);
    auto f = resolve<Fn>(std::string(kWrapper) + "18RemoveSharedFolder" + kStringConst + "SA_");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, host);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::setupSharedFolder(const std::string &vm) {
    using Fn = int (*)(void *, const std::string &);
    auto f = resolve<Fn>(std::string(kWrapper) + "17SetUpSharedFolder" + kStringConst);
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
    auto f = resolve<Fn>(std::string(kWrapper) + "16GetVmIpv4Address" + kStringConst + "RS8_");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::hostNetProxyStatus(const std::string &vm, bool &out) {
    using Fn = int (*)(void *, const std::string &, bool &);
    auto f = resolve<Fn>(std::string(kWrapper) + "23GetVmHostNetProxyStatus" + kStringConst + "Rb");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::switchNetworkShare(const std::string &vm, bool on) {
    using Fn = int (*)(void *, const std::string &, bool);
    auto f = resolve<Fn>(std::string(kWrapper) + "20SwitchVmNetworkShare" + kStringConst + "b");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, on);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::setDnsAutoSync(const std::string &vm, bool on) {
    using Fn = int (*)(void *, const std::string &, bool);
    auto f = resolve<Fn>(std::string(kWrapper) + "21SetDnsAutoSyncEnabled" + kStringConst + "b");
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
    auto f = resolve<Fn>(std::string(kWrapper) + "17GetVmDiskCapacity" + kStringConst + "Rl");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, bytes);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::diskImagePath(const std::string &vm, std::string &out) {
    using Fn = int (*)(void *, const std::string &, std::string &);
    auto f = resolve<Fn>(std::string(kWrapper) + "18GetVmDiskImagePath" + kStringConst + "RS8_");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::diskImageFileSize(const std::string &vm, int64_t &bytes) {
    using Fn = int (*)(void *, const std::string &, int64_t &);
    auto f = resolve<Fn>(std::string(kWrapper) + "22GetVmDiskImageFileSize" + kStringConst + "Rl");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, bytes);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::expandCapacity(const std::string &vm, int sizeGb) {
    using Fn = int (*)(void *, const std::string &, int);
    auto f = resolve<Fn>(std::string(kWrapper) + "16VmExpandCapacity" + kStringConst + "i");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, vm, sizeGb);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::deleteLinuxDataImage() {
    using Fn = int (*)(void *);
    auto f = resolve<Fn>(std::string(kWrapper) + "20DeleteLinuxDataImageEv");
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
    auto f = resolve<Fn>(std::string(kWrapper) + "9GetVmInfoERjS2_");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, ddrSizeMb, vmPid);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::stratovirtMem(int &memMb) {
    using Fn = int (*)(void *);
    auto f = resolve<Fn>(std::string(kWrapper) + "16GetStratovirtMemEv");
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
    auto f = resolve<Fn>(std::string(kWrapper) + "28GetActiveVmStatusForShutdownERi");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, out);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::hostSn(std::string &out) {
    using Fn = int (*)(void *, std::string &);
    auto f = resolve<Fn>(std::string(kWrapper) + "9GetHostSN" + kStringRef);
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
    auto f = resolve<Fn>(std::string(kWrapper) + "16ModifyResolutionEjjb");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, width, height, fullScreen);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::touchVmMem(uint32_t size) {
    using Fn = int (*)(void *, uint32_t);
    auto f = resolve<Fn>(std::string(kWrapper) + "10TouchVmMemEj");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, size);
    } catch (...) {
        return kErrNoSymbol;
    }
}

int Client::set2dSwapSpace(int size) {
    using Fn = int (*)(void *, int);
    auto f = resolve<Fn>(std::string(kWrapper) + "14Set2DSwapSpaceEi");
    if (f == nullptr) return kErrNoSymbol;
    try {
        return f(instance_, size);
    } catch (...) {
        return kErrNoSymbol;
    }
}

}  // namespace hvm
