// fusion_pty.h —— 通过 LinuxFusion PTY 通道连入 openEuler 虚拟机
//
// 使用系统自带 NDK 库 /system/lib64/ndk/libfusion_pty_ndk.so
// （HiShell 终端"连接 openEuler 执行命令"功能用的就是这套接口）。
//
// 逆向得到的 C ABI（SDK 未提供头文件）：
//   int  OhGetPtyManager(void **outManager);
//   int  OhPtyManagerOpenPtySession(void *mgr, void **outSession,
//                                   OhPtyCallback *cb, OhPtyConfig *cfg, int *outSessionId);
//   int  OhPtySessionSendData(void *session, const char *data);   // data 需以 \0 结尾
//   int  OhPtySessionSetWinSize(void *session, OhPtyWinSize *ws);
//   int  OhPtySessionGetSessionId(void *session, int *id);
//   int  OhPtySessionClose(void *session);
//   void OhDeletePtyManager(void *mgr);
//
// 回调（由 InnerPtySessionCallback 转发，实参见 docs/api-notes.md）：
//   void onRecv  (void *session, int sessionId, const char *data);  // data 为 C 字符串
//   void onSignal(void *session, int sessionId, int signal);
//   void onStatus(void *session, int sessionId, int status, int exitCode);
#ifndef HVM_FUSION_PTY_H
#define HVM_FUSION_PTY_H

#include <cstdint>
#include <functional>
#include <string>

namespace hvm {

//: 窗口尺寸：逆向 PtyWinSize，4 个 uint32（Parcel 里按此顺序写入）
struct PtyWinSize {
    uint32_t rows = 24;
    uint32_t cols = 80;
    uint32_t xpixel = 0;
    uint32_t ypixel = 0;
};

//: PTY 配置。字段语义部分已知（见括号），其余留空即用库内默认值。
struct PtyConfig {
    // +0  → 内层 PtyConfig 第 1 个 string（默认 "/bin/bash"）：shell 路径
    std::string shell;
    // +8/+16/+24 → 内层第 2/3/4 个 string：语义未知，留空用默认
    std::string reserved1;
    std::string reserved2;
    std::string reserved3;
    // +32 → 内层第 5 个 string（默认 "openEuler"）：发行版/会话标识
    std::string distro;
    // +40 → 内层第 6 个 string（默认 "root"）：登录用户
    std::string user;
    PtyWinSize winSize;
};

//: 一次 PTY 会话。回调在库的线程上触发，本类做了线程安全缓冲。
class PtySession {
  public:
    using DataCallback = std::function<void(const char *)>;

    PtySession() = default;
    ~PtySession();
    PtySession(const PtySession &) = delete;
    PtySession &operator=(const PtySession &) = delete;

    //: 打开会话；成功返回 0，失败返回库的错误码（负值为本地错误）
    int open(const PtyConfig &cfg, DataCallback onData = nullptr);
    //: 发送一段数据（自动补 '\0'）
    int send(const std::string &data);
    int setWinSize(const PtyWinSize &ws);
    //: 下列三个作用于 manager（不依赖具体会话）
    //: 安装/更新 openEuler 镜像
    int installImage();
    //: 开启"共享目录"（宿主与 openEuler 互通）
    int enableShareFolder();
    //: 查询共享目录开关状态
    int sharedFolderToggleState(bool &enabled);
    int sessionId() const { return sessionId_; }
    void close();

    bool opened() const { return session_ != nullptr; }
    const std::string &lastError() const { return error_; }
    //: 库的加载状态，形如 "dlopen=ok"
    static std::string selfTest();

  private:
    void *manager_ = nullptr;
    void *session_ = nullptr;
    int sessionId_ = -1;
    DataCallback onData_;
    std::string error_;

    friend void ptyTrampolineRecv(void *, int, const char *);
    friend void ptyTrampolineSignal(void *, int, int);
    friend void ptyTrampolineStatus(void *, int, int);
};

//: 等待会话输出里出现指定子串（配合 PtySession 使用，用于等提示符）
//: 返回 true 表示在超时前命中。
bool waitForOutput(const char *needle, int timeoutMs);

//: 记录/读取全局最近一段输出（简单实现：单会话场景足够）
void feedOutput(const char *data);

//: 会话是否已被对端关闭（shell 退出）
bool sessionClosed();
//: 对端退出码（未拿到时为 -1）
int lastExitCode();
//: 重置会话状态（每次 exec/shell 前调用）
void resetSessionState();
//: 等待远端首次产生输出（PTY 通道真正就绪的信号；过早发送会被丢弃）
bool waitForAnyOutput(int timeoutMs);

}  // namespace hvm

#endif  // HVM_FUSION_PTY_H
