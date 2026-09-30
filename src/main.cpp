// main.cpp —— hvm-cli 命令行入口
//
// 纯 C++ 实现：直接使用系统自带的 libvm_manager_kits.z.so，
// 不需要 root、不需要 HAP。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>   // getuid：用 uid/200000 推导 OS 账号 id
#include <algorithm>   // sort/unique/remove/find（本地虚拟机清单）
#include <fstream>     // 清单文件读写
#include <chrono>      // vmlog -f 轮询
#include <regex>       // 识别日志行前缀
#include <thread>      // vmlog -f


#include <cstring>
#include <string>
#include <vector>

#include "cfginfo.h"
#include "hvm_client.h"
#include "ohos/vm_manager_service/vm_manager_errcode.h"
#include "sha256.h"

namespace {

//: 计算源镜像摘要时的进度显示（写在 stderr，不污染正常输出）
void importProgress(uint64_t done, uint64_t total) {
    if (total == 0) return;
    if (!isatty(STDERR_FILENO)) return;   // 重定向/管道时不刷屏
    static uint64_t lastPct = 101;
    const uint64_t pct = done * 100 / total;
    if (pct != lastPct) {
        lastPct = pct;
        fprintf(stderr, "\r读取中 %llu%%", static_cast<unsigned long long>(pct));
        if (pct >= 100) fprintf(stderr, "\n");
        fflush(stderr);
    }
}

}  // namespace

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
        "关于虚拟机名:\n"
        "  需要指定虚拟机的命令（stop / force-stop / net / disk / share / snapshot …）\n"
        "  用位置参数或 --vm <名字> 给出（哪些命令支持位置形式见各命令自己的提示）。\n"
        "  · 写操作**必须**显式给名字，省略会直接报错、不会替你猜；\n"
        "  · 读操作可以省略，此时按\"当前虚拟机\"处理，并会先把用到的名字打印出来。\n"
        "  （源码里不再有任何\"默认虚拟机名\"。）\n"
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
        "  --vm <名字>     目标虚拟机名（没有默认值；写操作必须给，见上）\n"
        "\n"
        "状态:\n"
        "  info                    汇总状态（能力/当前 VM/版本/共享目录）\n"
        "  list                    枚举我们自己记录的虚拟机清单（见 preferences 下的清单文件）\n"
        "  start --net nat|bridge [--nic 网卡] [--bridge-ip IP] [--proxy-sync] [--dns-sync]\n"
        "  vmlog [-f]              只打印虚拟机串口日志（-f 跟随；其它模块的日志不打印）\n"
        "  vms [名字...]           按已知名字探测虚拟机（服务端无枚举接口）\n"
        "\n"
        "虚拟机生命周期（CfgInfo 为逆向手工构造，见 docs/api-notes.md）：\n"

        "  create --name N --image P [选项]\n"
        "  start  --name N [选项]\n"




        "  range                   查询可用的 CPU / 内存范围（服务端校验依据）\n"

        "  mount-cd   --name N --image X.iso   挂载安装光盘\n"
        "  unmount-cd --name N --image X.iso   卸载\n"
        "  destroy N               销毁虚拟机\n"
        "    选项: --cpu N --mem GB --disk MB | --disk-gb GB\n"
        "          --bios PATH --enhance PATH --start-type N --partition --dynamic-mem\n"
        "    单位（实测）：memorySize 为 GB（范围见 range），diskSize 为 MB 且 >= 65536\n"
        "  pause / resume [名字]   暂停 / 恢复虚拟机\n"
        "  lock-guest              锁定客户机（LockGuest）\n"
        "  lx-ota                  Linux 环境 OTA（LxOtaHandle）\n"
        "  lx-snapshot <名> <op>   Linux 虚拟机快照（HandleLxSnapshot）\n"
        "  rgm-status [名字]       查询 RGM 镜像状态（GetRgmImageStatusFromVm）\n"
        "  recover-user-data <路径>  恢复用户数据（RecoverUserData）\n"
        "  autopause <0|1|3|10|15|30>  自动暂停时间（0=关闭，其余为分钟）\n"



        "  linux-data-delete       删除 Linux 数据镜像\n"
        "  rgm-image-delete <镜像> 删除 RGM 镜像（DeleteRgmImageFromVm）\n"


        "  gallery-share on|off    宿主图库共享（SetHostGallerySharedEnabled）\n"
        "  guest-disk-share <路径> on|off  客户机磁盘共享\n"
        "  pasteboard [status|enable|disable|usable-enable|usable-disable|add A B|remove A]\n"


        "  screen-lock-task on|off 锁屏任务开关\n"
        "  tablet <int>            平板切换上报\n"
        "  vminfo                  当前虚拟机的 DDR 大小（GB）与进程 PID\n"
        "  stratovirt-mem          stratoVirt 占用内存（KB）\n"
        "  host-sn                 宿主 SN\n"
        "  capability              本机是否支持虚拟化\n"
        "  active-name             当前虚拟机名\n"
        "  active-status           当前虚拟机状态码\n"
        "  vmstat [名字]           指定虚拟机状态码\n"
        "  process-exist <进程名>  进程是否存在\n"
        "  feature <ID>            特性是否支持\n"
        "  open-euler-version      openEuler 镜像版本\n"

        "  quick-start             是否快速启动场景\n"
        "  is-installing           是否安装中\n"
        "\n"
        "电源:\n"
        "  stop <名字> [--clean]   正常关机（服务端 StopVm）\n"
        "  force-stop <名字>       强制关机（服务端 ForceStopVm）\n"
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
        "  net ip                     客户机 IPv4 地址\n"
        "  net proxy                  查询宿主网络代理状态\n"
        "  net share-on | share-off   网络共享开关（SwitchVmNetworkShare）\n"
        "  net dns-on   | dns-off     DNS 自动同步开关（SetDnsAutoSyncEnabled）\n"
        "  net mode bridge|nat [接口] 网络模式（MODE_BRIDGE=0 / MODE_NAT=1）\n"
        "  net ports                  查询 NAT 端口转发表（GetPortForwardForNat）\n"
        "  net localhost-ports        查询本机转发表（GetLocalhostForwardFromVmToHost）\n"
        "  net proxy-status-on|proxy-status-off   设置宿主网络代理状态（SetVmHostNetProxyStatus）\n"
        "  net proxy-auto-on|proxy-auto-off       代理自动同步开关（SetProxyAutoSyncEnabled）\n"
        "\n"
        "磁盘:\n"
        "  disk capacity | disk path | disk size | disk expand <GB> | disk delete-data\n"
        "\n"
        "磁盘导出 / 导入（把某台虚拟机的磁盘导出成文件；或从镜像文件导入成一台新虚拟机）:\n"
        "  export --name <虚拟机> --src <目标目录> --dst <文件名>            导出该虚拟机的磁盘\n"
        "  import --name <新虚拟机名> --src <镜像文件> [--dst <sha256>]      从镜像导入成一台新虚拟机\n"
        "      · <目标目录> 必须已存在；最终文件是 目录/文件名，导出后可在文件管理器里找到\n"
        "      · <镜像文件> 给完整路径；--dst 可省略（省略时本工具多线程现算摘要），大小写随意\n"
        "      · 导入用的虚拟机名必须【尚不存在】（导入动作本身就会创建这台虚拟机）\n"
        "      · import 建出的虚拟机还没有配置，启动时要显式给参数：start <名字> --cpu 6 --mem 6\n"
        "\n"
        "显示/内存:\n"
        "  resolution <宽> <高> [--full]\n"
        "  touch-mem <MB>\n"
        "  swap-2d <MB>\n"
        "\n"
        "开发与验证命令（日常不需要；逆向/自检用，部分在 HiShell 身份下不可用）：\n"
        "  ctor [选项]             仅构造 CfgInfo 并打印（不调服务，验证构造配方）\n"
        "  view-state <0|1|2>      上报 HapViewState（研究视图机制）\n"
        "  displays <id[,…]>       把显示器 id 列表交给服务端（同上）\n"
        "  serial-read  --name N [--chan C] [--type T] [--arg A]   读客户机通道\n"
        "  serial-write --name N --data TEXT [--chan C] [--type T] 写客户机通道\n"
        "  selftest                kit 加载自检\n"
        "  sha256 <文件> [线程数]  计算文件 SHA-256（内置实现、多线程预读）\n"
        "  hash-name               迁移用的 Hash 名（未发起过迁移时为空）\n"
        "  share-volumes           列出全部共享卷（返回元素类型未还原，已禁用）\n"
        "  linux-path <宿主路径..> 宿主→客户机路径（服务端只允许 LinuxFusion 服务调用）\n"
        "  buffer avail|low <字节> 上报内存阈值（是“上报”而非查询，慎用）\n"
        "  perf <a> <b> / perf-ex <a> on|off <b>  性能请求（服务端 permission denied）\n");
}

// ---------------------------------------------------------------- 主逻辑
struct Args {
    std::vector<std::string> pos;
    //: 只有 --vm 显式给出时才非空。不再有任何"默认虚拟机名" ——
    //: 早期那句 `= hvm::kLinuxVm` 会让不带名字的命令悄悄作用在 virtualized_linux 上，
    //: 对 force-stop / snapshot destroy / disk expand 这类写操作是危险的。
    std::string vm;
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
    printRow("当前虚拟机", vi.activeVm.empty() ? "(无)" : vi.activeVm);
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
//: 取媒体库视图里的那个数字（OS 账号 id）。
//: 优先读环境变量 USER —— 本机 HiShell 终端里就是 USER=100，正是该数字；
//: 拿不到或不是纯数字时，按 OpenHarmony 的 uid 编码规则回落：
//:   uid = userId * 200000 + appId  →  userId = uid / 200000
//:   （HiShell 的 20020085 → 100；第二账号下的应用 202xxxxx → 101）
//: HVM_USER_ID 可显式覆盖（例如要引用别的账号视图下的文件）。
std::string currentUserId() {
    if (const char *env = getenv("HVM_USER_ID")) {
        if (*env != '\0') return env;
    }
    if (const char *user = getenv("USER")) {
        bool allDigits = (*user != '\0');
        for (const char *p = user; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9') {
                allDigits = false;
                break;
            }
        }
        if (allDigits) return user;
    }
    const std::string derived = std::to_string(static_cast<unsigned>(getuid()) / 200000u);
    return derived == "0" ? "100" : derived;
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

//: 解析"要操作哪台虚拟机"。
//:   positional = true  允许用位置参数给名字（如 `stop <名字>`）；否则只看 --vm
//:   write      = true  写操作：必须显式给名字，否则报错返回空串
//:   write      = false 读操作：允许省略，此时回落到"当前虚拟机"并**把名字打出来**
//:                      （不打出来就等于在替用户猜，这正是要避免的）
//: 返回空串表示调用方应直接 return 2（原因已经打印清楚了）。
std::string resolveVm(Client &c, const Args &a, const std::string &cmd, bool positional, bool write) {
    if (positional && !a.pos.empty()) return a.pos[0];
    if (!a.vm.empty()) return a.vm;
    if (write) {
        // 位置参数能不能给名字，取决于命令本身（如 `stop <名字>` 可以，
        // 而 `share add`/`net mode` 的第一个位置参数是自己的参数），提示要如实。
        if (positional)
            fprintf(stderr, "%s 需要指定虚拟机：%s <名字>，或 --vm <名字>\n", cmd.c_str(),
                    cmd.c_str());
        else
            fprintf(stderr, "%s 需要指定虚拟机：--vm <名字>\n", cmd.c_str());
        return {};
    }
    std::string cur;
    if (c.activeVmName(cur) == 0 && !cur.empty()) {
        fprintf(stderr, "（未指定虚拟机，按当前虚拟机 %s 处理）\n", cur.c_str());
        return cur;
    }
    fprintf(stderr, "%s 需要指定虚拟机：--vm <名字>（当前也没有可用的当前虚拟机）\n",
            cmd.c_str());
    return {};
}

// ---------------------------------------------------------------- LinuxFusion / RGM 运维
//: 这一组都是 kit 接口直通：签名已由 include/ohos/vm_manager_service/vm_manager_kits.h
//: 给出（编译器生成的符号名），不需要构造任何私有类。
static int cmdFusion(Client &c, const std::string &cmd, const Args &a) {
    // 注意：这里不再统一取 a.vm —— 有些子命令不需要虚拟机名（pause/lock-guest/…），
    // 有些需要（resume/recover-user-data/…），需要时才解析，避免误报"缺少名字"。
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
        printf("已请求暂停当前虚拟机\n");
        return 0;
    }
    if (cmd == "resume") {
        std::string name = resolveVm(c, a, "resume", /*positional=*/true, /*write=*/true);
        if (name.empty()) return 2;
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
        const std::string vm = resolveVm(c, a, "lx-snapshot", false, true);
        if (vm.empty()) return 2;
        return report(c.handleLxSnapshot(vm, a.pos[0], atoi(a.pos[1].c_str())),
                      "处理 Linux 虚拟机快照");
    }
    if (cmd == "rgm-status") {
        std::string name = resolveVm(c, a, "rgm-status", /*positional=*/true, /*write=*/false);
        if (name.empty()) return 2;
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
        const std::string vm = resolveVm(c, a, "recover-user-data", false, true);
        if (vm.empty()) return 2;
        return report(c.recoverUserData(vm, a.pos[0]), "恢复用户数据");
    }
    if (cmd == "autopause") {
        if (a.pos.empty()) {
            fprintf(stderr, "autopause <0|1|3|10|15|30>   （0=关闭，其余为分钟数）\n");
            return 2;
        }
        int32_t m = static_cast<int32_t>(atoi(a.pos[0].c_str()));
        const std::string vm = resolveVm(c, a, "autopause", false, true);
        if (vm.empty()) return 2;
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
        const std::string vm = resolveVm(c, a, "gallery-share", false, true);
        if (vm.empty()) return 2;
        return report(c.setHostGalleryShared(vm, onoff(a.pos[0])), "设置图库共享");
    }
    if (cmd == "guest-disk-share") {
        if (a.pos.size() < 2) {
            fprintf(stderr, "guest-disk-share <路径> on|off\n");
            return 2;
        }
        const std::string vm = resolveVm(c, a, "guest-disk-share", false, true);
        if (vm.empty()) return 2;
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

// ---------------------------------------------------------------- 串口日志
//: 只打印虚拟机串口（-serial redirect-to-log → /data/log/hwf_service/vmlog）的内容。
//: 串口行形如：
//:   2026-..T..: [pid][tid][chardev_backend/src/chardev.rs:207]:INFO: <客户机原始输出>
//: 客户机自己的一次输出可能跨多行，续行**没有**日志前缀，必须原样打印；
//: 而 stratoVirt 其它模块的多行日志（例如 VmConfig 那种超长 dump）不跟在串口行后面，
//: 因此不会被误当成串口输出。
constexpr const char *kVmLogPath = "/data/log/hwf_service/vmlog";

//: 匹配日志行前缀：<时间戳>: [pid][tid][模块:行]:LEVEL:
static bool isLogPrefixed(const std::string &line) {
    static const std::regex re(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}[^ ]*: \[\d+\]\[\d+\]\[[^\]]+\]:[A-Z]+:)");
    return std::regex_search(line, re);
}
static bool isSerialLine(const std::string &line) {
    return line.find("chardev_backend/src/chardev.rs") != std::string::npos;
}
//: 取日志前缀之后的内容
static std::string stripLogPrefix(const std::string &line) {
    std::size_t p = line.find("]:INFO: ");
    if (p != std::string::npos) return line.substr(p + 7);
    p = line.find("]:ERROR: ");
    if (p != std::string::npos) return line.substr(p + 8);
    return line;
}

//: 把整个文件（或新增部分）按上述规则过滤后写到 stdout；返回处理到的偏移
static long long emitVmLog(std::istream &in, bool *inGuest, long long *lastPrint) {
    std::string line;
    long long printed = 0;
    while (std::getline(in, line)) {
        if (isSerialLine(line)) {
            std::string text = stripLogPrefix(line);
            printf("%s\n", text.c_str());
            *inGuest = true;
            ++printed;
        } else if (!isLogPrefixed(line)) {
            if (*inGuest) {           // 串口输出的续行（客户机自己换的行）
                printf("%s\n", line.c_str());
                ++printed;
            }
        } else {
            *inGuest = false;         // 其它模块的日志：丢弃
        }
    }
    if (lastPrint) *lastPrint = printed;
    return printed;
}

// ------------------------------------------------------- 本地虚拟机清单（自管理）
//: vm_manager 没有枚举接口（见 docs/api-notes.md §13），所以由我们自己记一份清单：
//:     /data/storage/el2/base/preferences/hvm-cli-vms.list     （一行一个虚拟机名）
//: 维护点：`vm create` 成功后追加、`vm destroy` 成功后移除；`hvm-cli list` 用它来枚举。
constexpr const char *kRegistryPath =
    "/data/storage/el2/base/preferences/hvm-cli-vms.list";

std::vector<std::string> registryLoad() {
    std::vector<std::string> names;
    std::ifstream in(kRegistryPath);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) names.push_back(line);
    }
    return names;
}

bool registrySave(std::vector<std::string> names) {
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    const std::string tmp = std::string(kRegistryPath) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        for (const auto &n : names) out << n << "\n";
        if (!out) return false;
    }
    return std::rename(tmp.c_str(), kRegistryPath) == 0;
}

//: 返回 false 表示清单没写成功（调用方只提示、不影响主操作）
bool registryAdd(const std::string &name) {
    auto names = registryLoad();
    if (std::find(names.begin(), names.end(), name) != names.end()) return true;
    names.push_back(name);
    return registrySave(std::move(names));
}

bool registryRemove(const std::string &name) {
    auto names = registryLoad();
    auto it = std::remove(names.begin(), names.end(), name);
    if (it == names.end()) return true;   // 本来就不在
    names.erase(it, names.end());
    return registrySave(std::move(names));
}

//: 打印虚拟机表格（`vms` 与 `list` 共用）
int printVmTable(Client &c, const std::string &cmd,
                 const std::vector<std::string> &names) {
    std::string active;
    c.activeVmName(active);
    if (g_json) {
        std::string arr = "[";
        for (std::size_t i = 0; i < names.size(); ++i) {
            int st = 0;
            int rc = c.vmStatus(names[i], st);
            std::string path;
            c.diskImagePath(names[i], path);
            if (i) arr += ",";
            arr += "{\"name\":\"" + jsonEscape(names[i]) +
                   "\",\"rc\":" + std::to_string(rc) +
                   ",\"status\":" + std::to_string(st) +
                   ",\"active\":" + ((names[i] == active) ? "true" : "false") +
                   ",\"exists\":" + (path.empty() ? "false" : "true") +
                   ",\"diskImage\":\"" + jsonEscape(path) + "\"}";
        }
        arr += "]";
        Json j(cmd);
        j.str("activeVm", active).raw("vms", arr);
        printf("%s\n", j.ok().c_str());
        return 0;
    }
    printf("当前虚拟机: %s\n", active.empty() ? "(无)" : active.c_str());
    if (names.empty()) {
        printf("（没有可显示的虚拟机：本地清单为空，也没探测到框架自带的名字）\n");
        return 0;
    }
    printf("%s %s %s %s\n", padTo("名字", 24).c_str(), padTo("状态", 8).c_str(),
           padTo("当前", 8).c_str(), "磁盘镜像");
    std::vector<std::string> stale;
    for (const auto &n : names) {
        int st = 0;
        c.vmStatus(n, st);
        std::string path;
        c.diskImagePath(n, path);
        if (path.empty()) stale.push_back(n);   // 没有磁盘 = 服务端已不存在（已销毁）
        printf("%s %s %s %s\n", padTo(n, 24).c_str(),
               padTo(std::to_string(st), 8).c_str(),
               padTo(n == active ? "是" : "否", 8).c_str(),
               path.empty() ? "(无，已失效)" : path.c_str());
    }
    if (!stale.empty()) {
        // 只有**确实记在本地清单里**的条目，才该提示"可以从清单删掉"；
        // 像"当前虚拟机"这种服务端遗留记录并不在清单里，提清单只会误导。
        const std::vector<std::string> reg = registryLoad();
        std::vector<std::string> staleInReg, staleElse;
        for (const auto &n : stale) {
            if (std::find(reg.begin(), reg.end(), n) != reg.end()) staleInReg.push_back(n);
            else staleElse.push_back(n);
        }
        auto join = [](const std::vector<std::string> &v) {
            std::string out;
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (i) out += "、";
                out += v[i];
            }
            return out;
        };
        if (!staleInReg.empty())
            printf("提示：清单里的 %s 在服务端已不存在（已销毁）；\n"
                   "      清单文件是纯文本，可直接删掉这些名字：%s\n",
                   join(staleInReg).c_str(), kRegistryPath);
        if (!staleElse.empty())
            printf("提示：%s 在服务端已不存在（已销毁）%s\n", join(staleElse).c_str(),
                   (staleElse.size() == 1 && staleElse[0] == active)
                       ? "（这只是服务端遗留的「当前虚拟机」记录，不在本地清单里）"
                       : "");
    }
    return 0;
}

//: 判断虚拟机在服务端是否还存在。
//: 只能靠"磁盘镜像路径是否为空" —— 状态码 0 对"已停止"和"已销毁"是同一个值，
//: 拿它判断存在性会把已销毁的虚拟机当成还在（这正是本项目踩过的坑）。
bool vmExists(Client &c, const std::string &name, std::string *diskOut = nullptr) {
    std::string path;
    const int rc = c.diskImagePath(name, path);
    if (diskOut != nullptr) *diskOut = path;
    return rc == 0 && !path.empty();
}

//: vms 无参时探测的"框架已知名字"。它只是一个**探测名单**，
//: 不再充当任何命令的默认虚拟机名（见 Args::vm 的说明）。
constexpr const char *kProbeVmNames[] = {hvm::kLinuxVm};

// ---------------------------------------------------------------- vm 子命令
//: CfgInfo 是华为私有类型（无公开头文件），这里按逆向配方手工构造。
int cmdVm(Client &c, const std::vector<std::string> &pos) {
    if (pos.empty()) {
        fprintf(stderr, "用法: hvm-cli ctor|create|start|destroy [选项]\n");
        return 2;
    }
    const std::string act = pos[0];
    std::string name, image, bios, enhance;
    // 用户给的原始写法（本机可读）；bios 之后会被换成服务端视图，本机读不到
    std::string biosRaw;
    int cpu = 0, mem = 0, disk = 0, startType = -1;
    bool partition = false, dynMem = false;
    bool keepSnapshots = false, forceImport = false;
    //: --net nat|bridge：显式给虚拟机建立网络。原理（逆向确认）：
    //: Engine::CheckBeforeStartVm → Engine::NetConfig(CfgInfo,…) → NetManager::AllocateNet
    //: → DefaultNetConfig/BridgeNetConfig → VmNetProperties::SetNetConfigInfo。
    //: 也就是网络是**宿主侧按 CfgInfo 里的字段分配**的。
    //: 实测：这些字段要在 **create** 时就给 —— create --net nat 会生成网卡，
    //: 只加在 start 上则没有网卡。所以发布流程里 create 必须带 --net。
    std::string netMode;                 // --net nat|bridge|none
    std::string netNic;                  // --nic     桥接要用的宿主物理网卡名（NAT 不用）
    std::string netBridgeIp;             // --bridge-ip  桥接 IP
    bool netProxySync = false, netDnsSync = false;
    bool netHostSync = false, netShare = false;
    std::string password;
    std::string chanName = "winbox_serial0";
    unsigned chanType = 0;
    int chanArg = 0;
    std::string dataArg;

    for (std::size_t i = 1; i < pos.size(); ++i) {
        const std::string &k = pos[i];
        if (k == "--keep-snapshots") { keepSnapshots = true; continue; }
        if (k == "--force-import") { forceImport = true; continue; }
        else if (k == "--net" && i + 1 < pos.size()) { netMode = pos[++i]; continue; }
        else if (k == "--nic" && i + 1 < pos.size()) { netNic = pos[++i]; continue; }
        else if (k == "--bridge-ip" && i + 1 < pos.size()) { netBridgeIp = pos[++i]; continue; }
        else if (k == "--proxy-sync") { netProxySync = true; continue; }
        else if (k == "--dns-sync") { netDnsSync = true; continue; }
        else if (k == "--host-net-sync") { netHostSync = true; continue; }
        else if (k == "--net-share") { netShare = true; continue; }
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
        biosRaw = bios;                    // 留给"本机现算摘要"用
        std::string t = toServicePath(bios);
        if (t != bios) {
            if (!g_json)
                printf("（路径转换）%s\n           → %s\n           （账号 id=%s，取自 $USER（回落到 uid %u / 200000））\n",
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
    //: 网络字段（字段名与偏移见 cfginfo.h 的说明；全部来自 napi UnwrapNetworkDevice 与
    //: Engine::NetConfig / NetManager::SetNetConfig 的交叉核对）。
    //: 关键点：+284(networkDevice) 为真时 Engine::NetConfig 会**强制** netMode=1(NAT)，
    //: 所以走桥接必须把它置 false，否则填的 netMode=0 会被覆盖。
    if (netMode == "nat") {
        cfg.setNetworkDevice(true);          // → netMode 取 1（NAT）
        cfg.setNetMode(1);
        if (!netNic.empty()) cfg.setNicName(netNic);
        if (!netBridgeIp.empty()) cfg.setBridgeIp(netBridgeIp);
    } else if (netMode == "bridge") {
        cfg.setNetworkDevice(false);         // ★ 必须 false，否则被强制成 NAT
        cfg.setNetMode(0);                   // 0 = 桥接（VmNetProperties::IsBridgeMode 判定 mode==0）
        if (netNic.empty()) {
            fprintf(stderr, "桥接模式需要 --nic <宿主物理网卡名>（例如 --nic eth0）\n");
            return 2;
        }
        cfg.setNicName(netNic);
        if (!netBridgeIp.empty()) cfg.setBridgeIp(netBridgeIp);
    } else if (!netMode.empty() && netMode != "none") {
        fprintf(stderr, "未知的 --net 取值: %s（可用 nat / bridge / none）\n", netMode.c_str());
        return 2;
    }
    if (netProxySync) cfg.setProxyAutoSyncEnabled(true);
    if (netDnsSync) cfg.setDnsAutoSyncEnabled(true);
    if (netHostSync) cfg.setHostNetworkSyncFeatureEnabled(true);
    if (netShare) cfg.setNetworkShareSupported(true);


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
        // 导出虚拟机磁盘。参数语义（实测）：
        //   --src = 目标【目录】（服务端 realpath 且要求是目录）
        //   --dst = 目标【文件名】（纯文件名，服务端自己拼 目录/文件名）
        if (name.empty() || bios.empty() || enhance.empty()) {
            fprintf(stderr,
                    "用法: hvm-cli export --name <vm> --src <目标目录> --dst <目标文件名>\n"
                    "      （--src 与 --bios 同义，--dst 与 --enhance 同义；\n"
                    "        目录必须存在，文件名服务端会拼成 目录/文件名）\n");
            return 2;
        }
        hvm::MigrationOptionsBuilder opts;
        if (!opts.ok()) return fail("vm export", -1, "构造 MigrationOptions 失败: " + opts.lastError());
        int rc = c.exportVmDiskImage(name, bios, enhance, false, opts.raw());
        if (rc != 0)
            return fail("vm export", rc,
                        std::string("ExportVmDiskImage 返回: ") + ohos_vm_error_name(rc) + " (" +
                            std::to_string(rc) + ")");
        printf("已提交导出：%s -> %s\n", bios.c_str(), enhance.c_str());
        return 0;
    }

    if (act == "import") {
        // 导入磁盘（服务端把源镜像拷成该虚拟机的磁盘）。实测语义：
        //   --src = 源镜像的【完整文件路径】（服务端会对它做 realpath）
        //   --dst = 该文件的 SHA-256，**可选**；不填就由本工具现算（多线程预读）
        // 服务端逐字节比较摘要且要求【大写】，所以这里统一替用户转成大写。
        // 另：目标虚拟机名必须【尚不存在】（导入这个动作本身就会建机）。
        if (name.empty() || bios.empty()) {
            fprintf(stderr,
                    "用法: hvm-cli import --name <新虚拟机名> --src <源镜像文件> [--dst <sha256>]\n"
                    "      --dst 可省略：省略时由本工具计算源文件的 SHA-256（多线程预读）\n");
            return 2;
        }

        std::string digest = enhance;
        if (digest.empty()) {
            // 注意：要读【用户给的原始路径】—— bios 已被换成服务端视图，本机读不到
            const std::string local = biosRaw.empty() ? bios : biosRaw;
            fprintf(stderr, "正在计算 %s 的 SHA-256%s ...\n", local.c_str(),
                    hvm::sha256HwAccelAvailable() ? "（硬件加速）" : "");
            std::string shaErr;
            if (hvm::sha256File(local, digest, 0, importProgress, &shaErr) != 0) {
                digest.clear();
                if (local != bios) {   // 退回服务端视图再试一次（个别情况下本机也能读）
                    shaErr.clear();
                    if (hvm::sha256File(bios, digest, 0, importProgress, &shaErr) != 0)
                        digest.clear();
                }
            }
            if (digest.empty())
                return fail("vm import", -1,
                            "无法读取源镜像来算摘要：" + shaErr +
                                "\n提示：把镜像放到用户下载目录（本机可读），或用 --dst 自行提供摘要");
            fprintf(stderr, "SHA-256 = %s\n", digest.c_str());
        } else {
            const std::string upper = hvm::Sha256::normalizeHexUpper(digest);
            if (upper != digest) fprintf(stderr, "（--dst 已自动转为大写）\n");
            digest = upper;
            if (digest.size() != 64)
                fprintf(stderr, "警告：--dst 长度为 %zu（应为 64），服务端可能判为镜像损坏\n",
                        digest.size());
        }

        hvm::MigrationOptionsBuilder opts;
        if (!opts.ok()) return fail("vm import", -1, "构造 MigrationOptions 失败: " + opts.lastError());
        opts.setKeepSnapshots(keepSnapshots);
        opts.setForceImport(forceImport);
        if (!password.empty()) opts.setPassword(password);
        const int rc = c.importVmDiskImage(name, bios, digest, opts.raw());
        if (rc != 0)
            return fail("vm import", rc,
                        std::string("ImportVmDiskImage 返回: ") + ohos_vm_error_name(rc) + " (" +
                            std::to_string(rc) + ")");
        printf("已提交导入：%s → 虚拟机 %s\n", bios.c_str(), name.c_str());
        // 导入即建机，同样登记进本地清单（服务端没有枚举接口，见文件头说明）
        if (!registryAdd(name))
            fprintf(stderr, "提示: 虚拟机已导入，但清单写入失败（%s）\n", kRegistryPath);
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
        // 结束后确认一次：销毁是否真的生效，看服务端还有没有这台虚拟机的磁盘
        const bool gone = !vmExists(c, name);
        if (g_json) {
            Json j("vm destroy");
            j.str("name", name).boolean("gone", gone);
            printf("%s\n", j.ok().c_str());
        } else if (gone) {
            printf("已销毁 %s ✓（已确认服务端不再有这台虚拟机）\n", name.c_str());
        } else {
            printf("已销毁 %s（服务端仍能查到它的磁盘，可能稍后才清完）\n", name.c_str());
        }
        // 自己维护清单：从记录里移除（失败只提示，不影响销毁结果）
        if (!registryRemove(name))
            fprintf(stderr, "提示: 虚拟机已销毁，但清单更新失败（%s）\n", kRegistryPath);
        return 0;
    }

    if (act == "create" || act == "start") {
        if (name.empty()) { fprintf(stderr, "%s 需要 --name <虚拟机名>\n", act.c_str()); return 2; }
        if (act == "create" && image.empty()) {
            fprintf(stderr, "create 需要 --image <镜像路径>（服务端 CreateVm 的第 2 个字符串）\n");
            return 2;
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
        if (act == "create") {
            // 自己维护清单：服务端没有枚举接口，见文件头说明
            if (!registryAdd(name))
                fprintf(stderr, "提示: 虚拟机已创建，但清单写入失败（%s）\n", kRegistryPath);
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

    if (cmd == "sha256") {
        // 计算文件 SHA-256（内置实现，多线程预读）。用法：
        //   hvm-cli sha256 <文件> [线程数]     线程数省略时自动
        if (a.pos.empty()) {
            fprintf(stderr, "用法: hvm-cli sha256 <文件> [线程数]\n");
            return 2;
        }
        const std::string path = a.pos[0];
        const int threads = a.pos.size() > 1 ? std::atoi(a.pos[1].c_str()) : 0;
        std::string hex, err;
        const auto t0 = std::chrono::steady_clock::now();
        const int rc = hvm::sha256File(path, hex, threads, importProgress, &err);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        if (rc != 0) return fail(cmd, -1, err);
        if (g_json) {
            Json j(cmd);
            j.str("file", path).str("sha256", hex).num("threads", threads).num("ms", ms);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%s\n", hex.c_str());
            fprintf(stderr, "（%lld ms%s）\n", static_cast<long long>(ms),
                    hvm::sha256HwAccelAvailable() ? "，硬件加速可用" : "");
        }
        return 0;
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
        std::vector<std::string> names;
        if (a.pos.empty()) {
            // 探测名单里**不存在的不显示**：否则用户会看到一串自己从没建过的
            // 名字（如 virtualized_linux）被标成"已失效"而困惑。
            // 显式点名（vms <名字>）时不做过滤 —— 那是用户自己要看的。
            for (const char *p : kProbeVmNames)
                if (vmExists(c, p)) names.push_back(p);
        } else {
            names = a.pos;
        }
        int rc = printVmTable(c, "vms", names);
        if (!g_json)
            printf("\n注: vm_manager 未提供枚举接口，此表按已知名字探测得出。\n");
        return rc;
    }
    if (cmd == "vmlog") {
        bool follow = false;
        for (const auto &x : a.pos) if (x == "-f" || x == "--follow") follow = true;
        std::ifstream in(kVmLogPath);
        if (!in) return fail(cmd, -1, std::string("打不开 ") + kVmLogPath);
        bool inGuest = false;
        if (!follow) {                       // 一次性：全部串口日志
            emitVmLog(in, &inGuest, nullptr);
            return 0;
        }
        // 跟随：先给最近若干条，再持续输出新增内容
        std::vector<std::string> kept;
        std::string line;
        while (std::getline(in, line)) {
            if (isSerialLine(line)) { kept.push_back(stripLogPrefix(line)); inGuest = true; }
            else if (!isLogPrefixed(line)) { if (inGuest) kept.push_back(line); }
            else inGuest = false;
        }
        const std::size_t show = kept.size() > 20 ? kept.size() - 20 : 0;
        for (std::size_t i = show; i < kept.size(); ++i) printf("%s\n", kept[i].c_str());
        fflush(stdout);
        std::streampos pos = in.tellg();
        for (;;) {                            // 轮询新增内容
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::ifstream more(kVmLogPath);
            if (!more) continue;
            more.seekg(pos);
            if (!more) continue;
            emitVmLog(more, &inGuest, nullptr);
            fflush(stdout);
            pos = more.tellg();
            if (pos == std::streampos(-1)) pos = 0;
        }
        return 0;
    }
    if (cmd == "list") {
        // 枚举我们自己记录的虚拟机清单（vm create 时追加、vm destroy 时移除）
        std::vector<std::string> names = registryLoad();
        if (names.empty()) {
            if (g_json) {
                Json j("list");
                j.num("count", 0).str("registry", kRegistryPath);
                printf("%s\n", j.ok().c_str());
            } else {
                printf("清单为空（%s）\n", kRegistryPath);
                printf("新建或导入虚拟机后会自动记录。例如：\n");
                printf("  # 从 ISO 全新安装。--net nat 必须给，否则建出来的虚拟机没有网卡；\n");
                printf("  #   --enhance 也不能与 --image 指向同一个文件。\n");
                printf("  ./hvm-cli create --name myvm \\\n");
                printf("      --image   /storage/Users/currentUser/Download/debian-12-unattended-arm64.iso \\\n");
                printf("      --enhance /storage/Users/currentUser/Download/oetool.iso \\\n");
                printf("      --bios    /system/opt/virt_service/virtualized_hwf/stratovirt-uefi \\\n");
                printf("      --cpu 6 --mem 8 --disk-gb 128 --net nat\n");
                printf("  # 导入现成的 qcow2（镜像放下载目录即可；名字需尚不存在）\n");
                printf("  #   ！！导入出来的虚拟机【没有网络】：框架的网络配置是宿主侧按 CfgInfo 分配的，\n");
                printf("  #   而 import 这条路径不带 CfgInfo，所以 net ip 与网络模式切换一律返回 405。\n");
                printf("  ./hvm-cli import --name myvm --src /storage/Users/currentUser/Download/debian12.qcow2\n");
                printf("  ./hvm-cli start  --name myvm --cpu 6 --mem 6\n");
            }
            return 0;
        }
        int rc = printVmTable(c, "list", names);
        if (!g_json)
            printf("\n清单文件: %s（%zu 台）\n", kRegistryPath, names.size());
        return rc;
    }
    if (cmd == "vminfo") {
        // 实测：GetVmInfo 返回的是【GB】（给 6GB 的虚拟机返回 6、给 8GB 返回 8），
        // 不是 MB —— 早先按 MB 打印是错的。
        uint32_t ddrGb = 0, pid = 0;
        int rc = c.getVmInfo(ddrGb, pid);
        if (rc != 0) return fail(cmd, rc, "GetVmInfo 失败（可能没有当前虚拟机）");
        if (g_json) {
            Json j(cmd);
            j.num("ddrSizeGb", ddrGb).num("vmPid", pid);
            printf("%s\n", j.ok().c_str());
        } else {
            printRow("DDR 大小", std::to_string(ddrGb) + " GB");
            printRow("虚拟机 PID", std::to_string(pid));
        }
        return 0;
    }
    if (cmd == "stratovirt-mem") {
        int64_t mem = 0;
        int rc = c.stratovirtMem(mem);
        if (rc != 0) return fail(cmd, rc, "GetStratovirtMem 失败");
        if (g_json) {
            Json j(cmd);
            j.num("kb", mem);
            printf("%s\n", j.ok().c_str());
        } else {
            // 实测单位是【KB】不是字节：与引擎进程的 VmRSS(kB) 同步采样，
            // 两者量级一致且同向变化（224658/377828、701566/810664，比值 0.6~0.87）；
            // 若是字节，比值会是 0.001 量级。
            printf("%lld KB (%.1f MiB)\n", static_cast<long long>(mem),
                   static_cast<double>(mem) / 1024.0);
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

    // 虚拟机操作直接是顶层命令（hvm-cli 本身即 VM 工具，不再加 vm 前缀）
    {
        static const char *kVmActs[] = {"create", "start", "destroy", "mount-cd", "unmount-cd",
                                        "range", "ctor", "view-state", "displays",
                                        "serial-read", "serial-write", "import", "export"};
        for (const char *act : kVmActs) {
            if (cmd == act) {
                std::vector<std::string> pos;
                pos.push_back(cmd);
                pos.insert(pos.end(), a.pos.begin(), a.pos.end());
                return cmdVm(c, pos);
            }
        }
    }
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
    if (cmd == "vmstat") {
        if (a.pos.empty() && a.vm.empty()) {
            // 无参 = 列出所有已知虚拟机的状态（与 list / vms 同一张表，
            // 顺带会标出哪些条目在服务端已不存在）
            std::vector<std::string> names = registryLoad();
            // 当前虚拟机也一并探测（它可能不在清单里，比如导入后又手工删过名字）
            std::string active;
            if (c.activeVmName(active) == 0 && !active.empty() &&
                std::find(names.begin(), names.end(), active) == names.end())
                names.push_back(active);
            // 框架自带的名字只在**确实存在**时才列；不存在就不显示
            for (const char *p : kProbeVmNames) {
                if (!vmExists(c, p)) continue;
                if (std::find(names.begin(), names.end(), p) == names.end()) names.push_back(p);
            }
            std::sort(names.begin(), names.end());
            return printVmTable(c, "vmstat", names);
        }
        int st = 0;
        std::string vm = resolveVm(c, a, "vmstat", /*positional=*/true, /*write=*/false);
        if (vm.empty()) return 2;
        int rc = c.vmStatus(vm, st);
        if (rc != 0) return fail(cmd, rc, "GetVmStatus 失败");
        // 状态码 0 分不清"已停止"和"已销毁"，再查一次磁盘才能给出确定答案
        const bool exists = (st != 0) || vmExists(c, vm);
        if (g_json) {
            Json j(cmd);
            j.str("vm", vm).num("status", st).boolean("exists", exists);
            printf("%s\n", j.ok().c_str());
        } else if (!exists) {
            printf("%s: 已不存在（已销毁）\n", vm.c_str());
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
        // 迁移相关的"哈希名"（服务端 VmAssistantManager 的成员，由迁移流程填充）。
        // 早先曾解析成 VmManagerClientWrapper::GetHashName 而在库内空指针崩溃，
        // 改为 VmManagerProxy::GetHashName（与其它 87 个方法一致）后实测可正常调用；
        // 未发起过迁移时返回空串。
        std::string v;
        int rc = c.hashName(v);
        if (rc != 0) return fail(cmd, rc, "GetHashName 失败");
        if (g_json) {
            Json j(cmd);
            j.str("hashName", v.empty() ? "(空：尚未发起过迁移)" : v);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%s\n", v.empty() ? "(空：尚未发起过迁移)" : v.c_str());
        }
        return 0;
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
    if (cmd == "stop") {
        // 服务端 StopVm(name, bool)：正常关机（force-stop 是 ForceStopVm 强制关机）
        std::string vm = resolveVm(c, a, "stop", /*positional=*/true, /*write=*/true);
        if (vm.empty()) return 2;
        bool clean = false;
        for (std::size_t i = 1; i < a.pos.size(); ++i) {
            if (a.pos[i] == "--clean") clean = true;
        }
        int rc = c.stopVm(vm, clean);
        if (rc != 0) return fail(cmd, rc, "StopVm 失败");
        if (g_json) {
            Json j(cmd);
            j.str("vm", vm).boolean("clean", clean);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("已请求关机 %s%s\n", vm.c_str(), clean ? "（clean）" : "");
        }
        return 0;
    }
    if (cmd == "force-stop") {
        std::string vm = resolveVm(c, a, "force-stop", /*positional=*/true, /*write=*/true);
        if (vm.empty()) return 2;
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
        std::string vm = resolveVm(c, a, "snapshot", /*positional=*/false, /*write=*/act != "list");
        if (vm.empty()) return 2;
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
            const std::string vm = resolveVm(c, a, "share setup", false, true);
            if (vm.empty()) return 2;
            int rc = c.setupSharedFolder(vm);
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
            const std::string vm = resolveVm(c, a, "share add", false, true);
            if (vm.empty()) return 2;
            int rc = c.addSharedFolder(vm, a.pos[1], a.pos[2]);
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
            const std::string vm = resolveVm(c, a, "share remove", false, true);
            if (vm.empty()) return 2;
            int rc = c.removeSharedFolder(vm, a.pos[1]);
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
            const std::string vm = resolveVm(c, a, "net " + act, false, /*write=*/false);
            if (vm.empty()) return 2;
            std::vector<std::array<std::uint32_t, 3>> v;
            int rc = (act == "ports") ? c.getPortForwardForNat(vm, v)
                                      : c.getLocalhostForwardFromVmToHost(vm, v);
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
            const std::string vm = resolveVm(c, a, "net mode", false, true);
            if (vm.empty()) return 2;
            int rc = c.setVmNetMode(vm, mode, iface);
            if (rc != 0) return fail(cmd, rc, "SetVmNetMode 失败");
            printf("已设置网络模式: %s\n", mode == 1 ? "NAT" : "桥接");
            return 0;
        }
        if (act == "proxy-status-on" || act == "proxy-status-off") {
            const std::string vm = resolveVm(c, a, "net " + act, false, true);
            if (vm.empty()) return 2;
            int rc = c.setVmHostNetProxyStatus(vm, act == "proxy-status-on");
            if (rc != 0) return fail(cmd, rc, "SetVmHostNetProxyStatus 失败");
            printf("已设置宿主网络代理状态: %s\n", act == "proxy-status-on" ? "开" : "关");
            return 0;
        }
        if (act == "proxy-auto-on" || act == "proxy-auto-off") {
            const std::string vm = resolveVm(c, a, "net " + act, false, true);
            if (vm.empty()) return 2;
            int rc = c.setProxyAutoSyncEnabled(vm, act == "proxy-auto-on");
            if (rc != 0) return fail(cmd, rc, "SetProxyAutoSyncEnabled 失败");
            printf("已设置代理自动同步: %s\n", act == "proxy-auto-on" ? "开" : "关");
            return 0;
        }
        if (act == "ip") {
            const std::string vm = resolveVm(c, a, "net ip", false, /*write=*/false);
            if (vm.empty()) return 2;
            std::string ip;
            int rc = c.vmIpv4Address(vm, ip);
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
            const std::string vm = resolveVm(c, a, "net proxy", false, /*write=*/false);
            if (vm.empty()) return 2;
            bool on = false;
            int rc = c.hostNetProxyStatus(vm, on);
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
        const std::string vm = resolveVm(c, a, "net " + act, false, true);
        if (vm.empty()) return 2;
        int rc = (act == "dns-on" || act == "dns-off") ? c.setDnsAutoSync(vm, on)
                                                       : c.switchNetworkShare(vm, on);
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
            const std::string vm = resolveVm(c, a, "disk " + act, false, /*write=*/false);
            if (vm.empty()) return 2;
            int64_t bytes = 0;
            int rc = (act == "capacity") ? c.diskCapacity(vm, bytes)
                                         : c.diskImageFileSize(vm, bytes);
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
            const std::string vm = resolveVm(c, a, "disk path", false, /*write=*/false);
            if (vm.empty()) return 2;
            std::string p;
            int rc = c.diskImagePath(vm, p);
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
            const std::string vm = resolveVm(c, a, "disk expand", false, true);
            if (vm.empty()) return 2;
            int rc = c.expandCapacity(vm, std::atoi(a.pos[1].c_str()));
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
