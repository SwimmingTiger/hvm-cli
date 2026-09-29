#!/bin/sh
# 仓库卫生检查：被 git 跟踪的文件里不允许出现二进制。
# 用法：make check-repo（或直接执行本脚本）
set -e
bad=0
for f in $(git ls-files); do
    enc=$(file -b --mime-encoding "$f" 2>/dev/null || echo text)
    case "$enc" in
        binary)
            echo "✗ 二进制文件被跟踪: $f"
            bad=1
            ;;
    esac
done
if [ "$bad" -eq 0 ]; then
    echo "✓ 仓库内没有二进制文件（共 $(git ls-files | wc -l) 个文件）"
fi
exit $bad
