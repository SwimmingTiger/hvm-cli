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
    // 以下为实测观察值（枚举本身未导出，仅记录已验证的取值）
    Running = 9,  // 启动后：vm-info 能取到 stratoVirt 进程 PID、DDR 大小非 0
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
    //: 虚拟机可用 CPU 数范围 (min, max)
    int availableCpuRange(uint32_t &minVal, uint32_t &maxVal);
    //: 虚拟机可用内存范围 (min, max)，单位同 memorySize 字段
    int availableMemoryRange(uint32_t &minVal, uint32_t &maxVal);
    int openEulerVersion(std::string &out);
    int hashName(std::string &out);
    int isQuickStartScenario();
    int isInstalling();
    VmInfo info();

    // ------------------------------------------------------------ 生命周期
    //: 创建虚拟机。cfgObj 为 CfgInfoBuilder 构造出的对象（见 src/cfginfo.h）
    int createVm(const std::string &name, const std::string &imagePath, void *cfgObj);
    //: 启动已创建的虚拟机
    int startVm(const std::string &name, void *cfgObj);
    //: 销毁虚拟机（不需要 CfgInfo）
    int destroyVm(const std::string &name);
    //: 给虚拟机挂载/卸载光盘（安装 ISO）。成功时 out 返回挂载结果描述
    int mountCdDrive(const std::string &name, const std::string &path, bool insert,
                     std::string &out);
    int unmountCdDrive(const std::string &name, const std::string &path);
    //: 让服务端自己把 src 处文件拷到 dst（服务进程有权限读写用户区，且目标落在
    //: 服务数据区时标签正确）—— 实测用于把 ISO 搬进 stratovirt 读得到的地方
    //: opts 为 MigrationOptionsBuilder 构造出的对象指针（服务端要求非空）
    int importVmDiskImage(const std::string &name, const std::string &src,
                          const std::string &dst, void *opts);
    int stopVm(const std::string &name, bool clean);

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

    // ------------------------------------------------------------ 虚拟机信息
    //: 出参为 (虚拟机 DDR 大小 MB, 虚拟机进程 PID)
    int getVmInfo(uint32_t &ddrSizeMb, uint32_t &vmPid);
    //: stratoVirt 占用内存（MB）
    int stratovirtMem(int &memMb);
    //: 关机流程使用的活动状态
    int activeVmStatusForShutdown(int &out);
    //: 宿主 SN
    int hostSn(std::string &out);

    // ------------------------------------------------------------ 显示 / 内存
    int modifyResolution(uint32_t width, uint32_t height, bool fullScreen);
    int touchVmMem(uint32_t size);
    int set2dSwapSpace(int size);

  private:
    void *kit_ = nullptr;       //: dlopen 句柄
    void *instance_ = nullptr;  //: VmManagerClientWrapper 单例（sptr 里的裸指针）
    std::string error_;

    //: 按**方法名**在生成的符号表里查 mangled 名并 dlsym（见 vm_manager_kits.syms.h）
    template <typename T>
    T resolve(const char *methodName) const;
};

}  // namespace hvm

#endif  // HVM_CLIENT_H
