// fusion_pty.cpp —— LinuxFusion PTY 通道实现
#include "fusion_pty.h"

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

namespace hvm {
namespace {

// 逆向得到的结构布局（见 docs/api-notes.md）
struct RawConfig {
    const char *f0;
    const char *f1;
    const char *f2;
    const char *f3;
    const char *f4;
    const char *f5;
    uint32_t rows;
    uint32_t cols;
    uint32_t xpixel;
    uint32_t ypixel;
};

struct RawWinSize {
    uint32_t rows;
    uint32_t cols;
    uint32_t xpixel;
    uint32_t ypixel;
};

struct RawCallback {
    void *onRecv;
    void *onSignal;
    void *onStatus;
};

void *g_lib = nullptr;
std::string g_loadError;

//: 同一时刻只维护一个活动会话（CLI 场景足够，也避免回调分发歧义）
PtySession *g_active = nullptr;

std::mutex g_outMutex;
std::condition_variable g_outCv;
std::string g_output;

std::atomic<bool> g_closed{false};
std::atomic<int> g_exitCode{-1};

bool ensureLib() {
    if (g_lib != nullptr) return true;
    g_lib = dlopen("libfusion_pty_ndk.so", RTLD_NOW | RTLD_GLOBAL);
    if (g_lib == nullptr) {
        const char *e = dlerror();
        g_loadError = e ? e : "未知原因";
        return false;
    }
    return true;
}

template <typename T>
T sym(const char *name) {
    return reinterpret_cast<T>(g_lib ? dlsym(g_lib, name) : nullptr);
}

}  // namespace

// ------------------------------------------------------------------ 回调跳板
void ptyTrampolineRecv(void * /*session*/, int /*sessionId*/, const char *data) {
    if (data == nullptr) return;
    {
        std::lock_guard<std::mutex> lk(g_outMutex);
        g_output += data;
    }
    g_outCv.notify_all();
    if (g_active != nullptr && g_active->onData_) {
        g_active->onData_(data);
    }
}

void ptyTrampolineSignal(void * /*session*/, int /*sessionId*/, int /*signal*/) {}

void ptyTrampolineStatus(void * /*session*/, int /*sessionId*/, int status) {
    // 逆向：status 1=就绪, 2=会话结束。
    // 库内部的 exitCode 只在 hilog 里打印，不转发给回调，因此这里拿不到 shell 退出码。
    if (status == 2) {
        g_closed.store(true);
        g_outCv.notify_all();
    }
}

bool sessionClosed() { return g_closed.load(); }
int lastExitCode() { return g_exitCode.load(); }

void resetSessionState() {
    g_closed.store(false);
    g_exitCode.store(-1);
    std::lock_guard<std::mutex> lk(g_outMutex);
    g_output.clear();
}

void feedOutput(const char *data) {
    std::lock_guard<std::mutex> lk(g_outMutex);
    g_output += data;
}

bool waitForAnyOutput(int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::unique_lock<std::mutex> lk(g_outMutex);
    while (g_output.empty()) {
        if (g_closed.load()) return false;
        if (g_outCv.wait_until(lk, deadline) == std::cv_status::timeout) return !g_output.empty();
    }
    return true;
}

bool waitForOutput(const char *needle, int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::unique_lock<std::mutex> lk(g_outMutex);
    while (true) {
        if (g_output.find(needle) != std::string::npos) return true;
        if (g_closed.load()) return g_output.find(needle) != std::string::npos;
        if (g_outCv.wait_until(lk, deadline) == std::cv_status::timeout) {
            return g_output.find(needle) != std::string::npos;
        }
    }
}

// ------------------------------------------------------------------ PtySession
PtySession::~PtySession() { close(); }

std::string PtySession::selfTest() {
    if (!ensureLib()) return "dlopen=fail;error=" + g_loadError;
    std::string s = "dlopen=ok;";
    s += sym<int (*)(void **)>("OhGetPtyManager") ? "manager=ok;" : "manager=missing;";
    s += sym<int (*)(void *, void **, void *, void *, int *)>(
             "OhPtyManagerOpenPtySession")
             ? "open=ok;"
             : "open=missing;";
    s += sym<int (*)(void *, const char *)>("OhPtySessionSendData") ? "send=ok"
                                                                   : "send=missing";
    return s;
}

int PtySession::open(const PtyConfig &cfg, DataCallback onData) {
    if (!ensureLib()) {
        error_ = "dlopen 失败: " + g_loadError;
        return -1;
    }
    auto getManager = sym<int (*)(void **)>("OhGetPtyManager");
    auto openSession =
        sym<int (*)(void *, void **, void *, void *, int *)>("OhPtyManagerOpenPtySession");
    if (getManager == nullptr || openSession == nullptr) {
        error_ = "libfusion_pty_ndk.so 缺少必要符号（系统版本不匹配？）";
        return -2;
    }

    int rc = getManager(&manager_);
    if (rc != 0 || manager_ == nullptr) {
        error_ = "OhGetPtyManager 失败 (rc=" + std::to_string(rc) + ")";
        return rc != 0 ? rc : -3;
    }

    onData_ = std::move(onData);
    g_active = this;
    resetSessionState();

    RawConfig raw{};
    raw.f0 = cfg.shell.empty() ? nullptr : cfg.shell.c_str();
    raw.f1 = cfg.reserved1.empty() ? nullptr : cfg.reserved1.c_str();
    raw.f2 = cfg.reserved2.empty() ? nullptr : cfg.reserved2.c_str();
    raw.f3 = cfg.reserved3.empty() ? nullptr : cfg.reserved3.c_str();
    raw.f4 = cfg.distro.empty() ? nullptr : cfg.distro.c_str();
    raw.f5 = cfg.user.empty() ? nullptr : cfg.user.c_str();
    raw.rows = cfg.winSize.rows;
    raw.cols = cfg.winSize.cols;
    raw.xpixel = cfg.winSize.xpixel;
    raw.ypixel = cfg.winSize.ypixel;

    RawCallback cb{};
    cb.onRecv = reinterpret_cast<void *>(&ptyTrampolineRecv);
    cb.onSignal = reinterpret_cast<void *>(&ptyTrampolineSignal);
    cb.onStatus = reinterpret_cast<void *>(&ptyTrampolineStatus);

    int sid = -1;
    rc = openSession(manager_, &session_, &cb, &raw, &sid);
    if (rc != 0 || session_ == nullptr) {
        g_active = nullptr;
        error_ = "OpenPtySession 失败 (rc=" + std::to_string(rc) + ")";
        return rc != 0 ? rc : -4;
    }
    sessionId_ = sid;
    return 0;
}

int PtySession::send(const std::string &data) {
    if (session_ == nullptr) return -1;
    auto fn = sym<int (*)(void *, const char *)>("OhPtySessionSendData");
    if (fn == nullptr) return -2;
    return fn(session_, data.c_str());
}

int PtySession::setWinSize(const PtyWinSize &ws) {
    if (session_ == nullptr) return -1;
    auto fn = sym<int (*)(void *, void *)>("OhPtySessionSetWinSize");
    if (fn == nullptr) return -2;
    RawWinSize raw{ws.rows, ws.cols, ws.xpixel, ws.ypixel};
    return fn(session_, &raw);
}

void PtySession::close() {
    if (session_ != nullptr) {
        auto fn = sym<int (*)(void *)>("OhPtySessionClose");
        if (fn != nullptr) fn(session_);
        session_ = nullptr;
    }
    // 注意：不调用 OhDeletePtyManager、也不 dlclose。
    // 库内含单例与静态对象，卸载会导致进程退出阶段异常（与 vm_manager kit 同类问题）。
    manager_ = nullptr;
    if (g_active == this) g_active = nullptr;
}

}  // namespace hvm
