// main.cpp —— hvm-cli 命令行入口
//
// 纯 C++ 实现：直接使用系统自带的 libvm_manager_kits.z.so，
// 不需要 root、不需要 HAP。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>   // getuid：用 uid/200000 推导 OS 账号 id
#include <cstring>
#include <string>
#include <vector>

#include "cfginfo.h"
#include "hvm_client.h"
#include "ohos/vm_manager_service/vm_manager_errcode.h"

namespace {

using hvm::Client;

bool g_json = false;

// ---------------------------------------------------------------- 输出工具
std::string jsonEscape(const std::string &s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char t[8];
                    snprintf(t, sizeof t, "\\u%04x", c);
                    o += t;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

//: 一条满足“键=值”形态的简单 JSON 构造器
class Json {
  public:
    explicit Json(std::string cmd) : cmd_(std::move(cmd)) {}
    Json &str(const std::string &k, const std::string &v) {
        body_ += sep() + "\"" + k + "\":\"" + jsonEscape(v) + "\"";
        return *this;
    }
    Json &num(const std::string &k, long long v) {
        char b[64];
        snprintf(b, sizeof b, "%lld", v);
        body_ += sep() + "\"" + k + "\":" + b;
        return *this;
    }
    Json &boolean(const std::string &k, bool v) {
        body_ += sep() + "\"" + k + "\":" + (v ? "true" : "false");
        return *this;
    }
    Json &raw(const std::string &k, const std::string &v) {
        body_ += sep() + "\"" + k + "\":" + v;
        return *this;
    }
    std::string ok() const {
        return "{\"ok\":true,\"cmd\":\"" + cmd_ + "\",\"data\":{" + body_ + "}}";
    }
    std::string fail(int rc, const std::string &msg) const {
        return "{\"ok\":false,\"cmd\":\"" + cmd_ + "\",\"rc\":" + std::to_string(rc) +
               ",\"error\":\"" + jsonEscape(msg) + "\"}";
    }

  private:
    std::string sep() { return body_.empty() ? "" : ","; }
    std::string cmd_;
    std::string body_;
};

int fail(const std::string &cmd, int rc, const std::string &msg) {
    if (g_json) {
        printf("{\"ok\":false,\"cmd\":\"%s\",\"rc\":%d,\"error\":\"%s\"}\n", cmd.c_str(), rc,
               jsonEscape(msg).c_str());
    } else {
        fprintf(stderr, "错误: %s (rc=%d)\n", msg.c_str(), rc);
    }
    return 1;
}

// ---------------------------------------------------------------- 帮助
void usage() {
    printf(
        "hvm-cli —— 鸿蒙 PC 虚拟机控制工具\n"
        "\n"
        "用法: hvm-cli [--json] [--vm <名字>] <命令> [参数...]\n"
        "\n"
        "运行环境:\n"
        "  必须在系统自带的 HiShell 终端中运行。\n"
        "  原因：只有 HiShell 终端在虚拟机白名单内 —— vm_manager 对每个请求校验\n"
        "  调用者身份，白名单为 HiShell HAP / LinuxFusionService(5005) /\n"
        "  hwf_service(7700) / openEuler HAP，其余能在其中开终端的应用会被\n"
        "  直接拒绝（permission denied）。\n"
        "\n"
        "全局选项:\n"
        "  --json          以 JSON 输出（便于脚本调用）\n"
        "  --vm <名字>     目标虚拟机名，默认 virtualized_linux\n"
        "\n"
        "状态:\n"
        "  selftest                客户端 kit 加载自检\n"
        "  info                    汇总状态（能力/活动 VM/版本/共享目录）\n"
        "  vms [名字...]           列出/探测虚拟机（无枚举接口，按名字探测）\n"
        "\n"
        "虚拟机生命周期（CfgInfo 为逆向手工构造，见 docs/api-notes.md）：\n"
        "  vm ctor   [选项]        仅构造 CfgInfo 并打印（验证用，不调服务）\n"
        "  vm create --name N --image P [选项] [--apply]\n"
        "  vm start  --name N [选项] [--apply]\n"
        "  vm view-state <0|1|2>              上报 HapViewState（应用用它告知视图状态）\n"
        "  vm displays <id[,id...]>           把显示器 id 列表交给服务端\n"
        "  vm serial-read  --name N [--chan C] [--type T] [--arg A]   读客户机通道\n"
        "  vm serial-write --name N --data TEXT [--chan C] [--type T] 写客户机通道\n"
        "  vm range                查询可用的 CPU / 内存范围（服务端校验依据）\n"
        "  vm import --name N --src SRC --dst DST [--apply] 让服务端拷贝文件（搬 ISO）\n"
        "  vm mount-cd   --name N --image X.iso [--apply]   挂载安装光盘\n"
        "  vm unmount-cd --name N --image X.iso [--apply]   卸载\n"
        "  vm destroy N            销毁虚拟机\n"
        "    选项: --cpu N --mem GB --disk MB | --disk-gb GB\n"
        "          --bios PATH --enhance PATH --start-type N --partition --dynamic-mem\n"
        "    单位（实测）：memorySize 为 GB（范围见 vm range），diskSize 为 MB 且 >= 65536\n"
        "          --start-type N --partition --dynamic-mem\n"
        "    注意: 不带 --apply 时为预演，不产生副作用\n"
        "  pause / resume [名字]   暂停 / 恢复虚拟机\n"
        "  lock-guest              锁定客户机（LockGuest）\n"
        "  lx-ota                  Linux 环境 OTA（LxOtaHandle）\n"
        "  lx-snapshot <名> <op>   Linux 虚拟机快照（HandleLxSnapshot）\n"
        "  rgm-status [名字]       查询 RGM 镜像状态（GetRgmImageStatusFromVm）\n"
        "  recover-user-data <路径>  恢复用户数据（RecoverUserData）\n"
        "  autopause <0|1|3|10|15|30>  自动暂停时间（0=关闭，其余为分钟）\n"
        "  net mode bridge|nat [接口]  网络模式（MODE_BRIDGE=0 / MODE_NAT=1）\n"
        "  net ports                  查询 NAT 端口转发表（GetPortForwardForNat）\n"
        "  net localhost-ports        查询本机转发表（GetLocalhostForwardFromVmToHost）\n"
        "  linux-data-delete       删除 Linux 数据镜像\n"
        "  rgm-image-delete <镜像> 删除 RGM 镜像（DeleteRgmImageFromVm）\n"
        "  share-volumes           列出全部共享卷（GetAllSharedVolume）\n"
        "  linux-path <宿主路径..> 宿主路径 → 客户机内路径（GetLinuxPathFromOhPath）\n"
        "  gallery-share on|off    宿主图库共享（SetHostGallerySharedEnabled）\n"
        "  guest-disk-share <路径> on|off  客户机磁盘共享\n"
        "  pasteboard [status|enable|disable|usable-enable|usable-disable|add A B|remove A]\n"
        "  buffer avail|low <字节> 内存缓冲上限\n"
        "  perf <a> <b> / perf-ex <a> on|off <b>  性能请求\n"
        "  screen-lock-task on|off 锁屏任务开关\n"
        "  tablet <int>            平板切换上报\n"
        "  vm-info                 活动虚拟机的 DDR 大小与进程 PID\n"
        "  stratovirt-mem          stratoVirt 占用内存\n"
        "  host-sn                 宿主 SN\n"
        "  capability              本机是否支持虚拟化\n"
        "  active-name             活动虚拟机名\n"
        "  active-status           活动虚拟机状态码\n"
        "  vm-status [名字]        指定虚拟机状态码\n"
        "  process-exist <进程名>  进程是否存在\n"
        "  feature <ID>            特性是否支持\n"
        "  open-euler-version      openEuler 镜像版本\n"
        "  hash-name               Hash 名\n"
        "  quick-start             是否快速启动场景\n"
        "  is-installing           是否安装中\n"
        "\n"
        "电源:\n"
        "  force-stop              强制关机\n"
        "  quit-by-reboot-host     宿主机重启导致退出\n"
        "  require-big-mem         申请大内存\n"
        "\n"
        "快照:\n"
        "  snapshot list\n"
        "  snapshot create <名字>\n"
        "  snapshot restore <名字>\n"
        "  snapshot destroy <名字>\n"
        "  snapshot rename <旧名> <新名>\n"
        "\n"
        "共享目录:\n"
        "  share list | share enable | share disable\n"
        "  share add <宿主路径> <客机路径> | share remove <宿主路径> | share setup\n"
        "\n"
        "网络:\n"
        "  net ip | net proxy | net share-on | net share-off | net dns-on | net dns-off\n"
        "\n"
        "磁盘:\n"
        "  disk capacity | disk path | disk size | disk expand <GB> | disk delete-data\n"
        "\n"
        "显示/内存:\n"
        "  resolution <宽> <高> [--full]\n"
        "  touch-mem <MB>\n"
        "  swap-2d <MB>\n");
}

// ---------------------------------------------------------------- 主逻辑
struct Args {
    std::vector<std::string> pos;
    std::string vm = hvm::kLinuxVm;
    bool full = false;
};

Args parse(int argc, char **argv, int from) {
    Args a;
    for (int i = from; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--json") {
            g_json = true;
        } else if (s == "--vm" && i + 1 < argc) {
            a.vm = argv[++i];
        } else if (s == "--full") {
            a.full = true;
        } else {
            a.pos.push_back(s);
        }
    }
    return a;
}

//: 计算字符串的终端显示宽度（CJK 记 2 列）
int displayWidth(const std::string &s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        unsigned int cp = c;
        if (c >= 0xF0) { len = 4; cp = c & 0x07; }
        else if (c >= 0xE0) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xC0) { len = 2; cp = c & 0x1F; }
        for (size_t k = 1; k < len && i + k < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        w += (cp >= 0x1100) ? 2 : 1;  // CJK 及全角字符按 2 列
        i += len;
    }
    return w;
}

//: 按显示宽度右侧补空格（用于表格列）
std::string padTo(const std::string &s, int width) {
    std::string o = s;
    for (int i = displayWidth(s); i < width; ++i) o += ' ';
    return o;
}

//: 按显示宽度补空格后打印 "标签 : 值"
void printRow(const std::string &label, const std::string &value, int width = 14) {
    printf("%s", label.c_str());
    int pad = width - displayWidth(label);
    for (int i = 0; i < pad; ++i) putchar(' ');
    printf(" : %s\n", value.c_str());
}

int cmdInfo(Client &c) {
    auto vi = c.info();
    if (g_json) {
        Json j("info");
        j.boolean("capable", vi.capable)
            .str("activeVm", vi.activeVm)
            .num("activeStatus", vi.activeStatus)
            .str("openEulerVersion", vi.openEulerVersion)
            .boolean("sharedFolderEnabled", vi.sharedFolderEnabled)
            .num("rcCapability", vi.rcCapability)
            .num("rcActiveName", vi.rcActiveName)
            .num("rcActiveStatus", vi.rcActiveStatus)
            .num("rcOpenEulerVersion", vi.rcOpenEulerVersion)
            .num("rcSharedFolderEnabled", vi.rcSharedFolderEnabled);
        printf("%s\n", j.ok().c_str());
        return 0;
    }
    printRow("虚拟化能力", vi.capable ? "支持" : "不支持");
    printRow("活动虚拟机", vi.activeVm.empty() ? "(无)" : vi.activeVm);
    printRow("状态码", std::to_string(vi.activeStatus) + " (" +
                          hvm::statusName(vi.activeStatus) + ")");
    printRow("openEuler 版本",
             vi.openEulerVersion.empty() ? "(未知)" : vi.openEulerVersion);
    printRow("共享目录开关", vi.sharedFolderEnabled ? "开" : "关");
    return 0;
}

std::string humanBytes(int64_t bytes) {
    char b[64];
    const double kb = 1024.0, mb = kb * 1024, gb = mb * 1024;
    if (bytes >= static_cast<int64_t>(gb)) {
        snprintf(b, sizeof b, "%.2f GB", bytes / gb);
    } else if (bytes >= static_cast<int64_t>(mb)) {
        snprintf(b, sizeof b, "%.2f MB", bytes / mb);
    } else {
        snprintf(b, sizeof b, "%lld B", static_cast<long long>(bytes));
    }
    return b;
}

#define NEED_ARGS(n, usage_hint)                                                     \
    do {                                                                             \
        if (a.pos.size() < (n)) {                                                    \
            fprintf(stderr, "参数不足，用法: %s\n", usage_hint);                     \
            return 2;                                                                \
        }                                                                            \
    } while (0)

// ---------------------------------------------------------------- 路径转换
//: 把"用户视图"路径转成"媒体库视图"路径 —— 后者是 vm_manager 与 stratovirt
//: **两个域都能读**的形式，也正是 HAP 传给服务端的写法：
//:
//:   /storage/Users/currentUser/Download/x.iso
//:      → /storage/media/100/local/files/Docs/Download/x.iso
//:   file://docs/storage/Users/currentUser/Download/x.iso  （同上）
//:
//: 依据：实测正在安装 Windows 的虚拟机命令行里，光盘就是
//:   file=/storage/media/100/local/files/Docs/Download/<bundle>/Win11_....iso
//: 而 /storage/Users/... 与 hmdfs 真实路径都只对 vm_manager 可读、stratovirt 读不了。
//: 取当前进程所属的 OS 账号 id（= 媒体库视图里的那个数字）。
//: OpenHarmony 的 uid 编码规则是 uid = userId * 200000 + appId，所以直接整除即可：
//:   本机 HiShell 的 uid 20020085 → 100；第二个账号下的应用会是 101xxxxx → 101。
//: 也可以用 HVM_USER_ID 显式覆盖（例如要在别的账号视图下取文件时）。
std::string currentUserId() {
    if (const char *env = getenv("HVM_USER_ID")) {
        if (*env != '\0') return env;
    }
    const uid_t uid = getuid();
    const std::string derived = std::to_string(static_cast<unsigned>(uid) / 200000u);
    if (derived == "0") {
        // 理论上不该出现（系统进程 uid 很小）；回落到第一个账号
        return "100";
    }
    return derived;
}

std::string toServicePath(const std::string &in) {
    static const char *kUserPrefix = "file://docs/storage/Users/currentUser/";
    static const char *kPlainPrefix = "/storage/Users/currentUser/";
    static const char *kMediaPrefix = "/storage/media/";

    std::string rest;
    if (in.rfind(kUserPrefix, 0) == 0) {
        rest = in.substr(strlen(kUserPrefix));
    } else if (in.rfind(kPlainPrefix, 0) == 0) {
        rest = in.substr(strlen(kPlainPrefix));
    } else {
        return in;  // 已是服务端路径（/storage/media/... 或 /system/... 等）
    }
    return std::string(kMediaPrefix) + currentUserId() + "/local/files/Docs/" + rest;
}

// ---------------------------------------------------------------- LinuxFusion / RGM 运维
//: 这一组都是 kit 接口直通：签名已由 include/ohos/vm_manager_service/vm_manager_kits.h
//: 给出（编译器生成的符号名），不需要构造任何私有类。
static int cmdFusion(Client &c, const std::string &cmd, const Args &a) {
    const std::string &vm = a.vm;
    auto onoff = [](const std::string &v) {
        return v == "on" || v == "1" || v == "true" || v == "enable";
    };
    auto arg = [&](std::size_t i) { return i < a.pos.size() ? a.pos[i] : std::string(); };
    auto report = [&](int rc, const char *what) {
        if (rc != 0) return fail(cmd, rc, std::string(what) + " 失败");
        if (g_json) {
            Json j(cmd);
            j.num("rc", rc);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%s 完成\n", what);
        }
        return 0;
    };

    if (cmd == "pause") {
        int rc = c.pauseVm();
        if (rc != 0) return fail(cmd, rc, "PauseVm 失败");
        printf("已请求暂停活动虚拟机\n");
        return 0;
    }
    if (cmd == "resume") {
        std::string name = a.pos.empty() ? vm : a.pos[0];
        int rc = c.resumeVm(name);
        if (rc != 0) return fail(cmd, rc, "ResumeVm 失败");
        printf("已请求恢复 %s\n", name.c_str());
        return 0;
    }
    if (cmd == "lock-guest") return report(c.lockGuest(), "锁定客户机");
    if (cmd == "lx-snapshot") {
        if (a.pos.size() < 2) {
            fprintf(stderr, "lx-snapshot <快照名> <操作码>\n");
            return 2;
        }
        return report(c.handleLxSnapshot(vm, a.pos[0], atoi(a.pos[1].c_str())),
                      "处理 Linux 虚拟机快照");
    }
    if (cmd == "rgm-status") {
        std::string name = a.pos.empty() ? vm : a.pos[0];
        int st = c.rgmImageStatusFromVm(name);
        if (st == OHOS_VM_ERR_SYMBOL_MISSING) return fail(cmd, st, "GetRgmImageStatusFromVm 失败");
        if (g_json) {
            Json j(cmd);
            j.str("image", name).num("status", st);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%s 的 RGM 镜像状态: %d\n", name.c_str(), st);
        }
        return 0;
    }
    if (cmd == "recover-user-data") {
        if (a.pos.empty()) {
            fprintf(stderr, "recover-user-data <路径>\n");
            return 2;
        }
        return report(c.recoverUserData(vm, a.pos[0]), "恢复用户数据");
    }
    if (cmd == "autopause") {
        if (a.pos.empty()) {
            fprintf(stderr, "autopause <0|1|3|10|15|30>   （0=关闭，其余为分钟数）\n");
            return 2;
        }
        int32_t m = static_cast<int32_t>(atoi(a.pos[0].c_str()));
        return report(c.setAutoPauseTime(vm, m), "设置自动暂停时间");
    }
    if (cmd == "lx-ota") return report(c.lxOtaHandle(), "Linux 环境 OTA");
    if (cmd == "linux-data-delete") return report(c.deleteLinuxDataImage(), "删除 Linux 数据镜像");
    if (cmd == "rgm-image-delete") {
        if (a.pos.empty()) {
            fprintf(stderr, "rgm-image-delete 需要 <镜像名>\n");
            return 2;
        }
        return report(c.deleteRgmImageFromVm(a.pos[0]), "删除 RGM 镜像");
    }
    if (cmd == "share-volumes") {
        // 故意不调用：GetAllSharedVolume() 的参数形状与 GetSharedFolder() 相同（无参），
        // 差别在返回类型 —— 实测按 vector<string> 解释会段错误，说明元素是私有类。
        // 需要先在 [N]/[S] 里还原该元素类之后才能接。
        if (g_json) {
            Json j(cmd);
            j.boolean("supported", false).str("reason", "GetAllSharedVolume 的返回元素类型未还原");
            printf("%s\n", j.ok().c_str());
        } else {
            printf("暂不支持：GetAllSharedVolume() 的返回元素是私有类（按 vector<string> 解释会崩溃）。\n"
                   "需要用同样的方法还原该元素类型的布局后才能实现。\n");
        }
        return 3;
    }
    if (cmd == "linux-path") {
        if (a.pos.empty()) {
            fprintf(stderr, "linux-path 需要至少一个宿主路径\n");
            return 2;
        }
        std::vector<std::string> out;
        int rc = c.linuxPathFromOhPath(a.pos, out);
        if (rc != 0) {
            // 服务端日志: CheckCallingProcNameFromLinuxFusionService:780 GetNativeTokenInfo failed
            //             GetUpdatedSharedPath:3065 GetSharedFolder failed
            return fail(cmd, rc,
                        "GetLinuxPathFromOhPath 失败（该接口只允许 LinuxFusion 服务自己调用，"
                        "HiShell 身份也会被拒）");
        }
        for (std::size_t i = 0; i < a.pos.size(); ++i) {
            printf("%s  →  %s\n", a.pos[i].c_str(), i < out.size() ? out[i].c_str() : "(无)");
        }
        return 0;
    }
    if (cmd == "gallery-share") {
        if (a.pos.empty()) {
            fprintf(stderr, "gallery-share on|off\n");
            return 2;
        }
        return report(c.setHostGalleryShared(vm, onoff(a.pos[0])), "设置图库共享");
    }
    if (cmd == "guest-disk-share") {
        if (a.pos.size() < 2) {
            fprintf(stderr, "guest-disk-share <路径> on|off\n");
            return 2;
        }
        return report(c.setGuestDiskShared(vm, a.pos[0], onoff(a.pos[1])), "设置客户机磁盘共享");
    }
    if (cmd == "pasteboard") {
        const std::string sub = arg(0);
        if (sub.empty() || sub == "status") {
            bool en = false, usable = false;
            int rc1 = c.pasteboardEnableState(en);
            int rc2 = c.pasteboardUsableState(usable);
            if (rc1 != 0 && rc2 != 0) return fail(cmd, rc1, "查询剪贴板状态失败");
            if (g_json) {
                Json j(cmd);
                j.boolean("enabled", en).boolean("usable", usable);
                printf("%s\n", j.ok().c_str());
            } else {
                printf("剪贴板共享: %s\n可用状态  : %s\n", en ? "开" : "关", usable ? "可用" : "不可用");
            }
            return 0;
        }
        if (sub == "enable") return report(c.setPasteboardEnableState(true), "开启剪贴板共享");
        if (sub == "disable") return report(c.setPasteboardEnableState(false), "关闭剪贴板共享");
        if (sub == "usable-enable") return report(c.setPasteboardUsableState(true), "置剪贴板为可用");
        if (sub == "usable-disable") return report(c.setPasteboardUsableState(false), "置剪贴板为不可用");
        if (sub == "add") {
            if (a.pos.size() < 3) {
                fprintf(stderr, "pasteboard add <宿主目录> <客户机目录>\n");
                return 2;
            }
            return report(c.addPasteboardSharedFolder(a.pos[1], a.pos[2]), "添加共享目录");
        }
        if (sub == "remove") {
            if (a.pos.size() < 2) {
                fprintf(stderr, "pasteboard remove <宿主目录>\n");
                return 2;
            }
            return report(c.removePasteboardSharedFolder(a.pos[1]), "移除共享目录");
        }
        fprintf(stderr, "未知的 pasteboard 子命令: %s\n", sub.c_str());
        return 2;
    }
    if (cmd == "buffer") {
        if (a.pos.size() < 2) {
            fprintf(stderr, "buffer avail|low <字节数>\n");
            return 2;
        }
        const uint64_t v = strtoull(a.pos[1].c_str(), nullptr, 10);
        if (a.pos[0] == "avail") return report(c.sysAvailBufferLimit(v), "设置可用缓冲上限");
        if (a.pos[0] == "low") return report(c.sysLowBufferLimit(v), "设置低内存缓冲上限");
        fprintf(stderr, "未知的 buffer 子命令: %s\n", a.pos[0].c_str());
        return 2;
    }
    if (cmd == "perf") {
        if (a.pos.size() < 2) {
            fprintf(stderr, "perf <参数一> <参数二>\n");
            return 2;
        }
        return report(c.vmUniSocPerfRequest(a.pos[0], a.pos[1]), "性能请求");
    }
    if (cmd == "perf-ex") {
        if (a.pos.size() < 3) {
            fprintf(stderr, "perf-ex <参数一> on|off <参数二>\n");
            return 2;
        }
        return report(c.vmUniSocPerfRequestEx(a.pos[0], onoff(a.pos[1]), a.pos[2]), "性能请求(Ex)");
    }
    if (cmd == "screen-lock-task") {
        if (a.pos.empty()) {
            fprintf(stderr, "screen-lock-task on|off\n");
            return 2;
        }
        return report(c.toggleScreenLockTask(onoff(a.pos[0])), "切换锁屏任务");
    }
    if (cmd == "tablet") {
        if (a.pos.empty()) {
            fprintf(stderr, "tablet <int>\n");
            return 2;
        }
        return report(c.tabletSwitchChanged(atoi(a.pos[0].c_str())), "上报平板切换");
    }
    return 2;
}

// ---------------------------------------------------------------- vm 子命令
//: CfgInfo 是华为私有类型（无公开头文件），这里按逆向配方手工构造。
int cmdVm(Client &c, const std::vector<std::string> &pos) {
    if (pos.empty()) {
        fprintf(stderr, "用法: hvm-cli vm ctor|create|start|destroy [选项]\n");
        return 2;
    }
    const std::string act = pos[0];
    std::string name, image, bios, enhance;
    int cpu = 0, mem = 0, disk = 0, startType = -1;
    bool partition = false, dynMem = false, apply = false;
    bool keepSnapshots = false, forceImport = false;
    std::string password;
    std::string chanName = "winbox_serial0";
    unsigned chanType = 0;
    int chanArg = 0;
    std::string dataArg;

    for (std::size_t i = 1; i < pos.size(); ++i) {
        const std::string &k = pos[i];
        if (k == "--apply") { apply = true; continue; }
        if (k == "--keep-snapshots") { keepSnapshots = true; continue; }
        if (k == "--force-import") { forceImport = true; continue; }
        if (k == "--partition") { partition = true; continue; }
        if (k == "--dynamic-mem") { dynMem = true; continue; }
        if (k.size() != 0 && k[0] != '-') { name = k; continue; }
        if (i + 1 >= pos.size()) { fprintf(stderr, "%s 缺少取值\n", k.c_str()); return 2; }
        const std::string v = pos[++i];
        if (k == "--name") name = v;
        else if (k == "--image") image = v;
        else if (k == "--bios" || k == "--src") bios = v;
        else if (k == "--enhance" || k == "--dst") enhance = v;
        else if (k == "--cpu") cpu = atoi(v.c_str());
        else if (k == "--mem") mem = atoi(v.c_str());
        else if (k == "--disk") disk = atoi(v.c_str());          // MB
        else if (k == "--disk-gb") disk = atoi(v.c_str()) * 1024; // 便捷：按 GB 输入
        else if (k == "--start-type") startType = atoi(v.c_str());
        else if (k == "--password") password = v;
        else if (k == "--chan") chanName = v;
        else if (k == "--type") chanType = static_cast<unsigned>(atoi(v.c_str()));
        else if (k == "--arg") chanArg = atoi(v.c_str());
        else if (k == "--data") dataArg = v;
        else { fprintf(stderr, "未知选项: %s\n", k.c_str()); return 2; }
    }

    // 服务端会按可用范围校验；先查出来，给用户明确提示
    uint32_t cmin = 0, cmax = 0, mmin = 0, mmax = 0;
    const bool haveCpuRange = c.availableCpuRange(cmin, cmax) == 0;
    const bool haveMemRange = c.availableMemoryRange(mmin, mmax) == 0;
    if (act == "create" || act == "start") {
        if (haveCpuRange && cpu != 0 && (static_cast<uint32_t>(cpu) < cmin || static_cast<uint32_t>(cpu) > cmax)) {
            fprintf(stderr, "CPU 数 %d 超出可用范围 %u..%u\n", cpu, cmin, cmax);
            return 2;
        }
        if (haveMemRange && mem != 0 && (static_cast<uint32_t>(mem) < mmin || static_cast<uint32_t>(mem) > mmax)) {
            fprintf(stderr, "内存 %d GB 超出可用范围 %u..%u GB\n", mem, mmin, mmax);
            return 2;
        }
    }

    // 用户视图路径 → 媒体库视图（服务端与 stratovirt 都读得到）
    if (!bios.empty()) {
        std::string t = toServicePath(bios);
        if (t != bios) {
            if (!g_json)
                printf("（路径转换）%s\n           → %s\n           （账号 id=%s，由 uid %u / 200000 推导）\n",
                       bios.c_str(), t.c_str(), currentUserId().c_str(),
                       static_cast<unsigned>(getuid()));
            bios = t;
        }
    }
    if (!enhance.empty()) enhance = toServicePath(enhance);
    image = toServicePath(image);

    hvm::CfgInfoBuilder cfg;
    if (!cfg.ok()) return fail("vm " + act, -1, "构造 CfgInfo 失败: " + cfg.lastError());
    cfg.setCpuNum(cpu);
    cfg.setMemorySizeMb(mem);
    cfg.setDiskSizeGb(disk);
    cfg.setDiskPartition(partition);
    cfg.setDynamicMemory(dynMem);
    if (!bios.empty()) cfg.setBiosPath(bios);
    if (!enhance.empty()) cfg.setEnhanceFilePath(enhance);
    cfg.setStartType(startType);

    if (act == "view-state") {
        // 上报 HapViewState（应用用它告诉服务端视图状态）。取值实测见服务端分支：
        // 0/1/2 三种分支，语义未完全确定，先按 0..2 试。
        int st = 1;
        if (!image.empty()) st = atoi(image.c_str());
        int rc = c.progressDiedState(st);
        if (rc != 0)
            return fail("vm view-state", rc,
                        std::string("ProgressDiedStateToVm 返回: ") + ohos_vm_error_name(rc));
        printf("已上报 viewState=%d\n", st);
        return 0;
    }

    if (act == "displays") {
        // 交给服务端的显示器 id 列表；用法: vm displays <id>[,<id>...]
        std::vector<uint64_t> ids;
        std::string list = image.empty() ? "0" : image;
        for (std::size_t i = 0; i <= list.size();) {
            std::size_t j = list.find(',', i);
            if (j == std::string::npos) j = list.size();
            if (j > i) ids.push_back(strtoull(list.substr(i, j - i).c_str(), nullptr, 10));
            i = j + 1;
        }
        int rc = c.displaysNumber(ids);
        if (rc != 0)
            return fail("vm displays", rc,
                        std::string("DisplaysNumber 返回: ") + ohos_vm_error_name(rc));
        printf("已上报 %zu 个显示 id:", ids.size());
        for (auto v : ids) printf(" %llu", static_cast<unsigned long long>(v));
        printf("\n");
        return 0;
    }

    if (act == "serial-read" || act == "serial-write") {
        // 主机↔客户机通道（virtio-serial）。默认通道名取自虚拟机命令行里的
        // winbox_serial0（nr=1，对应 uds/serial0.sock）
        if (name.empty()) {
            fprintf(stderr, "%s 需要 --name <虚拟机名>\n", act.c_str());
            return 2;
        }
        hvm::ChannelInfoBuilder ch(chanType, chanName);
        if (!ch.ok()) return fail("vm " + act, -1, "构造 ChannelInfo 失败: " + ch.lastError());
        if (!g_json) printf("%s\n", ch.dump().c_str());
        if (act == "serial-write") {
            if (dataArg.empty()) {
                fprintf(stderr, "serial-write 需要 --data <文本>\n");
                return 2;
            }
            std::vector<uint8_t> buf(dataArg.begin(), dataArg.end());
            int rc = c.sendDataToVm(name, buf, chanArg, ch.raw());
            if (rc != 0)
                return fail("vm serial-write", rc,
                            std::string("SendDataToVm 返回: ") + ohos_vm_error_name(rc));
            printf("已发送 %zu 字节\n", buf.size());
            return 0;
        }
        // 服务端要求缓冲区非空（RecvDataFromVm:1048 "data size invalid."），
        // 且返回值里会带实际读到的长度
        const std::size_t want = (chanArg > 0) ? static_cast<std::size_t>(chanArg) : 4096;
        std::vector<uint8_t> buf(want, 0);
        int rc = c.recvDataFromVm(name, buf, chanArg, ch.raw());
        if (rc != 0)
            return fail("vm serial-read", rc,
                        std::string("RecvDataFromVm 返回: ") + ohos_vm_error_name(rc));
        printf("收到 %zu 字节:\n", buf.size());
        // 先按可打印文本显示，再给一份 hex
        std::string text(buf.begin(), buf.end());
        fwrite(text.data(), 1, text.size(), stdout);
        if (!text.empty() && text.back() != '\n') fputc('\n', stdout);
        return 0;
    }

    if (act == "export") {
        // 把虚拟机磁盘导出到用户可访问的位置（调查服务端期望的 qcow2 格式）
        if (name.empty() || bios.empty() || enhance.empty()) {
            fprintf(stderr, "用法: hvm-cli vm export --name N --src <服务端磁盘路径> --dst <用户区路径> [--apply]\n");
            return 2;
        }
        hvm::MigrationOptionsBuilder opts;
        if (!opts.ok()) return fail("vm export", -1, "构造 MigrationOptions 失败: " + opts.lastError());
        if (!apply) {
            printf("（预演）将调用 ExportVmDiskImage\n  虚拟机: %s\n  src : %s\n  dst : %s\n",
                   name.c_str(), bios.c_str(), enhance.c_str());
            return 0;
        }
        int rc = c.exportVmDiskImage(name, bios, enhance, false, opts.raw());
        if (rc != 0)
            return fail("vm export", rc,
                        std::string("ExportVmDiskImage 返回: ") + ohos_vm_error_name(rc) + " (" +
                            std::to_string(rc) + ")");
        printf("已提交导出：%s -> %s\n", bios.c_str(), enhance.c_str());
        return 0;
    }

    if (act == "import") {
        // 让服务端把文件拷到指定位置（用于把 ISO 搬进 stratovirt 能读的服务数据区）
        if (name.empty() || bios.empty() || enhance.empty()) {
            fprintf(stderr,
                    "用法: hvm-cli vm import --name <vm> --src <源路径> --dst <目标路径> [--apply]\n"
                    "      （--src 与 --bios 同义，--dst 与 --enhance 同义）\n");
            return 2;
        }
        // MigrationOptions：服务端要求非空，字段布局见 include/.../cfg_info.h
        hvm::MigrationOptionsBuilder opts;
        if (!opts.ok()) return fail("vm import", -1, "构造 MigrationOptions 失败: " + opts.lastError());
        opts.setKeepSnapshots(keepSnapshots);
        opts.setForceImport(forceImport);
        if (!password.empty()) opts.setPassword(password);
        if (!apply) {
            printf("（预演）将调用 ImportVmDiskImage\n  虚拟机: %s\n  src : %s\n  dst : %s\n",
                   name.c_str(), bios.c_str(), enhance.c_str());
            fputs(opts.dump().c_str(), stdout);
            printf("\n真正执行请加 --apply\n");
            return 0;
        }
        int rc = c.importVmDiskImage(name, bios, enhance, opts.raw());
        if (rc != 0)
            return fail("vm import", rc,
                        std::string("ImportVmDiskImage 返回: ") + ohos_vm_error_name(rc) + " (" +
                            std::to_string(rc) + ")");
        printf("已提交拷贝：%s -> %s\n", bios.c_str(), enhance.c_str());
        return 0;
    }

    if (act == "mount-cd" || act == "unmount-cd") {
        // 挂载/卸载安装光盘（ISO 路径同样要用服务端视角的真实路径）
        if (name.empty() || image.empty()) {
            fprintf(stderr, "%s 需要 --name <虚拟机名> 与 --image <ISO 路径>\n", act.c_str());
            return 2;
        }
        std::string detail;
        int rc = (act == "mount-cd") ? c.mountCdDrive(name, image, true, detail)
                                     : c.unmountCdDrive(name, image);
        if (rc != 0)
            return fail("vm " + act, rc,
                        std::string("返回: ") + ohos_vm_error_name(rc) + " (" +
                            std::to_string(rc) + ")");
        if (g_json) {
            Json j("vm " + act);
            j.str("name", name).str("path", image).str("detail", detail);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("已%s: %s\n", act == "mount-cd" ? "挂载" : "卸载", image.c_str());
            if (!detail.empty()) printf("服务端返回: %s\n", detail.c_str());
        }
        return 0;
    }

    if (act == "range") {
        uint32_t cmin = 0, cmax = 0, mmin = 0, mmax = 0;
        int rcCpu = c.availableCpuRange(cmin, cmax);
        int rcMem = c.availableMemoryRange(mmin, mmax);
        if (g_json) {
            Json j("vm range");
            j.num("cpuMin", cmin).num("cpuMax", cmax).num("rcCpu", rcCpu)
             .num("memMin", mmin).num("memMax", mmax).num("rcMem", rcMem);
            printf("%s\n", j.ok().c_str());
        } else {
            printRow("CPU 数范围", std::to_string(cmin) + " .. " + std::to_string(cmax) +
                                     (rcCpu ? "  (rc=" + std::to_string(rcCpu) + ")" : ""));
            printRow("内存范围", std::to_string(mmin) + " .. " + std::to_string(mmax) +
                                    (rcMem ? "  (rc=" + std::to_string(rcMem) + ")" : ""));
        }
        return 0;
    }

    if (act == "ctor") {
        // 只验证构造配方，不碰服务端
        if (g_json) {
            Json j("vm ctor");
            j.str("dump", cfg.dump()).raw("rawPtr", "null");
            printf("%s\n", j.ok().c_str());
        } else {
            fputs(cfg.dump().c_str(), stdout);
            fputc('\n', stdout);
        }
        return 0;
    }

    if (act == "destroy") {
        if (name.empty()) { fprintf(stderr, "destroy 需要虚拟机名\n"); return 2; }
        int rc = c.destroyVm(name);
        if (rc != 0)
            return fail("vm destroy", rc,
                        std::string("DestroyVm 失败: ") + ohos_vm_error_name(rc));
        if (g_json) { Json j("vm destroy"); j.str("name", name); printf("%s\n", j.ok().c_str()); }
        else printf("已销毁 %s\n", name.c_str());
        return 0;
    }

    if (act == "create" || act == "start") {
        if (name.empty()) { fprintf(stderr, "%s 需要 --name <虚拟机名>\n", act.c_str()); return 2; }
        if (act == "create" && image.empty()) {
            fprintf(stderr, "create 需要 --image <镜像路径>（服务端 CreateVm 的第 2 个字符串）\n");
            return 2;
        }
        if (!apply) {
            printf("（预演）将调用 %s\n", act == "create" ? "CreateVm" : "StartVm");
            printf("  虚拟机名   : %s\n", name.c_str());
            if (haveCpuRange) printf("  可用 CPU   : %u..%u\n", cmin, cmax);
            if (haveMemRange) printf("  可用内存   : %u..%u GB\n", mmin, mmax);
            if (act == "create") printf("  镜像路径   : %s\n", image.c_str());
            fputs(cfg.dump().c_str(), stdout);
            printf("\n以上为将要发送的内容；真正执行请加 --apply\n");
            return 0;
        }
        int rc = (act == "create") ? c.createVm(name, image, cfg.raw())
                                   : c.startVm(name, cfg.raw());
        if (rc != 0 && rc != 1)  // 1 也可能表示“已启动”之类，先按错误码如实报
            return fail("vm " + act, rc,
                        std::string(act == "create" ? "CreateVm" : "StartVm") + " 返回: " +
                            ohos_vm_error_name(rc) + " (" + std::to_string(rc) + ")");
        if (g_json) {
            Json j("vm " + act);
            j.str("name", name).num("rc", rc);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%s 返回 rc=%d (%s)\n", act == "create" ? "CreateVm" : "StartVm", rc,
                   ohos_vm_error_name(rc));
        }
        return 0;
    }

    fprintf(stderr, "未知 vm 动作: %s\n", act.c_str());
    return 2;
}

int run(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    std::string cmd = argv[1];
    if (cmd == "help" || cmd == "-h" || cmd == "--help") {
        usage();
        return 0;
    }

    // argv[1] 是选项时要从它本身开始解析（否则会吞掉它的取值，例如 --vm 的名字）
    const bool firstIsOpt = cmd.rfind("--", 0) == 0;
    Args a = parse(argc, argv, firstIsOpt ? 1 : 2);
    // 允许 `hvm-cli --json info` / `hvm-cli --vm win11 net ip` 这种写法：
    // 把选项后的第一条非选项当作命令
    if (firstIsOpt) {
        g_json = g_json || cmd == "--json";
        if (a.pos.empty()) {
            usage();
            return 2;
        }
        cmd = a.pos.front();
        a.pos.erase(a.pos.begin());
    }

    Client c;
    if (!c.ready() && cmd != "selftest") {
        return fail(cmd, -1,
                    std::string("客户端 kit 不可用: ") + c.lastError() +
                        "（请在系统自带 HiShell 终端内运行）");
    }

    if (cmd == "selftest") {
        std::string st = c.selfTest();
        if (g_json) {
            Json j("selftest");
            j.str("kit", st);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%s\n", st.c_str());
        }
        return 0;
    }

    if (cmd == "vms") {
        // vm_manager 没有提供枚举接口：以 GetActiveVmName 为准，
        // 再对已知/指定的名字逐个探测状态与磁盘镜像。
        std::vector<std::string> names =
            a.pos.empty() ? std::vector<std::string>{a.vm} : a.pos;
        std::string active;
        c.activeVmName(active);
        if (g_json) {
            std::string arr = "[";
            for (size_t i = 0; i < names.size(); ++i) {
                int st = 0;
                int rc = c.vmStatus(names[i], st);
                std::string path;
                c.diskImagePath(names[i], path);
                if (i) arr += ",";
                arr += "{\"name\":\"" + jsonEscape(names[i]) +
                       "\",\"rc\":" + std::to_string(rc) +
                       ",\"status\":" + std::to_string(st) +
                       ",\"active\":" + ((names[i] == active) ? "true" : "false") +
                       ",\"diskImage\":\"" + jsonEscape(path) + "\"}";
            }
            arr += "]";
            Json j("vms");
            j.str("activeVm", active).raw("vms", arr);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%s\n", ("活动虚拟机: " +
                             std::string(active.empty() ? "(无)" : active)).c_str());
            printf("%s %s %s %s\n", padTo("名字", 24).c_str(), padTo("状态", 8).c_str(),
                   padTo("活动", 8).c_str(), "磁盘镜像");
            for (const auto &n : names) {
                int st = 0;
                c.vmStatus(n, st);
                std::string path;
                c.diskImagePath(n, path);
                printf("%s %s %s %s\n", padTo(n, 24).c_str(),
                       padTo(std::to_string(st), 8).c_str(),
                       padTo(n == active ? "是" : "否", 8).c_str(),
                       path.empty() ? "(无)" : path.c_str());
            }
            printf("\n注: vm_manager 未提供枚举接口，此表按已知名字探测得出。\n");
        }
        return 0;
    }
    if (cmd == "vm-info") {
        uint32_t ddr = 0, pid = 0;
        int rc = c.getVmInfo(ddr, pid);
        if (rc != 0) return fail(cmd, rc, "GetVmInfo 失败（可能没有活动虚拟机）");
        if (g_json) {
            Json j(cmd);
            j.num("ddrSizeMb", ddr).num("vmPid", pid);
            printf("%s\n", j.ok().c_str());
        } else {
            printRow("DDR 大小", std::to_string(ddr) + " MB");
            printRow("虚拟机 PID", std::to_string(pid));
        }
        return 0;
    }
    if (cmd == "stratovirt-mem") {
        int mem = 0;
        int rc = c.stratovirtMem(mem);
        if (rc != 0) return fail(cmd, rc, "GetStratovirtMem 失败");
        if (g_json) {
            Json j(cmd);
            j.num("memMb", mem);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%d MB\n", mem);
        }
        return 0;
    }
    if (cmd == "host-sn") {
        std::string sn;
        int rc = c.hostSn(sn);
        if (rc != 0) return fail(cmd, rc, "GetHostSN 失败");
        if (g_json) {
            Json j(cmd);
            j.str("sn", sn);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%s\n", sn.c_str());
        }
        return 0;
    }

    if (cmd == "vm") return cmdVm(c, a.pos);
    if (cmd == "pause" || cmd == "resume" || cmd == "lock-guest" || cmd == "lx-ota" ||
        cmd == "lx-snapshot" || cmd == "rgm-status" || cmd == "recover-user-data" ||
        cmd == "autopause" ||
        cmd == "linux-data-delete" || cmd == "rgm-image-delete" || cmd == "share-volumes" ||
        cmd == "linux-path" || cmd == "gallery-share" || cmd == "guest-disk-share" ||
        cmd == "pasteboard" || cmd == "buffer" || cmd == "perf" || cmd == "perf-ex" ||
        cmd == "screen-lock-task" || cmd == "tablet") {
        return cmdFusion(c, cmd, a);
    }

    if (cmd == "info" || cmd == "status") return cmdInfo(c);

    if (cmd == "capability") {
        bool cap = false;
        int rc = c.checkVmCapability(cap);
        if (rc != 0) return fail(cmd, rc, "CheckVmCapability 失败");
        if (g_json) {
            Json j(cmd);
            j.boolean("capable", cap);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%s\n", cap ? "支持" : "不支持");
        }
        return 0;
    }
    if (cmd == "active-name") {
        std::string name;
        int rc = c.activeVmName(name);
        if (rc != 0) return fail(cmd, rc, "GetActiveVmName 失败");
        if (g_json) {
            Json j(cmd);
            j.str("name", name);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%s\n", name.empty() ? "(无)" : name.c_str());
        }
        return 0;
    }
    if (cmd == "active-status") {
        int st = 0;
        int rc = c.activeVmStatus(st);
        if (rc != 0) return fail(cmd, rc, "GetActiveVmStatus 失败");
        if (g_json) {
            Json j(cmd);
            j.num("status", st);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%d (%s)\n", st, hvm::statusName(st));
        }
        return 0;
    }
    if (cmd == "vm-status") {
        int st = 0;
        std::string vm = a.pos.empty() ? a.vm : a.pos[0];
        int rc = c.vmStatus(vm, st);
        if (rc != 0) return fail(cmd, rc, "GetVmStatus 失败");
        if (g_json) {
            Json j(cmd);
            j.str("vm", vm).num("status", st);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%s: %d (%s)\n", vm.c_str(), st, hvm::statusName(st));
        }
        return 0;
    }
    if (cmd == "process-exist") {
        NEED_ARGS(1, "hvm-cli process-exist <进程名>");
        int rc = c.isProcessExist(a.pos[0]);
        if (g_json) {
            Json j(cmd);
            j.str("name", a.pos[0]).boolean("exists", rc == 1).num("rc", rc);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%s\n", rc == 1 ? "存在" : "不存在");
        }
        return 0;
    }
    if (cmd == "feature") {
        NEED_ARGS(1, "hvm-cli feature <ID>");
        bool sup = false;
        int rc = c.isFeatureSupported(std::atoi(a.pos[0].c_str()), sup);
        if (rc != 0) return fail(cmd, rc, "IsFeatureSupported 失败");
        if (g_json) {
            Json j(cmd);
            j.boolean("supported", sup);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%s\n", sup ? "支持" : "不支持");
        }
        return 0;
    }
    if (cmd == "open-euler-version") {
        std::string v;
        int rc = c.openEulerVersion(v);
        if (rc != 0) return fail(cmd, rc, "GetOpenEulerVersion 失败");
        if (g_json) {
            Json j(cmd);
            j.str("version", v);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%s\n", v.empty() ? "(未知)" : v.c_str());
        }
        return 0;
    }
    if (cmd == "hash-name") {
        // 实测：本机（未 provision，/data/virt_service 不存在）调用该接口会在
        // 系统库内部解引用 *(this+184) 得到空指针而段错误，调试器抓到的现场：
        //   VmManagerClient::GetHashName: ldr x0,[x0,#184]; ldr x9,[x0]  ← 崩在这
        // 这是库自身的健壮性问题（我们的调用约定经其它按值返回接口验证无误），
        // 因此这里直接拒绝执行，避免用户看到段错误。
        fprintf(stderr,
                "hash-name 已禁用：GetHashName 在本机未 provision 的环境下会在\n"
                "系统库内空指针崩溃（VmManagerClient::GetHashName 解引用 *(this+184)）。\n"
                "该接口与本地开发无关，如需请先让 vm_manager 完成 provisioning。\n");
        return 3;
    }
    if (cmd == "hash-name-disabled") {
        std::string v;
        int rc = c.hashName(v);
        if (rc != 0) return fail(cmd, rc, "GetHashName 失败");
        if (g_json) {
            Json j(cmd);
            j.str("hashName", v);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%s\n", v.c_str());
        }
        return 0;
    }
    if (cmd == "quick-start" || cmd == "is-installing") {
        int rc = (cmd == "quick-start") ? c.isQuickStartScenario() : c.isInstalling();
        bool yes = rc == 1;
        if (g_json) {
            Json j(cmd);
            j.boolean(cmd == "quick-start" ? "quickStart" : "installing", yes).num("rc", rc);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%s\n", yes ? "是" : "否");
        }
        return 0;
    }

    // ------------------------------------------------------------ 电源
    if (cmd == "force-stop") {
        std::string vm = a.pos.empty() ? a.vm : a.pos[0];
        int rc = c.forceStop(vm);
        if (rc != 0) return fail(cmd, rc, "ForceStopVm 失败");
        if (g_json) {
            Json j(cmd);
            j.str("vm", vm);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("已向 %s 发送强制关机\n", vm.c_str());
        }
        return 0;
    }
    if (cmd == "quit-by-reboot-host") {
        int rc = c.quitByRebootHost();
        if (rc != 0) return fail(cmd, rc, "VmQuitByRebootHost 失败");
        if (g_json) {
            Json j(cmd);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("已通知虚拟机退出\n");
        }
        return 0;
    }
    if (cmd == "require-big-mem") {
        int rc = c.requireBigMem();
        if (rc != 0) return fail(cmd, rc, "RequireBigMem 失败");
        if (g_json) {
            Json j(cmd);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("已申请大内存\n");
        }
        return 0;
    }

    // ------------------------------------------------------------ 快照
    if (cmd == "snapshot") {
        NEED_ARGS(1, "hvm-cli snapshot list|create|restore|destroy|rename [参数]");
        const std::string &act = a.pos[0];
        std::string vm = a.vm;
        if (act == "list") {
            std::vector<std::pair<std::string, std::string>> snaps;
            int rc = c.snapshotList(vm, snaps);
            if (rc != 0) return fail(cmd, rc, "GetSnapshotList 失败");
            if (g_json) {
                std::string arr = "[";
                for (size_t i = 0; i < snaps.size(); ++i) {
                    if (i) arr += ",";
                    arr += "{\"name\":\"" + jsonEscape(snaps[i].first) + "\",\"path\":\"" +
                           jsonEscape(snaps[i].second) + "\"}";
                }
                arr += "]";
                Json j("snapshot list");
                j.str("vm", vm).raw("snapshots", arr);
                printf("%s\n", j.ok().c_str());
            } else if (snaps.empty()) {
                printf("(无快照)\n");
            } else {
                for (auto &kv : snaps) printf("%s\t%s\n", kv.first.c_str(), kv.second.c_str());
            }
            return 0;
        }
        NEED_ARGS(2, "hvm-cli snapshot create|restore|destroy <名字>");
        int rc = 0;
        std::string what;
        if (act == "create") {
            rc = c.snapshotCreate(vm, a.pos[1]);
            what = "创建";
        } else if (act == "restore") {
            rc = c.snapshotRestore(vm, a.pos[1]);
            what = "恢复";
        } else if (act == "destroy") {
            rc = c.snapshotDestroy(vm, a.pos[1]);
            what = "删除";
        } else if (act == "rename") {
            NEED_ARGS(3, "hvm-cli snapshot rename <旧名> <新名>");
            rc = c.snapshotRename(vm, a.pos[1], a.pos[2]);
            what = "重命名";
        } else {
            return fail(cmd, 2, "未知快照动作: " + act);
        }
        if (rc != 0) return fail(cmd, rc, "快照" + what + "失败");
        if (g_json) {
            Json j("snapshot " + act);
            j.str("vm", vm);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("快照%s完成: %s\n", what.c_str(), a.pos[1].c_str());
        }
        return 0;
    }

    // ------------------------------------------------------------ 共享目录
    if (cmd == "share") {
        NEED_ARGS(1, "hvm-cli share list|enable|disable|add|remove|setup");
        const std::string &act = a.pos[0];
        if (act == "list") {
            std::string v;
            int rc = c.sharedFolder(v);
            if (rc != 0) return fail(cmd, rc, "GetSharedFolder 失败");
            if (g_json) {
                Json j("share list");
                j.str("sharedFolder", v);
                printf("%s\n", j.ok().c_str());
            } else {
                printf("%s\n", v.empty() ? "(空)" : v.c_str());
            }
            return 0;
        }
        if (act == "enable" || act == "disable") {
            c.setSharedFolderEnabled(act == "enable");
            if (g_json) {
                Json j("share " + act);
                printf("%s\n", j.ok().c_str());
            } else {
                printf("共享目录已%s\n", act == "enable" ? "开启" : "关闭");
            }
            return 0;
        }
        if (act == "setup") {
            int rc = c.setupSharedFolder(a.vm);
            if (rc != 0) return fail(cmd, rc, "SetUpSharedFolder 失败");
            if (g_json) {
                Json j("share setup");
                printf("%s\n", j.ok().c_str());
            } else {
                printf("已触发共享目录建立\n");
            }
            return 0;
        }
        if (act == "add") {
            NEED_ARGS(3, "hvm-cli share add <宿主路径> <客机路径>");
            int rc = c.addSharedFolder(a.vm, a.pos[1], a.pos[2]);
            if (rc != 0) return fail(cmd, rc, "AddSharedFolder 失败");
            if (g_json) {
                Json j("share add");
                j.str("host", a.pos[1]).str("guest", a.pos[2]);
                printf("%s\n", j.ok().c_str());
            } else {
                printf("已添加共享目录 %s -> %s\n", a.pos[1].c_str(), a.pos[2].c_str());
            }
            return 0;
        }
        if (act == "remove") {
            NEED_ARGS(2, "hvm-cli share remove <宿主路径>");
            int rc = c.removeSharedFolder(a.vm, a.pos[1]);
            if (rc != 0) return fail(cmd, rc, "RemoveSharedFolder 失败");
            if (g_json) {
                Json j("share remove");
                j.str("host", a.pos[1]);
                printf("%s\n", j.ok().c_str());
            } else {
                printf("已移除共享目录 %s\n", a.pos[1].c_str());
            }
            return 0;
        }
        return fail(cmd, 2, "未知共享目录动作: " + act);
    }

    // ------------------------------------------------------------ 网络
    if (cmd == "net") {
        NEED_ARGS(1, "hvm-cli net ip|proxy|share-on|share-off|dns-on|dns-off|"
                      "proxy-status-on|proxy-status-off|proxy-auto-on|proxy-auto-off");
        const std::string &act = a.pos[0];
        if (act == "ports" || act == "localhost-ports") {
            std::vector<std::array<std::uint32_t, 3>> v;
            int rc = (act == "ports") ? c.getPortForwardForNat(a.vm, v)
                                      : c.getLocalhostForwardFromVmToHost(a.vm, v);
            if (rc != 0)
                return fail(cmd, rc, act == "ports" ? "GetPortForwardForNat 失败"
                                                    : "GetLocalhostForwardFromVmToHost 失败");
            if (g_json) {
                Json j(cmd);
                j.num("count", v.size());
                printf("%s\n", j.ok().c_str());
            }
            printf("条目数 %zu（每行：字段1 字段2 字段3）\n", v.size());
            for (const auto &e : v)
                printf("  %u  %u  %u\n", e[0], e[1], e[2]);
            return 0;
        }
        if (act == "mode") {
            if (a.pos.size() < 2) {
                fprintf(stderr, "net mode bridge|nat [接口名]\n");
                return 2;
            }
            int32_t mode = (a.pos[1] == "nat" || a.pos[1] == "1") ? 1 : 0;   // MODE_NAT=1, MODE_BRIDGE=0
            std::string iface = a.pos.size() > 2 ? a.pos[2] : std::string();
            int rc = c.setVmNetMode(a.vm, mode, iface);
            if (rc != 0) return fail(cmd, rc, "SetVmNetMode 失败");
            printf("已设置网络模式: %s\n", mode == 1 ? "NAT" : "桥接");
            return 0;
        }
        if (act == "proxy-status-on" || act == "proxy-status-off") {
            int rc = c.setVmHostNetProxyStatus(a.vm, act == "proxy-status-on");
            if (rc != 0) return fail(cmd, rc, "SetVmHostNetProxyStatus 失败");
            printf("已设置宿主网络代理状态: %s\n", act == "proxy-status-on" ? "开" : "关");
            return 0;
        }
        if (act == "proxy-auto-on" || act == "proxy-auto-off") {
            int rc = c.setProxyAutoSyncEnabled(a.vm, act == "proxy-auto-on");
            if (rc != 0) return fail(cmd, rc, "SetProxyAutoSyncEnabled 失败");
            printf("已设置代理自动同步: %s\n", act == "proxy-auto-on" ? "开" : "关");
            return 0;
        }
        if (act == "ip") {
            std::string ip;
            int rc = c.vmIpv4Address(a.vm, ip);
            if (rc != 0) return fail(cmd, rc, "GetVmIpv4Address 失败");
            if (g_json) {
                Json j("net ip");
                j.str("ipv4", ip);
                printf("%s\n", j.ok().c_str());
            } else {
                printf("%s\n", ip.empty() ? "(未知)" : ip.c_str());
            }
            return 0;
        }
        if (act == "proxy") {
            bool on = false;
            int rc = c.hostNetProxyStatus(a.vm, on);
            if (rc != 0) return fail(cmd, rc, "GetVmHostNetProxyStatus 失败");
            if (g_json) {
                Json j("net proxy");
                j.boolean("enabled", on);
                printf("%s\n", j.ok().c_str());
            } else {
                printf("%s\n", on ? "开启" : "关闭");
            }
            return 0;
        }
        bool on = act == "share-on" || act == "dns-on";
        int rc = (act == "dns-on" || act == "dns-off") ? c.setDnsAutoSync(a.vm, on)
                                                       : c.switchNetworkShare(a.vm, on);
        if (rc != 0) return fail(cmd, rc, "网络设置失败");
        if (g_json) {
            Json j("net " + act);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("已设置 %s = %s\n", act.c_str(), on ? "开" : "关");
        }
        return 0;
    }

    // ------------------------------------------------------------ 磁盘
    if (cmd == "disk") {
        NEED_ARGS(1, "hvm-cli disk capacity|path|size|expand|delete-data");
        const std::string &act = a.pos[0];
        if (act == "capacity" || act == "size") {
            int64_t bytes = 0;
            int rc = (act == "capacity") ? c.diskCapacity(a.vm, bytes)
                                         : c.diskImageFileSize(a.vm, bytes);
            if (rc != 0) return fail(cmd, rc, "查询磁盘失败");
            if (g_json) {
                Json j("disk " + act);
                j.num("bytes", static_cast<long long>(bytes)).str("human", humanBytes(bytes));
                printf("%s\n", j.ok().c_str());
            } else {
                printf("%lld (%s)\n", static_cast<long long>(bytes), humanBytes(bytes).c_str());
            }
            return 0;
        }
        if (act == "path") {
            std::string p;
            int rc = c.diskImagePath(a.vm, p);
            if (rc != 0) return fail(cmd, rc, "GetVmDiskImagePath 失败");
            if (g_json) {
                Json j("disk path");
                j.str("path", p);
                printf("%s\n", j.ok().c_str());
            } else {
                printf("%s\n", p.c_str());
            }
            return 0;
        }
        if (act == "expand") {
            NEED_ARGS(2, "hvm-cli disk expand <GB>");
            int rc = c.expandCapacity(a.vm, std::atoi(a.pos[1].c_str()));
            if (rc != 0) return fail(cmd, rc, "VmExpandCapacity 失败");
            if (g_json) {
                Json j("disk expand");
                j.str("gb", a.pos[1]);
                printf("%s\n", j.ok().c_str());
            } else {
                printf("已扩容到 %s GB\n", a.pos[1].c_str());
            }
            return 0;
        }
        if (act == "delete-data") {
            int rc = c.deleteLinuxDataImage();
            if (rc != 0) return fail(cmd, rc, "DeleteLinuxDataImage 失败");
            if (g_json) {
                Json j("disk delete-data");
                printf("%s\n", j.ok().c_str());
            } else {
                printf("已删除 Linux 数据镜像\n");
            }
            return 0;
        }
        return fail(cmd, 2, "未知磁盘动作: " + act);
    }

    // ------------------------------------------------------------ 显示 / 内存
    if (cmd == "resolution") {
        NEED_ARGS(2, "hvm-cli resolution <宽> <高> [--full]");
        uint32_t w = static_cast<uint32_t>(std::atoi(a.pos[0].c_str()));
        uint32_t h = static_cast<uint32_t>(std::atoi(a.pos[1].c_str()));
        int rc = c.modifyResolution(w, h, a.full);
        if (rc != 0) return fail(cmd, rc, "ModifyResolution 失败");
        if (g_json) {
            Json j(cmd);
            j.num("width", w).num("height", h).boolean("fullScreen", a.full);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("已设置分辨率 %ux%u%s\n", w, h, a.full ? "（全屏）" : "");
        }
        return 0;
    }
    if (cmd == "touch-mem") {
        NEED_ARGS(1, "hvm-cli touch-mem <MB>");
        int rc = c.touchVmMem(static_cast<uint32_t>(std::atoi(a.pos[0].c_str())));
        if (rc != 0) return fail(cmd, rc, "TouchVmMem 失败");
        if (g_json) {
            Json j(cmd);
            j.str("mb", a.pos[0]);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("已触达内存 %s MB\n", a.pos[0].c_str());
        }
        return 0;
    }
    if (cmd == "swap-2d") {
        NEED_ARGS(1, "hvm-cli swap-2d <MB>");
        int rc = c.set2dSwapSpace(std::atoi(a.pos[0].c_str()));
        if (rc != 0) return fail(cmd, rc, "Set2DSwapSpace 失败");
        if (g_json) {
            Json j(cmd);
            j.str("mb", a.pos[0]);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("已设置 2D 交换空间 %s MB\n", a.pos[0].c_str());
        }
        return 0;
    }

    fprintf(stderr, "未知命令: %s\n\n", cmd.c_str());
    usage();
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    // 先扫描一遍全局选项，便于 `hvm-cli info --json` 这类写法
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0) g_json = true;
    }
    return run(argc, argv);
}
