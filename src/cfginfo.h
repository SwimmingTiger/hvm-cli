// cfginfo.h —— 手工构造 CfgInfo（华为私有无头文件类型）
//
// 该类的构造函数被内联展开且库被 strip，无法 dlsym，因此按逆向出的序列
// 逐字节构造。配方与偏移见 include/ohos/vm_manager_service/cfg_info.h。
#ifndef HVM_CFGINFO_H
#define HVM_CFGINFO_H

#include <cstdint>
#include <string>

namespace hvm {

class CfgInfoBuilder {
  public:
    CfgInfoBuilder();
    ~CfgInfoBuilder();
    CfgInfoBuilder(const CfgInfoBuilder &) = delete;
    CfgInfoBuilder &operator=(const CfgInfoBuilder &) = delete;

    bool ok() const { return obj_ != nullptr; }
    const std::string &lastError() const { return error_; }

    // 以下 setter 的字段语义来自 libvmmanager_napi.z.so 的 UnwrapBaseCfgInfo
    void setCpuNum(int v);                    // +12
    void setMemorySizeMb(int v);              // +16
    void setDiskSizeGb(int v);                // +20
    void setDiskPartition(bool v);            // +24
    void setDynamicMemory(bool v);            // +25
    void setBiosPath(const std::string &v);   // +32  string
    void setEnhanceFilePath(const std::string &v);  // +56  string
    void setStartType(int v);                 // +80  默认 -1，合法 0..3

    //: 生成对象的可读摘要（字段当前值）
    std::string dump() const;

    //: 裸指针（作为 CfgInfo* 传给 VM manager 客户端接口）
    void *raw() const { return obj_; }

  private:
    void *lib_ = nullptr;   // napi 库句柄（不 dlclose）
    char *base_ = nullptr;  // napi 库加载基址
    void *obj_ = nullptr;
    std::string error_;
};

//: 手工构造 MigrationOptions —— ImportVmDiskImage / ExportVmDiskImage 的第 4 个参数。
//: 服务端要求它非空（HandleImportVmDiskImage:1086），配方见 vmmanager 的 ABI 头文件。
class MigrationOptionsBuilder {
  public:
    MigrationOptionsBuilder();
    ~MigrationOptionsBuilder();
    MigrationOptionsBuilder(const MigrationOptionsBuilder &) = delete;
    MigrationOptionsBuilder &operator=(const MigrationOptionsBuilder &) = delete;

    bool ok() const { return obj_ != nullptr; }
    const std::string &lastError() const { return error_; }

    void setKeepSnapshots(bool v);            // +10
    void setPassword(const std::string &v);   // +16（std::string）
    void setForceImport(bool v);              // +40

    std::string dump() const;
    void *raw() const { return obj_; }

  private:
    void *obj_ = nullptr;
    std::string error_;
};

//: 手工构造 ChannelInfo（SendDataToVm / RecvDataFromVm 的描述参数）
class ChannelInfoBuilder {
  public:
    ChannelInfoBuilder(std::uint32_t type, const std::string &name);
    ~ChannelInfoBuilder();
    ChannelInfoBuilder(const ChannelInfoBuilder &) = delete;
    ChannelInfoBuilder &operator=(const ChannelInfoBuilder &) = delete;

    bool ok() const { return obj_ != nullptr; }
    const std::string &lastError() const { return error_; }
    std::string dump() const;
    void *raw() const { return obj_; }

  private:
    void *obj_ = nullptr;
    std::string error_;
};

//: 手工构造 PortInfoList（端口转发条目表），并持有其 sptr 引用
class PortInfoListBuilder {
  public:
    explicit PortInfoListBuilder(const std::vector<std::array<std::uint32_t, 3>> &entries);
    ~PortInfoListBuilder();
    PortInfoListBuilder(const PortInfoListBuilder &) = delete;
    PortInfoListBuilder &operator=(const PortInfoListBuilder &) = delete;

    bool ok() const { return obj_ != nullptr; }
    const std::string &lastError() const { return error_; }
    std::string dump() const;
    void *raw() const { return obj_; }
    //: 传给接口的 sptr 值（对象的地址，已加引用计数）
    void *sptrValue() const { return holder_; }

  private:
    void *obj_ = nullptr;
    void *holder_ = nullptr;
    std::string error_;
};

}  // namespace hvm

#endif  // HVM_CFGINFO_H
