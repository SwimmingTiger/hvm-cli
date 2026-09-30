#!/bin/bash
# 生成「最小 Debian 12 (bookworm) arm64」qcow2 —— 只含基础系统 + 内核 + GRUB + sshd
#
# 产物用途：导入鸿蒙 PC 的虚拟机服务（vm_manager）当虚拟机用。导入方式：
#   ./hvm-cli import --name <新虚拟机名> --src <这个 qcow2>     # 名字必须尚不存在
#   ./hvm-cli start  --name <新虚拟机名> --cpu 6 --mem 6
#
# 用法（需 root）：
#   sudo scripts/build-deb12min.sh [输出路径] [虚拟大小] [根文件系统类型]
#   默认：$HOME/deb12min.qcow2  8G  ext4
#   例：  sudo scripts/build-deb12min.sh out.qcow2 100G btrfs
#   根分区大小由脚本自己算：**ESP 之外的全部剩余空间**（不单独给 root 尺寸）。
# 可用环境变量覆盖：
#   MIRROR      apt 镜像（默认清华 http；https 在部分环境证书不全）
#   SUITE       Debian 版本代号（默认 bookworm = Debian 12）
#   ROOT_PASS   root 密码（默认 root —— 发布镜像请务必改掉）
#   VM_IP / VM_GW / VM_DNS   客户机静态网络（默认 172.16.100.2/24 / 172.16.100.1 / 114.114.114.114）
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

# 家目录一律用 $HOME：$USER 是"用户名"，不是家目录本身 —— 家目录不一定位于
# /home/<用户名>（也可能根本没设 $USER）。sudo 下 HOME 通常被保留；真为空时用 shell 的
# ~ 展开兜底（bash 内建，不依赖 getent —— HarmonyOS 上就没有 getent，实测踩到）。
[ -n "${HOME:-}" ] || HOME=$(eval echo ~)
IMG="${1:-$HOME/deb12min.qcow2}"
SIZE="${2:-8G}"
ROOT_FS="${3:-ext4}"
MIRROR="${MIRROR:-http://mirrors.tuna.tsinghua.edu.cn/debian}"
SUITE="${SUITE:-bookworm}"
ROOT_PASS="${ROOT_PASS:-root}"
NBD="${NBD:-/dev/nbd0}"
ROOT="${ROOT:-/mnt/deb12min}"    # 可覆盖：并跑多个构建时各自用不同挂载点
LOG() { printf '\n=== %s ===\n' "$*"; }

[ "$(uname -m)" = "aarch64" ] || { echo "此脚本面向 arm64（鸿蒙 PC 虚拟机是 aarch64），当前: $(uname -m)"; exit 1; }
[ "$(id -u)" = 0 ] || { echo "请用 root 运行（sudo $0）"; exit 1; }
case "$ROOT_FS" in
    ext4)  MKFS_OPTS=(-q -F -L root); FSTAB_OPTS="errors=remount-ro"; FSTAB_PASS=1; FS_PKGS="" ;;
    xfs)   MKFS_OPTS=(-f -L root);    FSTAB_OPTS="defaults";         FSTAB_PASS=0; FS_PKGS="xfsprogs" ;;
    btrfs) MKFS_OPTS=(-f -L root);    FSTAB_OPTS="defaults";         FSTAB_PASS=0; FS_PKGS="btrfs-progs" ;;
    *) echo "不支持的根文件系统: $ROOT_FS（可选 ext4 / xfs / btrfs）"; exit 1 ;;
esac

for c in qemu-img qemu-nbd debootstrap sgdisk mkfs.vfat "mkfs.$ROOT_FS" partprobe blkid fstrim; do
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
    # 用 umount -R 递归卸载；成功不了再用 -l（惰性）兜底。
    # 顺序 umount 在 dev/pts 上会 busy，别用。
    umount -R "$ROOT" 2>/dev/null || umount -R -l "$ROOT" 2>/dev/null
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
# p2 从剩余空间开头一直到最后：根分区自动用满 ESP 之外的**全部**空间
sgdisk -n 2:0:0       -t 2:8300 -c 2:root "$NBD" >/dev/null
wait_part "${NBD}p1"
wait_part "${NBD}p2"
mkfs.vfat -F 32 -n ESP  "${NBD}p1" >/dev/null
mkfs."$ROOT_FS" "${MKFS_OPTS[@]}" "${NBD}p2"
ROOT_BYTES=$(blockdev --getsize64 "${NBD}p2")
echo "根分区（$ROOT_FS）大小: $((ROOT_BYTES / 1024 / 1024)) MiB（= 整盘减去 512MiB ESP）"

LOG "2/8 挂载并 debootstrap（$SUITE, minbase）"
mkdir -p "$ROOT"
mount "${NBD}p2" "$ROOT"
mkdir -p "$ROOT/boot/efi"
mount "${NBD}p1" "$ROOT/boot/efi"
debootstrap --arch=arm64 --variant=minbase \
    --components=main "$SUITE" "$ROOT" "$MIRROR"

LOG "3/8 进入 chroot 安装内核 / GRUB / sshd"
cp /etc/resolv.conf "$ROOT/etc/resolv.conf"
# 绑定宿主的 /dev /proc /sys 时，必须先断开与宿主的**共享传播**：
#   * /dev 是 bind，用 --make-rslave（官方推荐做法，chroot 工具链都这么写）；
#   * /proc /sys 是新挂载，用 --make-private 即可。
# 不做这一步的后果实测过：收尾时 umount 会卡在
#   "umount: <root>/dev/pts: target is busy"
# 而且在某些内核上 chroot 里的挂载还会反向传染到宿主。
for d in dev dev/pts; do
    mkdir -p "$ROOT/$d"
    mount --bind "/$d" "$ROOT/$d"
    mount --make-rslave "$ROOT/$d"
done
mkdir -p "$ROOT/proc" "$ROOT/sys"
mount -t proc proc "$ROOT/proc"
mount --make-private "$ROOT/proc"
mount -t sysfs sys "$ROOT/sys"
mount --make-private "$ROOT/sys"
cat > "$ROOT/etc/apt/sources.list" <<EOF
deb $MIRROR $SUITE main
deb $MIRROR $SUITE-updates main
deb http://security.debian.org/debian-security $SUITE-security main
EOF
chroot "$ROOT" /bin/bash -eux <<CHROOT
export DEBIAN_FRONTEND=noninteractive
export FS_PKGS="$FS_PKGS"
apt-get update -qq
# 只装必要组件：内核、EFI 引导、sshd，外加 ca-certificates/iproute2/guest-agent
apt-get install -y --no-install-recommends \
    linux-image-arm64 grub-efi-arm64 openssh-server \
    ca-certificates iproute2 qemu-guest-agent systemd-resolved \
    systemd-sysv ${FS_PKGS}
# 说明：systemd-sysv 提供 /sbin/init → systemd；debootstrap 的 minbase 只装了
# systemd 本体，**没有** 这个符号链接，缺了它内核起来后会掉进 initramfs 的急救 shell。
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
UUID=$ROOT_UUID	/	$ROOT_FS	$FSTAB_OPTS	0	$FSTAB_PASS
UUID=$ESP_UUID	/boot/efi	vfat	umask=0077		0	1
EOF
# 网络：静态 IP（框架的虚拟网络是 172.16.100.0/24，网关 .1）。
# 可用 VM_IP / VM_GW / VM_DNS 覆盖；不设 DHCP —— 实测框架侧不保证有 DHCP。
cat > "$ROOT/etc/systemd/network/20-wired.network" <<EOF
[Match]
Name=en* eth*

[Network]
Address=${VM_IP:-172.16.100.2/24}
Gateway=${VM_GW:-172.16.100.1}
DNS=${VM_DNS:-114.114.114.114}
EOF
ln -sf /run/systemd/resolve/stub-resolv.conf "$ROOT/etc/resolv.conf"
# 关键：initramfs 要包含根文件系统的驱动。内核包安装时 fstab 还没写、当前根还是 nbd 上的
# 临时系统，initramfs-tools 判断不出真正的根类型 —— 所以配好 fstab 后必须重建一次，
# 否则 xfs/btrfs 会卡在 "run-init: /sbin/init: No such file or directory" 这类错误里。
chroot "$ROOT" update-initramfs -u -k all
chroot "$ROOT" /bin/bash -eux <<'CHROOT'
systemctl enable systemd-networkd systemd-resolved ssh qemu-guest-agent
systemctl enable serial-getty@ttyAMA0
# 首启为每台实例生成自己的 SSH 主机密钥（镜像里**不预置**任何密钥）。
# 坑：Debian 的 ssh.service 自己带 `ExecStartPre=/usr/sbin/sshd -t`，而主单元里的
# ExecStartPre 排在 drop-in 之前 —— 没有主机密钥时 `sshd -t` 会直接失败，
# 于是 ssh.service 起不来（实测：引导日志里 "[FAILED] Failed to start ssh.service"）。
# 所以这里先用空赋值**清空**继承来的 ExecStartPre 列表，再按正确顺序重写：
# 先生成密钥，再做配置自检。
# 另一个坑（实测踩到）：Debian 的 ssh-keygen 在 **/usr/bin/** 而不是 /usr/sbin，
# 写成 /usr/sbin/ssh-keygen 会让这条 ExecStartPre 直接执行不起来 → 密钥永远不生成 →
# sshd -t 必然失败。这里用 /bin/sh -c 'ssh-keygen -A' 走 PATH，不依赖具体路径。
rm -f /etc/ssh/ssh_host_*
mkdir -p /etc/systemd/system/ssh.service.d
cat > /etc/systemd/system/ssh.service.d/10-host-keys.conf <<'EOF'
[Service]
ExecStartPre=
ExecStartPre=/bin/sh -c 'ssh-keygen -A'
ExecStartPre=/usr/sbin/sshd -t
EOF
# 空赋值会重置列表，这里断言一下顺序确实写对了（生成密钥在自检之前）
awk '/^ExecStartPre=/ {print NR": "$0}' /etc/systemd/system/ssh.service.d/10-host-keys.conf
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
umount -R "$ROOT" 2>/dev/null || umount -R -l "$ROOT"
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
