#!/bin/bash
# 生成「最小 Debian 12 (bookworm) arm64」qcow2 —— 只含基础系统 + 内核 + GRUB + sshd
#
# 产物用途：导入鸿蒙 PC 的虚拟机服务（vm_manager）当虚拟机用。导入方式：
#   ./hvm-cli import --name <新虚拟机名> --src <这个 qcow2>     # 名字必须尚不存在
#   ./hvm-cli start  --name <新虚拟机名> --cpu 6 --mem 6
#
# 用法（需 root）：
#   sudo scripts/build-deb12min.sh [输出路径] [虚拟大小]
#   默认：/home/$USER/deb12min.qcow2  8G
# 可用环境变量覆盖：
#   MIRROR      apt 镜像（默认清华 http；https 在部分环境证书不全）
#   SUITE       Debian 版本代号（默认 bookworm = Debian 12）
#   ROOT_PASS   root 密码（默认 root —— 发布镜像请务必改掉）
#
# 设计要点（为什么这样做）：
#   * 分区：p1 = 512MiB ESP(FAT32, 类型 ef00)，p2 = 其余全部 ext4 根分区。
#     鸿蒙的 stratovirt 走 UEFI 引导，必须有 ESP，且 EFI/BOOT/BOOTAA64.EFI 要在。
#   * 内核参数：console=ttyAMA0,115200 提供串口（hvm-cli vmlog 读的就是它）；
#     modprobe.blacklist=vmwgfx **屏蔽** vmwgfx 模块（框架的显示不走它，
#     加载它反而有问题），并与 modprobe.d/blacklist-vmwgfx.conf 双保险。
#   * 网络：systemd-networkd + 通配 en*/eth* 的 DHCP，换机器/换网段都能自己起来。
#   * 发布卫生：清空 /etc/machine-id，删除预生成的 SSH 主机密钥，并加一个
#     ssh-keygen -A 的 drop-in，让每台实例首启生成**自己**的密钥。
#   * 只装 --no-install-recommends；装完 apt clean 并删掉 apt lists；
#     最后 fstrim 配合 qemu-nbd --discard=unmap 把已删文件的块真正还给 qcow2，
#     这样得到的才是尽量小的镜像。
set -euo pipefail

IMG="${1:-/home/${SUDO_USER:-$USER}/deb12min.qcow2}"
SIZE="${2:-8G}"
MIRROR="${MIRROR:-http://mirrors.tuna.tsinghua.edu.cn/debian}"
SUITE="${SUITE:-bookworm}"
ROOT_PASS="${ROOT_PASS:-root}"
NBD="${NBD:-/dev/nbd0}"
ROOT="/mnt/deb12min"
LOG() { printf '\n=== %s ===\n' "$*"; }

[ "$(uname -m)" = "aarch64" ] || { echo "此脚本面向 arm64（鸿蒙 PC 虚拟机是 aarch64），当前: $(uname -m)"; exit 1; }
[ "$(id -u)" = 0 ] || { echo "请用 root 运行（sudo $0）"; exit 1; }
for c in qemu-img qemu-nbd debootstrap sgdisk mkfs.vfat mkfs.ext4 partprobe blkid fstrim; do
    command -v "$c" >/dev/null || { echo "缺少命令: $c（Debian/Ubuntu: apt-get install qemu-utils debootstrap gdisk dosfstools parted util-linux）"; exit 1; }
done

#: 等待内核把分区节点建出来（nbd 上 partprobe 之后偶有延迟）
wait_part() {
    local dev="$1" i
    for i in $(seq 1 50); do
        [ -b "$dev" ] && return 0
        partprobe "$NBD" >/dev/null 2>&1 || true
        sleep 0.2
    done
    echo "分区节点未出现: $dev"; return 1
}

cleanup() {
    set +e
    umount "$ROOT/boot/efi" 2>/dev/null
    umount "$ROOT/dev/pts" "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" 2>/dev/null
    umount "$ROOT" 2>/dev/null
    qemu-nbd -d "$NBD" >/dev/null 2>&1
}
trap cleanup EXIT

LOG "0/8 准备磁盘镜像 $IMG（虚拟大小 $SIZE）"
modprobe nbd max_part=16
qemu-nbd -d "$NBD" >/dev/null 2>&1 || true
rm -f "$IMG"
qemu-img create -f qcow2 "$IMG" "$SIZE"
# --discard=unmap：让 guest 的 discard 请求真正把 qcow2 的块变成空洞（末尾 fstrim 用）
qemu-nbd --discard=unmap -c "$NBD" "$IMG"
wait_part "${NBD}"

LOG "1/8 分区 + 文件系统"
sgdisk --zap-all "$NBD" >/dev/null
sgdisk -n 1:2048:+512M -t 1:ef00 -c 1:ESP  "$NBD" >/dev/null
sgdisk -n 2:0:0       -t 2:8300 -c 2:root "$NBD" >/dev/null
wait_part "${NBD}p1"
wait_part "${NBD}p2"
mkfs.vfat -F 32 -n ESP  "${NBD}p1" >/dev/null
mkfs.ext4 -q -F -L root "${NBD}p2"

LOG "2/8 挂载并 debootstrap（$SUITE, minbase）"
mkdir -p "$ROOT"
mount "${NBD}p2" "$ROOT"
mkdir -p "$ROOT/boot/efi"
mount "${NBD}p1" "$ROOT/boot/efi"
debootstrap --arch=arm64 --variant=minbase \
    --components=main "$SUITE" "$ROOT" "$MIRROR"

LOG "3/8 进入 chroot 安装内核 / GRUB / sshd"
cp /etc/resolv.conf "$ROOT/etc/resolv.conf"
mount --bind /dev  "$ROOT/dev"
mount --bind /dev/pts "$ROOT/dev/pts"
mount -t proc proc "$ROOT/proc"
mount -t sysfs sys "$ROOT/sys"
cat > "$ROOT/etc/apt/sources.list" <<EOF
deb $MIRROR $SUITE main
deb $MIRROR $SUITE-updates main
deb http://security.debian.org/debian-security $SUITE-security main
EOF
chroot "$ROOT" /bin/bash -eux <<'CHROOT'
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# 只装必要组件：内核、EFI 引导、sshd，外加 ca-certificates/iproute2/guest-agent
apt-get install -y --no-install-recommends \
    linux-image-arm64 grub-efi-arm64 openssh-server \
    ca-certificates iproute2 qemu-guest-agent systemd-resolved
CHROOT

LOG "4/8 系统配置（主机名 / fstab / 网络 / 串口）"
ROOT_UUID=$(blkid -s UUID -o value "${NBD}p2")
ESP_UUID=$(blkid -s UUID -o value "${NBD}p1")
echo "deb12min" > "$ROOT/etc/hostname"
cat > "$ROOT/etc/hosts" <<'EOF'
127.0.0.1	localhost
127.0.1.1	deb12min
::1		localhost ip6-localhost ip6-loopback
EOF
cat > "$ROOT/etc/fstab" <<EOF
UUID=$ROOT_UUID	/	ext4	errors=remount-ro	0	1
UUID=$ESP_UUID	/boot/efi	vfat	umask=0077		0	1
EOF
cat > "$ROOT/etc/systemd/network/20-wired.network" <<'EOF'
[Match]
Name=en* eth*

[Network]
DHCP=yes
EOF
ln -sf /run/systemd/resolve/stub-resolv.conf "$ROOT/etc/resolv.conf"
chroot "$ROOT" /bin/bash -eux <<'CHROOT'
systemctl enable systemd-networkd systemd-resolved ssh qemu-guest-agent
systemctl enable serial-getty@ttyAMA0
# 首启为每台实例生成自己的 SSH 主机密钥（镜像里不预置任何密钥）
rm -f /etc/ssh/ssh_host_*
mkdir -p /etc/systemd/system/ssh.service.d
cat > /etc/systemd/system/ssh.service.d/10-host-keys.conf <<'EOF'
[Service]
ExecStartPre=/usr/sbin/ssh-keygen -A
EOF
# 发布卫生：清空机器标识（首启重新生成）
: > /etc/machine-id
rm -f /var/lib/dbus/machine-id
CHROOT

LOG "5/8 root 密码与 sshd 策略"
chroot "$ROOT" /bin/bash -eux <<CHROOT
echo 'root:$ROOT_PASS' | chpasswd
mkdir -p /etc/ssh/sshd_config.d
cat > /etc/ssh/sshd_config.d/10-hvm.conf <<'EOF'
PermitRootLogin yes
PasswordAuthentication yes
EOF
CHROOT

LOG "6/8 屏蔽 vmwgfx + 安装 GRUB"
cat > "$ROOT/etc/modprobe.d/blacklist-vmwgfx.conf" <<'EOF'
blacklist vmwgfx
EOF
CMDLINE='console=ttyAMA0,115200 modprobe.blacklist=vmwgfx'
sed -i "s|^GRUB_CMDLINE_LINUX_DEFAULT=.*|GRUB_CMDLINE_LINUX_DEFAULT=\"$CMDLINE\"|" "$ROOT/etc/default/grub"
grep -q '^GRUB_CMDLINE_LINUX_DEFAULT=' "$ROOT/etc/default/grub" || \
    echo "GRUB_CMDLINE_LINUX_DEFAULT=\"$CMDLINE\"" >> "$ROOT/etc/default/grub"
chroot "$ROOT" /bin/bash -eux <<'CHROOT'
grub-install --target=arm64-efi --efi-directory=/boot/efi --bootloader-id=debian --no-nvram
update-grub
# 可移除介质路径也放一份，兼容只看 EFI/BOOT/BOOTAA64.EFI 的固件
mkdir -p /boot/efi/EFI/BOOT
cp /boot/efi/EFI/debian/grubaa64.efi /boot/efi/EFI/BOOT/BOOTAA64.EFI
CHROOT

LOG "7/8 清理（发布卫生 + 压体积）"
chroot "$ROOT" /bin/bash -eux <<'CHROOT'
apt-get clean
rm -rf /var/lib/apt/lists/*
find /var/log -type f -delete
rm -rf /tmp/* /var/tmp/*
CHROOT

LOG "8/8 释放空洞并卸载"
# 被 apt 删掉的文件在 qcow2 里原本仍是"已分配"块；fstrim 把它们变成空洞，
# 配合连接时的 --discard=unmap，qcow2 会真正缩小 —— 这是**减小**体积的一步。
fstrim -v "$ROOT" || true
sync
umount "$ROOT/boot/efi" "$ROOT/dev/pts" "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT"
qemu-nbd -d "$NBD" >/dev/null
trap - EXIT

echo
echo "镜像: $IMG"
ls -l "$IMG"
echo "虚拟大小: $(qemu-img info --output=json "$IMG" | tr -d ' \n' | sed 's/.*"virtual-size":\([0-9]*\).*/\1/') 字节"
echo -n "SHA-256: "; sha256sum "$IMG" | cut -d' ' -f1
echo
echo "登录：root / ${ROOT_PASS}（ssh 与串口都可）"
echo "导入：./hvm-cli import --name <新虚拟机名> --src $IMG"
echo "      ./hvm-cli start  --name <新虚拟机名> --cpu 6 --mem 6"
