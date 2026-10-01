/*
 * vm_manager_errcode.h —— 虚拟机管理 API 返回码
 *
 * 说明：华为未公开该服务的错误码枚举（库被 strip，日志里也只有数字），
 *       下列取值全部来自**设备实测**（每条都标注了触发条件）。
 *       命名是本仓库按语义归纳的，方便代码里判断，不代表华为内部命名。
 *
 * 用法：
 *     int rc = client.vmStatus(vm, st);
 *     if (rc == OHOS_VM_ERR_NO_VIRTUAL_MACHINE) { ... }
 *     printf("%s\n", ohos_vm_error_name(rc));
 */
#ifndef OHOS_VM_MANAGER_SERVICE_VM_MANAGER_ERRCODE_H
#define OHOS_VM_MANAGER_SERVICE_VM_MANAGER_ERRCODE_H

#include <cstdint>

/* ---------------------------------------------------------------- 成功 */
/** 调用成功。 */
#define OHOS_VM_OK 0

/* ------------------------------------------------- 服务端返回码（实测） */

/** 201：无虚拟机时调用 GetOpenEulerVersion。 */
#define OHOS_VM_ERR_NO_VIRTUAL_MACHINE 201

/**
 * 401：服务端拒绝虚拟机名（HandleStartVm/HandleCreateVm 校验长度与 ".."）。
 * 注意：实测中该码也会在其他校验失败时被回写，不宜单独作为"名字问题"的判据。
 */
#define OHOS_VM_ERR_INVALID_VM_NAME 401

/** 404：CfgInfo 为空，或从 Parcel 反序列化失败（HandleStartVm/HandleCreateVm）。 */
#define OHOS_VM_ERR_CFG_INFO_UNAVAILABLE 404

/** 405：无虚拟机时调用 GetVmIpv4Address。 */
#define OHOS_VM_ERR_VM_IP_UNAVAILABLE 405

/** 建虚拟机时前置校验未通过（实测：框架的当前虚拟机属于别的虚拟机程序且正在运行时得到此码）。
 *  来源：VmAssistantManager::CreateVm 里 CheckBeforeStartVm 的返回值。 */
#define OHOS_VM_ERR_CREATE_CHECK_FAILED (static_cast<std::int32_t>(0xFEFF000Au))
/** 启动虚拟机时前置校验未通过（实测：同上场景下 start 得到的是 0xFEFF0010）。
 *  来源：VmAssistantManager::StartVm 里 CheckBeforeStartVm 的返回值。 */
#define OHOS_VM_ERR_START_CHECK_FAILED (static_cast<std::int32_t>(0xFEFF0010u))

/**
 * 0xF8FF000C：无虚拟机时调用 GetSnapshotList。
 * 属 OHOS 统一错误码风格（高位段标识模块），此处按有符号 32 位取值。
 */
#define OHOS_VM_ERR_SNAPSHOT_UNAVAILABLE (static_cast<std::int32_t>(0xF8FF000Cu))

/* ------------------------------------------------- 客户端本地错误码（本仓库） */

/** -1001：客户端 kit 未加载（dlopen 失败）。 */
#define OHOS_VM_ERR_KIT_NOT_LOADED (-1001)

/** -1002：符号不存在 —— 通常意味着系统版本与逆向时不一致。 */
#define OHOS_VM_ERR_SYMBOL_MISSING (-1002)

namespace OHOS {
namespace VmManagerService {

/** 取返回码的可读名字；未知值返回 "unknown"。 */
inline const char *ErrorName(std::int32_t code) {
    switch (code) {
        case OHOS_VM_OK:
            return "OK";
        case OHOS_VM_ERR_NO_VIRTUAL_MACHINE:
            return "NO_VIRTUAL_MACHINE";
        case OHOS_VM_ERR_INVALID_VM_NAME:
            return "INVALID_VM_NAME";
        case OHOS_VM_ERR_CFG_INFO_UNAVAILABLE:
            return "CFG_INFO_UNAVAILABLE";
        case OHOS_VM_ERR_CREATE_CHECK_FAILED:
            // 实测：框架当前虚拟机属于别的虚拟机程序且正在运行时，create 得到这个码。
            return "VM_CREATE_CHECK_FAILED";
        case OHOS_VM_ERR_START_CHECK_FAILED:
            // 实测：同上场景下 start 得到这个码。
            return "VM_START_CHECK_FAILED";
        case OHOS_VM_ERR_VM_IP_UNAVAILABLE:
            return "VM_IP_UNAVAILABLE";
        case OHOS_VM_ERR_SNAPSHOT_UNAVAILABLE:
            return "SNAPSHOT_UNAVAILABLE";
        case OHOS_VM_ERR_KIT_NOT_LOADED:
            return "KIT_NOT_LOADED";
        case OHOS_VM_ERR_SYMBOL_MISSING:
            return "SYMBOL_MISSING";
        default:
            return "unknown";
    }
}

}  // namespace VmManagerService
}  // namespace OHOS

/** C 风格的便捷封装。 */

static inline const char *ohos_vm_error_name(std::int32_t code) {
    return OHOS::VmManagerService::ErrorName(code);
}


#endif /* OHOS_VM_MANAGER_SERVICE_VM_MANAGER_ERRCODE_H */
