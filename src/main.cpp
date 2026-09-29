// main.cpp —— hvm-cli 命令行入口
//
// 纯 C++ 实现：直接使用系统自带的 libvm_manager_kits.z.so，
// 不需要 Python、不需要 root、不需要 HAP。
#include <cstdint>
#include <cstdio>
#include <termios.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "fusion_pty.h"
#include "hvm_client.h"

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
        "全局选项:\n"
        "  --json          以 JSON 输出（便于脚本调用）\n"
        "  --vm <名字>     目标虚拟机名，默认 virtualized_linux\n"
        "\n"
        "状态:\n"
        "  selftest                客户端 kit 加载自检\n"
        "  info                    汇总状态（能力/活动 VM/版本/共享目录）\n"
        "  exec <命令...>          在 openEuler 虚拟机里执行命令并打印输出\n"
        "  shell                   连入 openEuler 虚拟机交互式 shell（Ctrl-D 退出）\n"
        "  pty-selftest            fusion PTY 通道自检\n"
        "  vms [名字...]           列出/探测虚拟机（无枚举接口，按名字探测）\n"
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

//: 去掉 ANSI 转义序列，便于脚本消费
std::string stripAnsi(const std::string &in) {
    std::string out;
    for (size_t i = 0; i < in.size();) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == 0x1B) {  // ESC
            if (i + 1 < in.size() && in[i + 1] == '[') {
                i += 2;
                while (i < in.size() && !((in[i] >= '@' && in[i] <= '~'))) ++i;
                if (i < in.size()) ++i;
                continue;
            }
            if (i + 1 < in.size() && in[i + 1] == ']') {  // OSC ... BEL/ST
                i += 2;
                while (i < in.size() && in[i] != 0x07 && in[i] != 0x1B) ++i;
                if (i < in.size() && in[i] == 0x1B) ++i;
                if (i < in.size()) ++i;
                continue;
            }
            ++i;
            continue;
        }
        out += static_cast<char>(c);
        ++i;
    }
    return out;
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

    Args a = parse(argc, argv, 2);
    // 允许 `hvm-cli --json info` 这种写法：把选项后的第一条非选项当作命令
    if (cmd.rfind("--", 0) == 0) {
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

    // ------------------------------------------------------------ fusion PTY
    if (cmd == "pty-selftest") {
        std::string st = hvm::PtySession::selfTest();
        if (g_json) {
            Json j(cmd);
            j.str("pty", st);
            printf("%s\n", j.ok().c_str());
        } else {
            printf("%s\n", st.c_str());
        }
        return 0;
    }
    if (cmd == "exec" || cmd == "vm-exec") {
        if (a.pos.empty()) {
            fprintf(stderr, "用法: hvm-cli exec <命令...>\n");
            return 2;
        }
        std::string command;
        for (size_t i = 0; i < a.pos.size(); ++i) {
            if (i) command += " ";
            command += a.pos[i];
        }
        hvm::PtyConfig pcfg;
        pcfg.winSize = {40, 120, 0, 0};
        hvm::PtySession pty;
        std::string out;
        int rc = pty.open(pcfg, [&out](const char *d) { out += d; });
        if (rc != 0) return fail(cmd, rc, "打开 PTY 会话失败: " + pty.lastError());

        // 等 shell 提示符出现（新会话会先打欢迎信息）
        hvm::waitForOutput("$ ", 20000) || hvm::waitForOutput("# ", 1000);

        // 标记在 shell 侧拼出来，避免"命令回显"里出现与输出相同的字面量，
        // 否则等待结束标记会立刻命中回显、误判命令已结束。
        const std::string m = "__HVM_END";
        const std::string startMarker = m + "_START";
        const std::string endMarker = m + "__";
        std::string line = "M=" + m + "; echo ${M}_START; " + command +
                           "; echo ${M}__$?\n";
        int src = pty.send(line);
        if (src != 0) {
            pty.close();
            return fail(cmd, src, "发送命令失败");
        }
        bool finished = hvm::waitForOutput(endMarker.c_str(), 60000);
        if (!finished && !hvm::sessionClosed()) {
            pty.close();
            return fail(cmd, -1, "等待命令结束超时");
        }
        if (!finished && hvm::sessionClosed()) {
            // shell 被命令本身结束（如 exit N）。库不转发退出码，只能报告"已结束"。
            pty.close();
            if (g_json) {
                Json j(cmd);
                j.str("command", command).str("stdout", "").num("exitCode", -1);
                printf("%s\n", j.ok().c_str());
                return 0;
            }
            fprintf(stderr, "会话已被命令结束（收不到结束标记，退出码不可得）\n");
            return 1;
        }
        std::string plain = stripAnsi(out);
        size_t spos = plain.find(startMarker);
        size_t epos = plain.find(endMarker, spos == std::string::npos ? 0 : spos);
        std::string body;
        if (spos != std::string::npos && epos != std::string::npos) {
            body = plain.substr(spos + startMarker.size(), epos - spos - startMarker.size());
        } else {
            body = plain;
        }
        // 去掉标记行前后的 CR/LF 与提示符残留
        while (!body.empty() && (body.front() == '\r' || body.front() == '\n')) body.erase(0, 1);
        while (!body.empty() && (body.back() == '\r' || body.back() == '\n' || body.back() == ' '))
            body.pop_back();
        int exitCode = -1;
        if (epos != std::string::npos) {
            size_t p = epos + endMarker.size();
            std::string digits;
            while (p < plain.size() && plain[p] >= '0' && plain[p] <= '9') digits += plain[p++];
            if (!digits.empty()) exitCode = std::atoi(digits.c_str());
        }
        pty.close();
        if (g_json) {
            Json j(cmd);
            j.str("command", command).str("stdout", body).num("exitCode", exitCode);
            printf("%s\n", j.ok().c_str());
            return exitCode == 0 ? 0 : 0;  // JSON 模式下仍返回 0，exitCode 在字段里
        }
        fputs(body.c_str(), stdout);
        if (!body.empty() && body.back() != '\n') fputs("\n", stdout);
        return exitCode == 0 ? 0 : (exitCode > 0 ? exitCode : 1);
    }
    if (cmd == "shell" || cmd == "vm-shell") {
        hvm::PtyConfig pcfg;
        pcfg.winSize = {40, 120, 0, 0};
        hvm::PtySession pty;
        int rc = pty.open(pcfg, [](const char *d) {
            fputs(d, stdout);
            fflush(stdout);
        });
        if (rc != 0) return fail(cmd, rc, "打开 PTY 会话失败: " + pty.lastError());
        hvm::waitForOutput("$ ", 20000) || hvm::waitForOutput("# ", 1000);
        printf("[已连入 openEuler 环境，输入 exit 退出]\n");
        struct termios oldt {}, rawt {};
        bool rawOk = tcgetattr(STDIN_FILENO, &oldt) == 0;
        if (rawOk) {
            rawt = oldt;
            rawt.c_lflag &= ~(ICANON | ECHO);
            rawt.c_cc[VMIN] = 1;
            rawt.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &rawt);
        }
        std::string line;
        char ch = 0;
        while (true) {
            ssize_t n = read(STDIN_FILENO, &ch, 1);
            if (n <= 0) break;
            if (ch == '\n') {
                // 字符已逐个发送，这里只补回车，避免整行重复下发
                pty.send("\n");
                if (line == "exit" || line == "logout") break;
                line.clear();
            } else if (ch == 0x04) {  // Ctrl-D
                pty.send("exit\n");
                break;
            } else if (ch == 0x7F || ch == 0x08) {
                if (!line.empty()) line.pop_back();
                pty.send("\x7f");
            } else {
                line += ch;
                std::string k(1, ch);
                pty.send(k);
            }
        }
        if (rawOk) tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        pty.close();
        return 0;
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
        NEED_ARGS(1, "hvm-cli net ip|proxy|share-on|share-off|dns-on|dns-off");
        const std::string &act = a.pos[0];
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
