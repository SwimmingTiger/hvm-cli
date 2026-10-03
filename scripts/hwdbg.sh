#!/bin/sh
# hwdbg.sh —— 在鸿蒙 PC 沙箱内用 lldb 调试本机程序
#
# 背景： DevBox、Harmonybrew 和 OHOS-SDK 的 `lldb` / `lldb-server`
#       在当前身份下会 `ptrace failed: Permission denied`；
#       应用商店里的 CodeArts IDE（com.huawei.codearts）自带一个自包含的
#       huawei-debug-lldb-server，
#       可以正常拉起进程。它位于 CodeArts IDE 自己的沙箱里，
#       需要在 **CodeArts IDE 的终端**里把它拷出来：
#
#           mkdir -p ~/.local/bin
#           cp /data/storage/el2/base/files/huawei-debug-lldb-server ~/.local/bin/
#
#       拷到 ~/.local/bin 之后，在 HiShell 终端里即可使用（也可用
#       HVM_LLDB_SERVER 指向别处）。
#
# 用法:
#   scripts/hwdbg.sh ./hvm-cli [端口]                    # 启动并进入 lldb 交互
#   scripts/hwdbg.sh ./hvm-cli 7799 -o "b main" -o continue
#
# 给【被调试程序】传参：在 -- 之后写（与 `lldb -- prog args` 同惯例）：
#   scripts/hwdbg.sh ./hvm-cli 7799 -o "b main" -o continue -o bt -- \
#       share add --vm clean6 /path name
#   （-- 之前是 lldb 的参数，-- 之后全是被调试程序的参数）
#
# 也可用环境变量 PROG_ARGS 作为后备（两者同时给时，-- 之后的优先）。
# 注意：不要用 shell 包装脚本来传参 —— lldb-server 无法 execve 脚本，
#       会报 "failed to launch ...: execve failed: Operation not permitted"。
#
# 注意：进程已由 lldb-server 预先拉起并停在动态链接器入口，
#       所以下完断点要 `continue` 恢复，**不要用 `run`**（会重新拉起而冲突）。
#
# 常用 lldb 命令（连上之后）:
#   bt                       看调用栈
#   register read x0 x1 x2   看寄存器
#   image list               看已加载模块与基址（配合逆向出的静态偏移算地址）
#   br set -a <运行时地址>    在计算出的地址下断点
set -e

SERVER=${HVM_LLDB_SERVER:-$HOME/.local/bin/huawei-debug-lldb-server}
[ -x "$SERVER" ] || {
    echo "找不到可执行的 $SERVER" >&2
    echo "获取方式：打开应用商店里的 CodeArts IDE，在它的终端里执行" >&2
    echo "    mkdir -p ~/.local/bin" >&2
    echo "    cp /data/storage/el2/base/files/huawei-debug-lldb-server ~/.local/bin/" >&2
    echo "（也可用 HVM_LLDB_SERVER 指向其它位置）" >&2
    exit 1
}
[ $# -ge 1 ] || { echo "用法: $0 <可执行文件> [端口] [lldb 参数...]" >&2; exit 2; }

PROG=$1; shift
PORT=7799
case "$1" in
    [0-9]*) PORT=$1; shift ;;
esac

# 把 "$@" 在第一个 -- 处切开：  之前 → lldb 的参数；之后 → 被调试程序的参数。
# 注意：必须逐个参数做 shell 转义后再拼，否则 `-o "br set -r X"` 这种带空格的
#       参数会被拆散（实测会报 "Multiple possible REPL languages"）。
shq() {
    # 把单个参数转义成可安全放进 eval 的 shell 词
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\''/g")"
}
LLDB_ARGS=""
PROG_ARGS="${PROG_ARGS:-}"
seen_dd=0
if [ "$PROG_ARGS" != "" ]; then
    PROG_ARGS=$(shq "$PROG_ARGS")
fi
for arg in "$@"; do
    if [ "$seen_dd" = 0 ] && [ "$arg" = "--" ]; then
        seen_dd=1
        PROG_ARGS=""
        continue
    fi
    if [ "$seen_dd" = 1 ]; then
        PROG_ARGS="$PROG_ARGS $(shq "$arg")"
    else
        LLDB_ARGS="$LLDB_ARGS $(shq "$arg")"
    fi
done

BUILD_DIR=$(dirname "$0")/../build
mkdir -p "$BUILD_DIR"
LOG="$BUILD_DIR/hwdbg.log"

# shellcheck disable=SC2086
eval "setsid nohup \"\$SERVER\" gdbserver --log-file=\"\$LOG\" \"127.0.0.1:\$PORT\" -- \"\$PROG\" $PROG_ARGS" \
    >"$BUILD_DIR/hwdbg.out" 2>&1 </dev/null &
SRV_PID=$!
trap 'kill $SRV_PID 2>/dev/null || true' EXIT

sleep 2
# shellcheck disable=SC2086
eval "exec lldb -o \"gdb-remote 127.0.0.1:\$PORT\" $LLDB_ARGS"
