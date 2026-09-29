// hvm_client.h —— 鸿蒙 PC 虚拟机客户端封装
//
// 直接使用系统自带的 C++ 库 /system/lib64/libvm_manager_kits.z.so
// （OHOS::VmManagerService::VmManagerClientWrapper），
// 通过 dlopen + dlsym 解析 mangled 符号后调用，无需 root、无需 HAP。
#ifndef HVM_CLIENT_H
#define HVM_CLIENT_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace hvm {

//: 系统内置 Linux（openEuler）虚拟机的名字
inline constexpr const char *kLinuxVm = "virtualized_linux";

//: 客户端 kit 的路径（实际由动态链接器在 /system/lib64 下解析）
inline constexpr const char *kKitName = "libvm_manager_kits.z.so";

//: 虚拟机状态码。
//: 服务端未把枚举名编进二进制，目前只实测到 NONE（无虚拟机在跑），
//: 其余取值需在有虚拟机运行时逐个观测补全。
enum class VmStatus : int {
    Unknown = -1,
    None = 0,
};

const char *statusName(int status);

//: info() 的汇总结果
struct VmInfo {
    bool capable = false;
    std::string activeVm;
    int activeStatus = 0;
    std::string openEulerVersion;
    bool sharedFolderEnabled = false;
    //: 各子调用的原始返回码，便于排查
    int rcCapability = 0;
    int rcActiveName = 0;
    int rcActiveStatus = 0;
    int rcOpenEulerVersion = 0;
    int rcSharedFolderEnabled = 0;
};

//: 客户端。构造即尝试加载 kit，ready() 为 false 时看 lastError()。
class Client {
  public:
    Client();
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    bool ready() const { return instance_ != nullptr; }
    const std::string &lastError() const { return error_; }

    //: 输出 "dlopen=ok;instance=ok" 之类的自检信息
    std::string selfTest() const;

    // ------------------------------------------------------------ 状态
    int activeVmName(std::string &out);
    int activeVmStatus(int &out);
    int vmStatus(const std::string &vm, int &out);
    int checkVmCapability(bool &out);
    //: 返回原始码：1 表示存在，0 表示不存在
    int isProcessExist(const std::string &name);
    int isFeatureSupported(int featureId, bool &out);
    int openEulerVersion(std::string &out);
    int hashName(std::string &out);
    int isQuickStartScenario();
    int isInstalling();
    VmInfo info();

    // ------------------------------------------------------------ 电源
    int forceStop(const std::string &vm);
    int quitByRebootHost();
    int requireBigMem();

    // ------------------------------------------------------------ 快照
    int snapshotList(const std::string &vm,
                     std::vector<std::pair<std::string, std::string>> &out);
    int snapshotCreate(const std::string &vm, const std::string &name);
    int snapshotRestore(const std::string &vm, const std::string &name);
    int snapshotDestroy(const std::string &vm, const std::string &name);
    int snapshotRename(const std::string &vm, const std::string &from, const std::string &to);

    // ------------------------------------------------------------ 共享目录
    int sharedFolder(std::string &out);
    int sharedFolderEnabled(bool &out);
    int setSharedFolderEnabled(bool enabled);
    int addSharedFolder(const std::string &vm, const std::string &host, const std::string &guest);
    int removeSharedFolder(const std::string &vm, const std::string &host);
    int setupSharedFolder(const std::string &vm);

    // ------------------------------------------------------------ 网络
    int vmIpv4Address(const std::string &vm, std::string &out);
    int hostNetProxyStatus(const std::string &vm, bool &out);
    int switchNetworkShare(const std::string &vm, bool on);
    int setDnsAutoSync(const std::string &vm, bool on);

    // ------------------------------------------------------------ 磁盘
    int diskCapacity(const std::string &vm, int64_t &bytes);
    int diskImagePath(const std::string &vm, std::string &out);
    int diskImageFileSize(const std::string &vm, int64_t &bytes);
    int expandCapacity(const std::string &vm, int sizeGb);
    int deleteLinuxDataImage();

    // ------------------------------------------------------------ 显示 / 内存
    int modifyResolution(uint32_t width, uint32_t height, bool fullScreen);
    int touchVmMem(uint32_t size);
    int set2dSwapSpace(int size);

  private:
    void *kit_ = nullptr;       //: dlopen 句柄
    void *instance_ = nullptr;  //: VmManagerClientWrapper 单例（sptr 里的裸指针）
    std::string error_;

    template <typename T>
    T resolve(const std::string &symbol) const;
};

}  // namespace hvm

#endif  // HVM_CLIENT_H
