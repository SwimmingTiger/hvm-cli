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

BUILD_DIR=$(dirname "$0")/../build
mkdir -p "$BUILD_DIR"
LOG="$BUILD_DIR/hwdbg.log"

setsid nohup "$SERVER" gdbserver --log-file="$LOG" "127.0.0.1:$PORT" -- "$PROG" \
    >"$BUILD_DIR/hwdbg.out" 2>&1 </dev/null &
SRV_PID=$!
trap 'kill $SRV_PID 2>/dev/null || true' EXIT

sleep 2
exec lldb -o "gdb-remote 127.0.0.1:$PORT" "$@"
