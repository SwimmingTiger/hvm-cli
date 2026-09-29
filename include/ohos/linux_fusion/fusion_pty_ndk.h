/*
 * fusion_pty_ndk.h —— 融合开发引擎 openEuler 终端通道（NDK 库）ABI 还原
 *
 * 对应库：/system/lib64/ndk/libfusion_pty_ndk.so
 *         依赖  ：libfusion_pty_manager_client.z.so / libfusion_pty_session_client.z.so
 *                 libfusion_pty_common.z.so / libutils.z.so / libhilog.so
 *
 * 来源：该库未随公开 SDK 提供头文件（IPCKit 里没有它），
 *       本文件由 idalib 反编译 + 设备实测还原：
 *         - 函数原型：导出符号表（干净、非 mangled 的 C 符号）
 *         - 结构布局：OhPtyManager::GetInnerConfig 反推 OhPtyConfig；
 *                     PtyWinSize::Marshalling 反推 4×uint32；
 *                     OhPtyManager::OpenPtySession 反推回调三指针顺序
 *         - 回调签名：InnerPtySessionCallback::On{RecvData,Signal,Status} 的转发代码
 *
 * 关键坑（都已实测踩过）：
 *   1) OhGetPtyManager 是「带出参」的：int OhGetPtyManager(OhPtyManager** out)。
 *      当成无返回值调用会立刻段错误。
 *   2) 会话刚 Open 成功时远端尚未就绪，此时 SendData 可能返回 0 但数据进不到 guest；
 *      可靠的就绪信号是「远端首次产生输出」。
 *   3) 库内部虽打了 exitCode 日志，但不会转发给用户回调 —— 远端 `exit N` 拿不到 N。
 *   4) 发送缓冲区必须以 '\0' 结尾（SendData 内部用 strlen）。
 */
#ifndef OHOS_LINUX_FUSION_FUSION_PTY_NDK_H
#define OHOS_LINUX_FUSION_FUSION_PTY_NDK_H

#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------- 返回码 */
/** 成功。 */
#define OH_PTY_OK 0
/** 6：入参为空（manager / callback / config / sessionId 任一为 NULL）。 */
#define OH_PTY_ERR_NULL_PARAM 6
/**
 * 13：会话尚未就绪时 SendData 的返回值。
 * 实测：OpenPtySession 返回 0 之后的一小段时间内发送会得到 13，
 * 且此时数据可能被丢弃 —— 应以「远端首次产生输出」作为就绪信号。
 */
#define OH_PTY_ERR_NOT_READY 13

#ifdef __cplusplus
extern "C" {
#endif

/** 不透明句柄。 */
typedef struct OhPtyManager OhPtyManager;
typedef struct OhPtySession OhPtySession;

/**
 * 终端窗口尺寸。
 * 还原自 OHOS::LinuxFusion::PtyWinSize::Marshalling —— 4 个 uint32 依次写入 Parcel。
 */
typedef struct OhPtyWinSize {
    uint32_t rows;    /**< 行 */
    uint32_t cols;    /**< 列 */
    uint32_t xpixel;  /**< 像素宽，常为 0 */
    uint32_t ypixel;  /**< 像素高，常为 0 */
} OhPtyWinSize;

/**
 * 会话配置（64 字节）。6 个字符串指针 + 内联窗口尺寸。
 * 还原自 OhPtyManager::GetInnerConfig：字段按顺序拷贝进内层
 * OHOS::LinuxFusion::PtyConfig 的 6 个 std::string，最后 16 字节拷贝进 PtyWinSize。
 * 传 NULL 表示使用库内默认值（"/bin/bash"、"openEuler"、"root"）。
 */
typedef struct OhPtyConfig {
    const char *shell;            /**< 默认 "/bin/bash" */
    const char *reserved1;        /**< 语义未知，NULL 用默认 */
    const char *reserved2;        /**< 语义未知，NULL 用默认 */
    const char *reserved3;        /**< 语义未知，NULL 用默认 */
    const char *distro;           /**< 默认 "openEuler" */
    const char *user;             /**< 默认 "root" */
    OhPtyWinSize winSize;         /**< 内联，非指针 */
} OhPtyConfig;

/**
 * 会话回调（3 个函数指针）。
 * 转发链：InnerPtySessionCallback::OnXxx → 用户回调，因此每个回调的第一个参数是
 * ptySession_（OhPtySession*），第二个是 sessionId。
 * 注意：data 是 C 字符串（无长度参数）；status 为 1=就绪、2=会话结束。
 */
typedef struct OhPtyCallback {
    void (*onRecvData)(OhPtySession *session, int sessionId, const char *data);
    void (*onSignal)(OhPtySession *session, int sessionId, int signal);
    void (*onStatus)(OhPtySession *session, int sessionId, int status);
} OhPtyCallback;

/* ---------------------------------------------------------------- 管理器 */

/** 取管理器单例。注意是出参形式，返回 0 表示成功。 */
int OhGetPtyManager(OhPtyManager **outManager);

/** 释放管理器。 */
int OhDeletePtyManager(OhPtyManager *manager);

/** 安装/更新 openEuler 镜像。 */
int OhPtyManagerInstallImage(OhPtyManager *manager);

/** 打开宿主与 openEuler 之间的共享目录（只有开启，没有关闭接口）。 */
int OhPtyManagerEnableShareFolder(OhPtyManager *manager);

/** 查询共享目录开关状态。 */
int OhPtyManagerGetSharedFolderToggleState(OhPtyManager *manager, bool *enabled);

/**
 * 打开一个 PTY 会话。
 * @param outSession  出参，会话句柄
 * @param callback    回调，不可为 NULL
 * @param config      配置，不可为 NULL
 * @param outSessionId 出参，会话 ID（实测从 3 开始递增）
 * @return 0 成功；6 表示参数为空
 */
int OhPtyManagerOpenPtySession(OhPtyManager *manager, OhPtySession **outSession,
                               OhPtyCallback *callback, OhPtyConfig *config,
                               int *outSessionId);

/* ---------------------------------------------------------------- 会话 */

/** 发送数据（内部 strlen，需以 '\0' 结尾）。 */
int OhPtySessionSendData(OhPtySession *session, const char *data);

/** 调整窗口尺寸。 */
int OhPtySessionSetWinSize(OhPtySession *session, OhPtyWinSize *winSize);

/** 取会话 ID。 */
int OhPtySessionGetSessionId(OhPtySession *session, int *sessionId);

/** 关闭会话。 */
int OhPtySessionClose(OhPtySession *session);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* OHOS_LINUX_FUSION_FUSION_PTY_NDK_H */
