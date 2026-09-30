#!/bin/bash
# ISO 落地后的「一次性审计」：在 openEuler 的容器里跑（不碰设备、不碰 x570）
#
# 用法（在 openEuler 上）：
#   sudo podman run --rm --network host \
#       -v "$HOME/iso-work":/work \
#       -v /mnt/linux_share/storage/Users/currentUser/Download:/iso:ro \
#       debian:12 bash /work/audit-iso.sh [ISO 文件名]
#
# 审计三件事：
#   1) efi.img：El Torito 指向的映像里，BOOTAA64.EFI 在不在、大小对不对、PE 头是不是 AArch64
#   2) squashfs：装好后系统要用的关键文件是否齐全（少一个都会让安装/启动失败）
#   3) initrd：live-boot 的脚本与 squashfs/isofs 模块在不在
set -uo pipefail
ISO_NAME="${1:-debian-12-unattended-arm64.iso}"
ISO="/iso/$ISO_NAME"
FAIL=0
ok()   { echo "  ✓ $*"; }
bad()  { echo "  ✗ $*"; FAIL=1; }

apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq squashfs-tools xorriso cpio gzip mtools >/dev/null 2>&1

echo "=== 0) ISO 基本信息 ==="
[ -f "$ISO" ] && ok "$ISO_NAME 存在（$(stat -c %s "$ISO") 字节）" || { bad "找不到 $ISO"; exit 1; }

echo "=== 1) efi.img（El Torito 引导映像）==="
# 从 ISO 取出 efi.img（xorriso 直接解，不需要 mount）
rm -rf /tmp/efi && mkdir -p /tmp/efi
xorriso -osirrox on -indev "$ISO" -extract /efi.img /tmp/efi.img >/dev/null 2>&1
if [ -f /tmp/efi.img ]; then
    ok "efi.img 存在（$(stat -c %s /tmp/efi.img) 字节）"
    # mtools 看内容（名称显示为 BOOTAA64 EFI）
    MDIR=$(mdir -i /tmp/efi.img ::/EFI/BOOT 2>&1 || true)
    SZ=$(echo "$MDIR" | awk '/BOOTAA64/{print $3}' | head -1)
    [ -n "$SZ" ] && ok "里面 BOOTAA64.EFI = $SZ 字节" || bad "里面没有 BOOTAA64.EFI（固件不会引导）: $MDIR"
    # PE 头：从 ISO 直接读更省事，这里从 efi.img 里搜
    python3 - "$SZ" <<'PY' 2>/dev/null || true
import sys
d=open('/tmp/efi.img','rb').read()
j=d.find(b'PE\x00\x00')
if j<0: print("  ✗ efi.img 里找不到 PE 头"); raise SystemExit(1)
import struct
mach=struct.unpack_from('<H',d,j+4)[0]; magic=struct.unpack_from('<H',d,j+24)[0]; sub=struct.unpack_from('<H',d,j+24+68)[0]
print(f"  {'✓' if mach==0xAA64 else '✗'} Machine=0x{mach:04x}; "
      f"{'✓' if magic==0x20b else '✗'} PE32+; {'✓' if sub==10 else '✗'} Subsystem={sub}(UEFI)")
PY
else
    bad "ISO 里没有 /efi.img"
fi

echo "=== 2) squashfs 内容审计（装好后要用的关键文件）==="
rm -rf /tmp/rd && mkdir -p /tmp/rd
xorriso -osirrox on -indev "$ISO" -extract /live/filesystem.squashfs /tmp/fs.sqfs >/dev/null 2>&1
if [ -f /tmp/fs.sqfs ]; then
    ok "filesystem.squashfs 存在（$(stat -c %s /tmp/fs.sqfs) 字节）"
    unsquashfs -l /tmp/fs.sqfs > /tmp/sq.list 2>/dev/null
    # 按【文件名】匹配，不猜目录：Debian 12 是 usr-merge，/sbin 只是符号链接，
    # 而且不同包的工具落在 /usr/bin 或 /usr/sbin 各不相同。
    # 格式："路径片段(正则)  说明"
    check_file() {   # $1=正则(匹配 squashfs -l 的整条路径)  $2=说明
        if grep -qE "$1" /tmp/sq.list; then ok "$2"; else bad "$2（$1）"; fi
    }
    check_file 'squashfs-root/(usr/)?sbin/init$'          "init（systemd-sysv 提供，缺了内核起来找不到它）"
    check_file 'squashfs-root/usr/bin/ssh-keygen$'        "ssh-keygen（Debian 在 /usr/bin）"
    check_file 'squashfs-root/etc/systemd/system/hvm-install.service$' "自动安装器 unit"
    check_file 'squashfs-root/usr/local/sbin/hvm-install-to-disk.sh$'  "安装器脚本"
    check_file 'squashfs-root/etc/systemd/network/20-wired.network$'   "静态 IP 配置"
    check_file 'squashfs-root/etc/systemd/system/ssh.service.d/10-host-keys.conf$' "sshd 主机密钥 drop-in"
    check_file '/unsquashfs$'                             "unsquashfs（安装器在客户机里要用它解 squashfs）"
    check_file '/sgdisk$'                                 "sgdisk（分区）"
    check_file '/mkfs\.vfat$'                            "mkfs.vfat（ESP）"
    check_file '/mkfs\.ext4$'                            "mkfs.ext4（根分区）"
    check_file '/partprobe$'                              "partprobe（重读分区表）"
    check_file '/grub-install$'                           "grub-install（装引导）"
    check_file '/update-grub$'                            "update-grub（生成 grub.cfg）"
    check_file '/live-boot$'                              "live-boot（把 squashfs 挂成根）"
    # 内核与 initrd 是否在 /boot 下
    K=$(grep -c "^squashfs-root/boot/vmlinuz-" /tmp/sq.list)
    I=$(grep -c "^squashfs-root/boot/initrd.img-" /tmp/sq.list)
    [ "$K" -ge 1 ] && ok "boot/ 下有内核（$K 个）" || bad "boot/ 下没有内核"
    [ "$I" -ge 1 ] && ok "boot/ 下有 initrd（$I 个）" || bad "boot/ 下没有 initrd"
else
    bad "ISO 里没有 /live/filesystem.squashfs"
fi

echo "=== 3) initrd 里的 live-boot 机制 ==="
xorriso -osirrox on -indev "$ISO" -extract /live/initrd.img /tmp/ird.img >/dev/null 2>&1
if [ -f /tmp/ird.img ]; then
    ok "initrd 存在（$(stat -c %s /tmp/ird.img) 字节）"
    mkdir -p /tmp/ird && cd /tmp/ird && zcat /tmp/ird.img 2>/dev/null | cpio -idm --quiet 2>/dev/null || true
    [ -e /tmp/ird/scripts/live ] && ok "scripts/live（live-boot 的引导脚本）" || bad "没有 scripts/live"
    [ -e /tmp/ird/usr/bin/live-boot ] && ok "usr/bin/live-boot" || bad "没有 live-boot"
    find /tmp/ird -name "squashfs.ko" | grep -q . && ok "squashfs.ko" || bad "initrd 里没有 squashfs.ko"
    find /tmp/ird -name "isofs.ko" | grep -q . && ok "isofs.ko" || bad "initrd 里没有 isofs.ko"
else
    bad "ISO 里没有 /live/initrd.img"
fi

echo
[ "$FAIL" = "0" ] && echo "===== 审计全部通过 ✓ 可以拿去 create =====" || echo "===== 审计有失败项 ✗ 先修再烧轮次 ====="
exit "$FAIL"
