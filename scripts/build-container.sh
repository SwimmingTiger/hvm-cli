#!/bin/bash
# 在 debian:12 容器里执行：装构建依赖 → 跑 squashfs ISO 构建
#
#   sudo podman run --rm --network host \
#       -v <工作目录>:/work debian:12 /work/build-container.sh
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# 压缩工具齐全，mksquashfs -comp 才能选 xz / gzip / zstd / lz4 / bzip2 等；
# zstd/xz 也用于 initrd（客户机里另有一份，见 build-deb12iso.sh）。
apt-get install -y -qq --no-install-recommends \
    debootstrap squashfs-tools xorriso grub-efi-arm64-bin \
    mtools dosfstools gdisk parted util-linux ca-certificates \
    zstd xz-utils bzip2 lz4
echo "=== 依赖就绪，开始构建 ==="
uname -m
chmod +x /work/build-deb12iso.sh
# ★ 工作目录必须用容器【内部】的 /build，不能用挂进来的 /work：
#   /work 是宿主机目录（nodev 挂载），debootstrap 会拒绝：
#   "Cannot install into target mounted with noexec or nodev"
# 产物 ISO 仍写到 /work（宿主能看到）。
/work/build-deb12iso.sh /work/deb12iso.iso /build
