// main.cpp —— openeuler 命令行入口
//
// 控制华为「融合开发引擎」（内部代号 RGM / LinuxFusion）里的 openEuler 环境：
// 通过系统自带 NDK 库 /system/lib64/ndk/libfusion_pty_ndk.so 建立
// virtio-vsock PTY 会话，在 openEuler 里执行命令。
//
// 定位：只做数据面。行规程（行编辑/回显/历史/补全/Ctrl-C）由远端 bash 负责。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "fusion_pty.h"

namespace {

bool g_json = false;

// ------------------------------------------------------------------ 输出
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

int fail(const std::string &cmd, int rc, const std::string &msg) {
    if (g_json) {
        printf("{\"ok\":false,\"cmd\":\"%s\",\"rc\":%d,\"error\":\"%s\"}\n", cmd.c_str(), rc,
               jsonEscape(msg).c_str());
    } else {
        fprintf(stderr, "错误: %s (rc=%d)\n", msg.c_str(), rc);
    }
    return 1;
}

void okJson(const std::string &cmd, const std::string &body) {
    printf("{\"ok\":true,\"cmd\":\"%s\",\"data\":{%s}}\n", cmd.c_str(), body.c_str());
}

//: 去掉 ANSI 转义序列
std::string stripAnsi(const std::string &in) {
    std::string out;
    for (size_t i = 0; i < in.size();) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == 0x1B) {
            if (i + 1 < in.size() && in[i + 1] == '[') {
                i += 2;
                while (i < in.size() && !(in[i] >= '@' && in[i] <= '~')) ++i;
                if (i < in.size()) ++i;
                continue;
            }
            if (i + 1 < in.size() && in[i + 1] == ']') {
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

//: 本地终端窗口尺寸
hvm::PtyWinSize localWinSize() {
    hvm::PtyWinSize ws;
    struct winsize w {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row != 0) {
        ws.rows = w.ws_row;
        ws.cols = w.ws_col;
        ws.xpixel = w.ws_xpixel;
        ws.ypixel = w.ws_ypixel;
    }
    return ws;
}

volatile sig_atomic_t g_winch = 0;
extern "C" void onWinch(int) { g_winch = 1; }

// ------------------------------------------------------------------ 帮助
void usage() {
    printf(
        "openeuler —— 融合开发引擎（RGM / LinuxFusion）openEuler 环境控制\n"
        "\n"
        "用法: openeuler [--json] <命令> [参数...]\n"
        "\n"
        "  shell                    连入 openEuler 交互式 shell（纯字节透传，Ctrl-D 退出）\n"
        "  exec <命令...>           在 openEuler 里执行命令并输出（退出码透传）\n"
        "  status                   通道与环境概览\n"
        "  selftest                 客户端通道自检\n"
        "  image install            安装/更新 openEuler 镜像\n"
        "  share status             查询共享目录开关\n"
        "  share enable|disable     打开/关闭共享目录\n"
        "  help                     显示本帮助\n"
        "\n"
        "运行环境:\n"
        "  必须在系统自带的 HiShell 终端中运行 —— 融合开发引擎的终端通道同样受\n"
        "  调用者身份限制，第三方应用的内置终端会被拒绝。\n"
        "\n"
        "全局选项:\n"
        "  --json                   以 JSON 输出（便于脚本调用）\n"
        "\n"
        "环境变量:\n"
        "  HVM_DEBUG=1              打印传输层调试信息\n"
        "  HVM_EOF_WAIT=<秒>        shell 在 stdin 结束后继续抽取输出的等待时间（默认 10）\n");
}

// ------------------------------------------------------------------ shell
//: 纯字节透传：本地 tty 切 raw，双向搬运；行规程完全交给远端 bash/readline。
int cmdShell() {
    hvm::PtyConfig pcfg;
    pcfg.winSize = localWinSize();
    hvm::PtySession pty;
    int rc = pty.open(pcfg, [](const char *d) {
        size_t n = strlen(d), off = 0;
        while (off < n) {
            ssize_t w = write(STDOUT_FILENO, d + off, n - off);
            if (w <= 0) break;
            off += static_cast<size_t>(w);
        }
    });
    if (rc != 0) return fail("shell", rc, "打开 PTY 会话失败: " + pty.lastError());

    struct termios oldt {}, rawt {};
    bool rawOk = tcgetattr(STDIN_FILENO, &oldt) == 0;
    if (rawOk) {
        rawt = oldt;
        cfmakeraw(&rawt);
        tcsetattr(STDIN_FILENO, TCSANOW, &rawt);
    }
    signal(SIGWINCH, onWinch);

    // 传输层就绪门：远端还没吐数据时发送会被静默丢弃
    if (!hvm::waitForAnyOutput(20000) && !hvm::sessionClosed()) {
        if (rawOk) tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        pty.close();
        return fail("shell", -1, "等待远端就绪超时");
    }
    if (g_json) {
        okJson("shell", "\"sessionId\":" + std::to_string(pty.sessionId()));
    }

    const bool dbg = getenv("HVM_DEBUG") != nullptr;
    const int eofWaitSec = [] {
        const char *v = getenv("HVM_EOF_WAIT");
        int s = v ? atoi(v) : 10;
        return s > 0 ? s : 10;
    }();

    bool stdinEof = false;
    auto deadline = std::chrono::steady_clock::now();
    std::vector<char> buf(4096);

    while (!hvm::sessionClosed()) {
        if (g_winch) {
            g_winch = 0;
            pty.setWinSize(localWinSize());
        }
        if (stdinEof) {
            if (std::chrono::steady_clock::now() >= deadline) break;
            usleep(100000);
            continue;
        }
        struct pollfd pfd {STDIN_FILENO, POLLIN, 0};
        int pr = poll(&pfd, 1, 200);
        if (pr < 0) break;
        if (pr == 0) continue;
        ssize_t n = read(STDIN_FILENO, buf.data(), buf.size());
        if (n <= 0) {
            stdinEof = true;
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(eofWaitSec);
            continue;
        }
        std::string chunk(buf.data(), static_cast<size_t>(n));
        for (int attempt = 0; attempt < 50; ++attempt) {
            int src = pty.send(chunk);
            if (dbg) fprintf(stderr, "[dbg] send %zu bytes attempt=%d rc=%d\n", chunk.size(),
                             attempt, src);
            if (src == 0) break;
            if (hvm::sessionClosed()) break;
            usleep(100000);
        }
    }
    if (rawOk) tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    signal(SIGWINCH, SIG_DFL);
    pty.close();
    return 0;
}

// ------------------------------------------------------------------ exec
int cmdExec(const std::vector<std::string> &args) {
    if (args.empty()) {
        fprintf(stderr, "用法: openeuler exec <命令...>\n");
        return 2;
    }
    std::string command;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) command += " ";
        command += args[i];
    }

    hvm::PtyConfig pcfg;
    pcfg.winSize = {40, 120, 0, 0};
    hvm::PtySession pty;
    std::string out;
    int rc = pty.open(pcfg, [&out](const char *d) { out += d; });
    if (rc != 0) return fail("exec", rc, "打开 PTY 会话失败: " + pty.lastError());

    hvm::waitForAnyOutput(20000) || hvm::waitForOutput("$ ", 1000) ||
        hvm::waitForOutput("# ", 1000);

    // 标记在 shell 侧拼出来：命令回显里不会出现与输出相同的字面量，
    // 否则等待结束标记会立刻命中回显、误判命令已结束。
    const std::string m = "__OE_END";
    const std::string startMarker = m + "_START";
    const std::string endMarker = m + "__";
    std::string line = "M=" + m + "; echo ${M}_START; " + command + "; echo ${M}__$?\n";
    int src = pty.send(line);
    if (src != 0) {
        pty.close();
        return fail("exec", src, "发送命令失败");
    }
    bool finished = hvm::waitForOutput(endMarker.c_str(), 60000);
    if (!finished && !hvm::sessionClosed()) {
        pty.close();
        return fail("exec", -1, "等待命令结束超时");
    }
    if (!finished && hvm::sessionClosed()) {
        // 命令把 shell 结束了（如 exit N）；库不转发退出码，只能报告"已结束"
        pty.close();
        if (g_json) {
            okJson("exec", "\"command\":\"" + jsonEscape(command) +
                               "\",\"stdout\":\"\",\"exitCode\":-1");
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
    while (!body.empty() && (body.front() == '\r' || body.front() == '\n')) body.erase(0, 1);
    while (!body.empty() &&
           (body.back() == '\r' || body.back() == '\n' || body.back() == ' '))
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
        okJson("exec", "\"command\":\"" + jsonEscape(command) + "\",\"stdout\":\"" +
                           jsonEscape(body) + "\",\"exitCode\":" + std::to_string(exitCode));
        return 0;
    }
    fputs(body.c_str(), stdout);
    if (!body.empty() && body.back() != '\n') fputs("\n", stdout);
    return exitCode == 0 ? 0 : (exitCode > 0 ? exitCode : 1);
}

// ------------------------------------------------------------------ 其余
int cmdSelftest() {
    std::string st = hvm::PtySession::selfTest();
    if (g_json) {
        okJson("selftest", "\"pty\":\"" + jsonEscape(st) + "\"");
    } else {
        printf("%s\n", st.c_str());
    }
    return 0;
}

int cmdStatus() {
    hvm::PtyConfig pcfg;
    hvm::PtySession pty;
    int rc = pty.open(pcfg, nullptr);
    bool on = false;
    int src = (rc == 0) ? pty.sharedFolderToggleState(on) : rc;
    std::string st = hvm::PtySession::selfTest();
    if (g_json) {
        okJson("status", "\"pty\":\"" + jsonEscape(st) + "\",\"sessionId\":" +
                             std::to_string(pty.sessionId()) + ",\"sharedFolderEnabled\":" +
                             (on ? "true" : "false") + ",\"rc\":" + std::to_string(src));
    } else {
        printf("通道           : %s\n", st.c_str());
        printf("会话 ID        : %d\n", pty.sessionId());
        printf("共享目录开关   : %s\n", on ? "开" : "关");
    }
    if (rc == 0) pty.close();
    return 0;
}

int cmdImageInstall() {
    hvm::PtySession pty;
    int rc = pty.open(hvm::PtyConfig{}, nullptr);
    if (rc != 0) return fail("image install", rc, "打开通道失败: " + pty.lastError());
    int irc = pty.installImage();
    pty.close();
    if (irc != 0) return fail("image install", irc, "InstallImage 失败");
    if (g_json) {
        okJson("image install", "\"installed\":true");
    } else {
        printf("已触发镜像安装/更新\n");
    }
    return 0;
}

int cmdShare(const std::vector<std::string> &args) {
    if (args.empty()) {
        fprintf(stderr, "用法: openeuler share status|enable|disable\n");
        return 2;
    }
    const std::string &act = args[0];
    hvm::PtySession pty;
    int rc = pty.open(hvm::PtyConfig{}, nullptr);
    if (rc != 0) return fail("share", rc, "打开通道失败: " + pty.lastError());
    int src = 0;
    if (act == "status") {
        bool on = false;
        src = pty.sharedFolderToggleState(on);
        if (src != 0) {
            pty.close();
            return fail("share status", src, "查询共享目录开关失败");
        }
        if (g_json) {
            okJson("share status", std::string("\"enabled\":") + (on ? "true" : "false"));
        } else {
            printf("%s\n", on ? "开" : "关");
        }
    } else if (act == "enable") {
        src = pty.enableShareFolder();
        if (src != 0) {
            pty.close();
            return fail("share enable", src, "EnableShareFolder 失败");
        }
        if (g_json) {
            okJson("share enable", "\"enabled\":true");
        } else {
            printf("共享目录已开启\n");
        }
    } else if (act == "disable") {
        // 逆向出的 NDK 只提供"开启"接口，未发现关闭接口
        pty.close();
        return fail("share disable", -1,
                    "该 NDK 未提供关闭接口（仅 OhPtyManagerEnableShareFolder）");
    } else {
        pty.close();
        return fail("share", 2, "未知动作: " + act);
    }
    pty.close();
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--json") {
            g_json = true;
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else {
            args.push_back(a);
        }
    }
    if (args.empty()) {
        usage();
        return 2;
    }
    const std::string cmd = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    if (cmd == "help") {
        usage();
        return 0;
    }
    if (cmd == "shell") return cmdShell();
    if (cmd == "exec") return cmdExec(rest);
    if (cmd == "status") return cmdStatus();
    if (cmd == "selftest") return cmdSelftest();
    if (cmd == "share") return cmdShare(rest);
    if (cmd == "image") {
        if (!rest.empty() && rest[0] == "install") return cmdImageInstall();
        fprintf(stderr, "用法: openeuler image install\n");
        return 2;
    }
    fprintf(stderr, "未知命令: %s\n\n", cmd.c_str());
    usage();
    return 2;
}
