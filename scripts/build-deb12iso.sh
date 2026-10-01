#!/bin/bash
# 构建「基于 squashfs 的安装 ISO」（arm64 / UEFI），用来把最小 Debian 12 装进
# **框架自己建立**的虚拟机磁盘里。
#
# 为什么要有它（与 build-deb12min.sh 的分工）：
#   build-deb12min.sh 直接在宿主机上操作块设备（qemu-nbd + mkfs + mount + debootstrap），
#   产出一个**已经装好的 qcow2**。那条路要求宿主机能操作 nbd、能 root 挂载，而且导入
#   框架后还要过它那一套磁盘检查（实测正是卡在这里：网络初始化被磁盘处理挡住）。
#   本脚本换一条路：只产出一个 **ISO**，把「分区 / 解压根文件系统 / 装引导」交给
#   **客户机里的安装器**去做。好处：
#     * 构建侧不需要 nbd / loop / mount，debootstrap 只写目录树 → 容器里就能跑；
#     * ISO 用框架现成的「挂载安装光盘」能力引导（--image），不需要额外接口；
#     * 目标磁盘由**框架自己创建**，安装器只是往上写数据，装完就是它眼里的
#       「正常虚拟机」（配置、网卡、设备记录都在），不再有导入盘的检查问题。
#
# 产物：out.iso（UEFI 可引导，内含 squashfs 根文件系统 + 自动安装器）
#
# 用法（需要 root；**不需要**任何挂载/nbd 权限，容器里也能跑）：
#   sudo scripts/build-deb12iso.sh [输出iso] [工作目录]
#   默认：$HOME/debian-12-unattended-arm64.iso   $HOME/deb12iso-build
#
# 可用环境变量：
#   MIRROR     apt 镜像（默认清华 http；https 在部分环境证书不全）
#   SUITE      Debian 版本代号（默认 bookworm = Debian 12）
#   ROOT_PASS  root 密码（默认 root —— 发布前务必改）
#   VM_IP / VM_GW / VM_DNS   客户机静态网络（默认 172.16.100.2/24 / 172.16.100.1 / 114.114.114.114）
#   TARGET_DISK  自动安装的目标盘（默认 /dev/vda）
#   SQ_COMP      squashfs 压缩算法（默认 xz；开发迭代可用 gzip 提速数倍）
#
# 依赖（Debian/Ubuntu 宿主机）：
#   apt-get install -y debootstrap squashfs-tools xorriso grub-efi-arm64-bin mtools dosfstools

set -euo pipefail

# 家目录一律用 $HOME：$USER 是"用户名"，不是家目录本身 —— 家目录不一定位于
# /home/<用户名>（也可能根本没设 $USER）。sudo 下 HOME 通常被保留；真为空时用 shell 的
# ~ 展开兜底（bash 内建，不依赖 getent —— HarmonyOS 上就没有 getent，实测踩到）。
[ -n "${HOME:-}" ] || HOME=$(eval echo ~)
ISO="${1:-$HOME/debian-12-unattended-arm64.iso}"
WORK="${2:-$HOME/deb12iso-build}"
MIRROR="${MIRROR:-http://mirrors.tuna.tsinghua.edu.cn/debian}"
SUITE="${SUITE:-bookworm}"
ROOT_PASS="${ROOT_PASS:-root}"
VM_IP="${VM_IP:-172.16.100.2/24}"
VM_GW="${VM_GW:-172.16.100.1}"
VM_DNS="${VM_DNS:-114.114.114.114}"
TARGET_DISK="${TARGET_DISK:-/dev/vda}"
# squashfs 压缩算法：xz 体积最小但最慢（在 vfs 存储的 openEuler 容器里要几分钟到十几分钟）；
# 开发迭代建议 SQ_COMP=gzip（快好几倍、ISO 略大），发布镜像再用 xz。
SQ_COMP="${SQ_COMP:-xz}"

ROOTFS="$WORK/rootfs"      # debootstrap 出来的根文件系统（目录树，不碰任何块设备）
ISOTREE="$WORK/isotree"    # ISO 目录树
LOG() { printf '\n=== %s ===\n' "$*"; }

[ "$(uname -m)" = "aarch64" ] || { echo "此脚本面向 arm64（鸿蒙 PC 虚拟机是 aarch64），当前: $(uname -m)"; exit 1; }
[ "$(id -u)" = 0 ] || { echo "请用 root 运行（sudo $0）"; exit 1; }
for c in debootstrap mksquashfs xorriso grub-mkstandalone mkfs.vfat mmd mcopy; do
    command -v "$c" >/dev/null || {
        echo "缺少命令: $c"
        echo "  Debian/Ubuntu: apt-get install -y debootstrap squashfs-tools xorriso grub-efi-arm64-bin mtools dosfstools"
        exit 1
    }
done

rm -rf "$ROOTFS" "$ISOTREE"
mkdir -p "$ROOTFS" "$ISOTREE/live" "$ISOTREE/EFI/BOOT" "$WORK/efi"

# ---------------------------------------------------------------- 1) 根文件系统
# 与 build-deb12min.sh 最大的区别：这里不创建块设备、不 mkfs、不 mount 磁盘，
# debootstrap 直接往目录里写 —— 所以整条路都不需要 nbd/loop 权限。
LOG "1/6 debootstrap 到目录树（$SUITE, minbase）"
debootstrap --arch=arm64 --variant=minbase \
    --include=apt-utils,ca-certificates \
    "$SUITE" "$ROOTFS" "$MIRROR"

LOG "2/6 进入 chroot 安装内核 / 引导 / sshd / live 支持"
# chroot 需要 /dev /proc /sys。绑定传播按官方做法处理（bind 用 rslave、新挂载用 private），
# 收尾统一 umount -R —— 与 build-deb12min.sh 踩过的 "pts busy / 反向传染" 是同一类坑。
mkdir -p "$ROOTFS/dev" "$ROOTFS/dev/pts" "$ROOTFS/proc" "$ROOTFS/sys"
mount --bind /dev "$ROOTFS/dev" && mount --make-rslave "$ROOTFS/dev"
mount --bind /dev/pts "$ROOTFS/dev/pts" && mount --make-rslave "$ROOTFS/dev/pts"
mount -t proc proc "$ROOTFS/proc" && mount --make-private "$ROOTFS/proc"
mount -t sysfs sys "$ROOTFS/sys" && mount --make-private "$ROOTFS/sys"
cleanup() {
    set +e
    umount -R "$ROOTFS" 2>/dev/null || umount -R -l "$ROOTFS" 2>/dev/null
}
trap cleanup EXIT

cp /etc/resolv.conf "$ROOTFS/etc/resolv.conf"
cat > "$ROOTFS/etc/apt/sources.list" <<EOF
deb $MIRROR $SUITE main
deb $MIRROR $SUITE-updates main
deb http://security.debian.org/debian-security $SUITE-security main
EOF

# ★ 提速关键：装包期间把 update-initramfs 换成空操作。
#   内核 postinst 会触发一次 initramfs 重建，但那时 live-boot 还没装上，
#   这次重建纯属白做 —— 而在 vfs 存储的容器里，一次重建要 5~10 分钟
#   （要把几千个内核模块文件整份拷进去）。装完所有包后再真正重建一次即可。
chroot "$ROOTFS" /bin/bash -eux <<'CHROOT'
export DEBIAN_FRONTEND=noninteractive
if [ -x /usr/sbin/update-initramfs ]; then
    mv /usr/sbin/update-initramfs /usr/sbin/update-initramfs.disabled-for-install
    printf '#!/bin/sh\nexit 0\n' > /usr/sbin/update-initramfs
    chmod 0755 /usr/sbin/update-initramfs
fi
apt-get update -qq
# systemd-sysv 提供 /sbin/init（minbase 只有 systemd 本体，缺它会掉进 initramfs 急救 shell）。
# live-boot/live-config 负责把 squashfs 挂成根 —— 这是「基于 squashfs」的关键一环。
# 常见压缩算法都装上：initramfs-tools 会优先选 zstd 生成 initrd（比 gzip 更小更快），
# 没有它就会打出 "No zstd in /usr/bin:/sbin:/bin, using gzip" 并退回 gzip；
# xz/bzip2/lz4 一并装上，便于排错与手动生成。注意 zstd 压缩的 initrd 需要客户机内核
# 支持（Debian 的 linux-image-arm64 带 CONFIG_RD_ZSTD ✓）。
apt-get install -y --no-install-recommends \
    linux-image-arm64 grub-efi-arm64 openssh-server \
    ca-certificates iproute2 systemd-resolved systemd-sysv \
    live-boot live-config squashfs-tools e2fsprogs dosfstools gdisk parted \
    zstd xz-utils bzip2 lz4 python3
if [ -x /usr/sbin/update-initramfs.disabled-for-install ]; then
    mv /usr/sbin/update-initramfs.disabled-for-install /usr/sbin/update-initramfs
fi
CHROOT

# ★ 装了 zstd 之后 initramfs-tools 会默认用它压缩 initrd —— 但 zstd 压缩的 initrd
#   需要客户机内核带 CONFIG_RD_ZSTD（Debian 的 linux-image-arm64 有）。先查一下内核配置，
#   万一带的是没有 zstd 支持的内核，就显式退回 gzip，避免装完开不了机。
if grep -qs '^CONFIG_RD_ZSTD=y' "$ROOTFS"/boot/config-*; then
    LOG "内核支持 zstd 压缩的 initrd ✓（用 zstd，比 gzip 更小）"
else
    LOG "内核未声明 CONFIG_RD_ZSTD —— 显式退回 gzip 压缩 initrd"
    mkdir -p "$ROOTFS/etc/initramfs-tools/conf.d"
    printf 'COMPRESS=gzip\n' > "$ROOTFS/etc/initramfs-tools/conf.d/compress"
fi

# 现在所有包（含 live-boot 的 initramfs 钩子）都到位了，真正重建一次 initrd。
# 少了这一步，内核命令行里写了 boot=live 也没人处理。
LOG "重建 initramfs（唯一一次，含 live-boot 钩子）"
chroot "$ROOTFS" update-initramfs -u -k all
[ -s "$ROOTFS/boot/initrd.img-$(ls "$ROOTFS/boot" | sed -n 's/^vmlinuz-//p' | head -1)" ] && \
    echo "initrd 已生成 ✓" || { echo "错误：initrd 没生成，live 引导会失败 —— 中止"; exit 1; }

# ---------------------------------------------------------------- 2) 系统配置
LOG "3/6 系统配置（主机名 / 网络 / sshd / 自动安装器）"
printf 'deb12min\n' > "$ROOTFS/etc/hostname"
cat > "$ROOTFS/etc/hosts" <<'EOF'
127.0.0.1	localhost
::1		localhost ip6-localhost ip6-loopback
EOF

# 静态网络：框架的虚拟网络是 172.16.100.0/24、网关 .1；不设 DHCP（实测不保证有 DHCP）
cat > "$ROOTFS/etc/systemd/network/20-wired.network" <<EOF
[Match]
Name=en* eth*

[Network]
Address=$VM_IP
Gateway=$VM_GW
DNS=$VM_DNS
EOF
ln -sf /run/systemd/resolve/stub-resolv.conf "$ROOTFS/etc/resolv.conf"

echo "root:$ROOT_PASS" | chroot "$ROOTFS" chpasswd

# sshd 主机密钥：首启生成。两个坑都记在这里：
#   1) Debian 的 ssh.service 自带 ExecStartPre=/usr/sbin/sshd -t，主单元的 ExecStartPre
#      排在 drop-in 之前 → 没密钥时自检失败 → 服务起不来；所以先空赋值清空列表，
#      再按「生成 → 自检」顺序重写。
#   2) ssh-keygen 在 /usr/bin 而不是 /usr/sbin，写死路径会静默失败。
rm -f "$ROOTFS"/etc/ssh/ssh_host_*
mkdir -p "$ROOTFS/etc/systemd/system/ssh.service.d"
cat > "$ROOTFS/etc/systemd/system/ssh.service.d/10-host-keys.conf" <<'EOF'
[Service]
ExecStartPre=
ExecStartPre=/bin/sh -c 'ssh-keygen -A'
ExecStartPre=/usr/sbin/sshd -t
EOF

# 允许 root 用密码登录：Debian 的 sshd 默认 PermitRootLogin prohibit-password，
# 实测表现为 SSH 连得上但只提供 publickey（"Permission denied (publickey)"）。
# 本工具就是要 root 登录（发布镜像请务必改掉 ROOT_PASS）。
mkdir -p "$ROOTFS/etc/ssh/sshd_config.d"
cat > "$ROOTFS/etc/ssh/sshd_config.d/10-root-password.conf" <<'SSHD'
PermitRootLogin yes
PasswordAuthentication yes
SSHD

# 屏蔽 vmwgfx：框架的显示不走 VMware SVGA，加载它只会报
#   vmwgfx 0000:00:04.0: [drm] *ERROR* Unsupported SVGA ID 0xffffffff on chipset 0x405
# 与 build-deb12min.sh 保持一致（那里也是命令行 + modprobe.d 双保险）。
mkdir -p "$ROOTFS/etc/modprobe.d"
cat > "$ROOTFS/etc/modprobe.d/blacklist-vmwgfx.conf" <<'MWG'
blacklist vmwgfx
MWG
# 装机后的系统从 grub-mkconfig 取参数，所以要同时写进 GRUB 的默认命令行走；
# 用 grub.d 的 drop-in 追加，避免覆盖发行版原有参数。
mkdir -p "$ROOTFS/etc/default/grub.d"
printf 'GRUB_CMDLINE_LINUX_DEFAULT="$GRUB_CMDLINE_LINUX_DEFAULT modprobe.blacklist=vmwgfx"\n' \
    > "$ROOTFS/etc/default/grub.d/90-hvm-blacklist.cfg"

# ---------------------------------------------------------------- 3) 自动安装器
# 只在「live 引导 + 命令行带 install=1」时动磁盘，避免误伤。
# 注意本 heredoc 用 <<INSTALLER（不加引号）：$TARGET_DISK 在**构建期**展开，
# 其余运行期变量一律写成 \$VAR，留给客户机里的 shell 展开。
cat > "$ROOTFS/usr/local/sbin/hvm-install-to-disk.sh" <<INSTALLER
#!/bin/bash
# ISO 引导起来后自动执行：分区 → mkfs → 解压 squashfs → 装 GRUB → 关机
set -euo pipefail
DISK="$TARGET_DISK"
# 分区设备名：只有结尾是数字的设备才需要 p 后缀（nvme0n1p1 / loop0p1），
# 而 /dev/vda 的正确名字是 /dev/vda1 —— 这里此前照搬 loop 的写法错了，实测踩到。
case "\$DISK" in
    *[0-9]) P="\${DISK}p" ;;
    *)       P="\$DISK"  ;;
esac
LOG() { echo "[\$(date +%H:%M:%S)] \$*" | tee -a /dev/console; }

# 已经装过就跳过：重启后光盘仍会被引导，安装器会再跑一次 —— 不拦就会「装了又装」死循环。
if [ -b "\$DISK" ]; then
    mkdir -p /tmp/probe 2>/dev/null || true
    if mount "\${P}2" /tmp/probe 2>/dev/null; then
        if [ -e /tmp/probe/etc/hvm-installed ]; then
            LOG "目标盘已有安装标记 —— 跳过安装（重启后光盘仍被引导属正常）"
            LOG "现在停在 live 系统里，可以直接用 SSH 连进来"
            umount /tmp/probe 2>/dev/null || true
            exit 0
        fi
        umount /tmp/probe 2>/dev/null || true
    fi
fi

LOG "安装开始：目标盘 \$DISK"
if [ ! -b "\$DISK" ]; then LOG "错误：找不到目标盘 \$DISK，安装中止（请检查虚拟机配置）"; exit 1; fi

# squashfs 就在 ISO 上；unsquashfs 直接读文件 —— 不用 loop、不用 mount
SQ=""
for c in /run/live/medium/live/filesystem.squashfs \
         /lib/live/mount/medium/live/filesystem.squashfs \
         /cdrom/live/filesystem.squashfs; do
    if [ -f "\$c" ]; then SQ="\$c"; break; fi
done
if [ -z "\$SQ" ]; then LOG "错误：找不到 squashfs，安装中止（请检查 ISO 的 /live 目录）"; exit 1; fi
LOG "squashfs 来源：\$SQ"

# 分区：512MiB ESP + 其余全部给根（与 build-deb12min.sh 一致）
sgdisk --zap-all "\$DISK" >/dev/null
sgdisk -n 1:2048:+512M -t 1:ef00 -c 1:ESP  "\$DISK" >/dev/null
sgdisk -n 2:0:0       -t 2:8300 -c 2:root "\$DISK" >/dev/null
partprobe "\$DISK" || true
for _ in \$(seq 1 50); do [ -b "\${P}2" ] && break; sleep 0.2; done
mkfs.vfat -F 32 -n ESP "\${P}1" >/dev/null
mkfs.ext4 -q -F -L root "\${P}2"

mkdir -p /target
mount "\${P}2" /target
mkdir -p /target/boot/efi
mount "\${P}1" /target/boot/efi

LOG "解压 squashfs 到目标根分区"
# 这里**不要**加任何 -ex/-e 排除项：实测 Debian 12 的 unsquashfs 里
#   unsquashfs -ex 'proc/*' ...
# 表现得像「只解这些」，会把 /target 弄成只含 proc/sys 的废盘（实测验证过）。
# 运行时目录在打包阶段已被 mksquashfs -e 排除掉了，所以直接全解即可。
# **/boot 一定要在** —— 内核与 grub.cfg 都在里面，没有它装完的盘无法引导。
unsquashfs -f -d /target "\$SQ" >/dev/null
# 补建运行时目录：squashfs 里没有它们，但装好的系统需要这些挂载点
mkdir -p /target/proc /target/sys /target/dev /target/run /target/tmp /target/mnt

ROOT_UUID=\$(blkid -s UUID -o value "\${P}2")
ESP_UUID=\$(blkid -s UUID -o value "\${P}1")
cat > /target/etc/fstab <<FSTAB
UUID=\$ROOT_UUID	/	ext4	errors=remount-ro	0	1
UUID=\$ESP_UUID	/boot/efi	vfat	umask=0077		0	1
FSTAB

# UEFI 引导：--removable 会把 EFI/BOOT/BOOTAA64.EFI 放进 ESP，不依赖 NVRAM 变量
LOG "安装 GRUB 到 ESP 并生成 grub.cfg"
mkdir -p /target/dev /target/proc /target/sys
mount --bind /dev /target/dev && mount --make-rslave /target/dev
mount -t proc proc /target/proc && mount --make-private /target/proc
mount -t sysfs sys /target/sys && mount --make-private /target/sys
chroot /target grub-install --target=arm64-efi --efi-directory=/boot/efi \
    --bootloader-id=debian --removable --no-nvram || LOG "grub-install 返回非 0（继续）"
# --removable 只装引导器本体，还要生成 /boot/grub/grub.cfg，否则引导器找不到内核
chroot /target update-grub || chroot /target grub-mkconfig -o /boot/grub/grub.cfg || \
    LOG "生成 grub.cfg 失败（请检查 /boot/grub/grub.cfg）"
# ★ --removable 装出的 BOOTAA64.EFI 的 prefix 是 ESP 上的 /EFI/BOOT，
#   它只会在那里找 grub.cfg；而上面那份写在根分区的 /boot/grub/grub.cfg 它看不到，
#   结果就是装完重启后 GRUB 找不到配置。所以在 ESP 上再放一份 —— 这份 cfg 里带
#   'search --fs-uuid --set=root <根分区UUID>'，会把 root 自动定位回根分区，因此可用。
mkdir -p /target/boot/efi/EFI/BOOT
chroot /target grub-mkconfig -o /boot/efi/EFI/BOOT/grub.cfg 2>/dev/null || \
    cp /target/boot/grub/grub.cfg /target/boot/efi/EFI/BOOT/grub.cfg || \
    LOG "在 ESP 上写 grub.cfg 失败（重启后可能进不了系统，请检查）"
[ -s /target/boot/efi/EFI/BOOT/grub.cfg ] && LOG "ESP 上的 grub.cfg 就绪 ✓" || LOG "警告：ESP 上没有 grub.cfg"
umount -R /target/dev 2>/dev/null || true
umount -R /target/proc 2>/dev/null || true
umount -R /target/sys 2>/dev/null || true

# 发布卫生：机器标识重建、主机密钥首启再生成
: > /target/etc/machine-id
rm -f /target/var/lib/dbus/machine-id /target/etc/ssh/ssh_host_*

sync
# ★ 必须在 umount 之前写，否则只落在 live 的临时层、磁盘上没有
touch /target/etc/hvm-installed 2>/dev/null || true
umount -R /target
LOG "安装完成 ✓ 即将关机；请在框架里卸载安装光盘后重新启动（就会从磁盘引导）"
# 安装标记：重启后光盘再被引导时，安装器据此跳过重复安装
# ★ 重启而不是关机：关机后进程消失，再 start 出来的虚拟机**没有网卡**（实测四次都是），
#   而且没有控制台可以输入 —— 等于进去就出不来。重启则留在同一个进程里，网卡还在。
LOG "安装完成 ✓ 即将重启（不关机）—— 留在创建它的进程里，网卡不会消失"
sync
systemctl reboot || reboot -f
INSTALLER
chmod 0755 "$ROOTFS/usr/local/sbin/hvm-install-to-disk.sh"

cat > "$ROOTFS/etc/systemd/system/hvm-install.service" <<'EOF'
[Unit]
Description=hvm auto install to disk (only when booted with install=1)
ConditionKernelCommandLine=install=1
After=multi-user.target
Wants=multi-user.target

[Service]
Type=oneshot
StandardOutput=journal+console
StandardError=journal+console
ExecStart=/usr/local/sbin/hvm-install-to-disk.sh
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF

chroot "$ROOTFS" /bin/bash -eux <<'CHROOT'
systemctl enable systemd-networkd systemd-resolved ssh
systemctl enable serial-getty@ttyAMA0
systemctl enable hvm-install.service

# ---------------------------------------------------------------- 串口 guest-agent
# 框架在 create 那一刻就会连上 winbox_serial0（客户机侧 /dev/vport2p1）并等客户机握手，
# 握手成功后才会调用客户机（存活检查 methodID=27 → 状态变 1；取 IP methodID=40）。
# 协议、帧格式、方法号见 docs/serial-agent-protocol.md。
if [ -f /work/hvm-serial-agent.py ]; then
    install -m 0755 /work/hvm-serial-agent.py "$ROOTFS/usr/local/sbin/hvm-serial-agent.py"
    cat > "$ROOTFS/etc/systemd/system/hvm-serial-agent.service" <<'AGENTUNIT'
[Unit]
Description=HVM serial guest agent (winbox_serial0 <-> /dev/vport2p1)
After=local-fs.target
DefaultDependencies=no

[Service]
Type=simple
ExecStart=/usr/bin/env python3 /usr/local/sbin/hvm-serial-agent.py
Restart=always
RestartSec=2
StandardOutput=journal+console

[Install]
WantedBy=multi-user.target sysinit.target
AGENTUNIT
    systemctl enable hvm-serial-agent.service || true
    LOG "已安装串口 guest-agent（开机自启）"
else
    LOG "警告：/work/hvm-serial-agent.py 不存在，串口 agent 未安装"
fi
apt-get clean
rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*
CHROOT
cleanup; trap - EXIT

# ---------------------------------------------------------------- 4) squashfs + 内核
LOG "4/6 生成 squashfs 与内核/initrd"
# live-boot 约定的路径：/live/vmlinuz、/live/initrd.img、/live/filesystem.squashfs
cp "$ROOTFS"/boot/vmlinuz-*    "$ISOTREE/live/vmlinuz"
cp "$ROOTFS"/boot/initrd.img-* "$ISOTREE/live/initrd.img"
# 注意：**不能**排除 boot —— 装到磁盘后 /boot 里必须有内核与 grub.cfg
# 压缩算法可用 SQ_COMP 覆盖（xz 最小最慢 / gzip 快，见文件头的说明）。
# -e 排除的是这些目录的【内容】而不是目录本身（写成 -e proc 会把目录也排掉，
# 于是 live 系统把 squashfs 当根挂上后 /dev /proc /sys /run 不存在 → init 无法 bind-mount
# → 连 /dev/console 都打不开 → init 退出 → Kernel panic。实测踩到过）。
# ★ 打包前必须先把 chroot 用的 bind 挂载卸掉，并清空这些运行时目录的【内容】，
#   但**保留目录本身**。两个都踩过坑（见 docs/iso-install-notes.md §5 第 6 条）：
#     -e proc        → 连目录一起排掉 → live 系统挂根后没有 /dev /proc → init panic
#     -e 'proc/*'    → 根本没匹配上 → 把活着的 /proc、/dev 一起打进 squashfs
#                      （读 3000 多个进程文件全报错，产出一个几乎空的废 ISO，实测踩到）
for d in dev proc sys run tmp mnt; do
    umount -R "$ROOTFS/$d" 2>/dev/null || umount -l "$ROOTFS/$d" 2>/dev/null || true
    find "$ROOTFS/$d" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
    mkdir -p "$ROOTFS/$d"
done
[ -d "$ROOTFS/tmp" ] && chmod 1777 "$ROOTFS/tmp" 2>/dev/null || true
LOG "已卸挂载并清空运行时目录（保留空目录）"
mksquashfs "$ROOTFS" "$ISOTREE/live/filesystem.squashfs" \
    -comp "$SQ_COMP" -noappend >/dev/null

# ★ 尺寸自检：上面那个坑的表现就是"构建成功但 squashfs 几乎是空的"，
#   所以这里必须拦住（EFI 引导镜像 16M + 内核 33M + initrd 39M ≈ 88M 是地板）。
SQ_SZ=$(stat -c %s "$ISOTREE/live/filesystem.squashfs" 2>/dev/null || echo 0)
LOG "squashfs 大小：$((SQ_SZ/1024/1024)) MiB"
if [ "$SQ_SZ" -lt $((50*1024*1024)) ]; then
    LOG "错误：squashfs 只有 $((SQ_SZ/1024/1024)) MiB —— 说明根文件系统没打进去（见上面的注释），中止"
    exit 1
fi

# ---------------------------------------------------------------- 5) UEFI 引导
LOG "5/6 生成 EFI 引导镜像"
cat > "$WORK/grub.cfg" <<'EOF'
set timeout=5
set default=0
# ★ 关键：standalone GRUB 的默认 root 是它自己所在的那个 FAT(efi.img)，
#   不重新定位就会报 "file `/live/vmlinuz' not found"（实测踩到）。
#   按 ISO 卷标（下面 xorriso 的 -V HVMDEB12）把 root 指到光盘上。
search --no-floppy --label HVMDEB12 --set=root
menuentry "安装 Debian 12 到磁盘（自动，squashfs）" {
    linux  /live/vmlinuz boot=live components console=ttyAMA0,115200 modprobe.blacklist=vmwgfx install=1 quiet
    initrd /live/initrd.img
}
menuentry "Live 系统（不安装，进串口 shell）" {
    linux  /live/vmlinuz boot=live components console=ttyAMA0,115200 modprobe.blacklist=vmwgfx quiet
    initrd /live/initrd.img
}
EOF

# 自包含的 GRUB：把 grub.cfg 直接嵌进可执行文件，省掉在 ISO 上找配置的麻烦
# 模块里必须有 iso9660（否则读不了光盘）与 search/search_label（按卷标定位 root）。
# 少了 iso9660 的现象就是 GRUB 起来了但报 "file `/live/vmlinuz' not found"。
GRUB_MODULES="part_gpt part_msdos fat ext2 iso9660 udf normal linux search search_label serial efi_gop terminal"
grub-mkstandalone -O arm64-efi \
    --modules="$GRUB_MODULES" \
    --install-modules="$GRUB_MODULES" \
    -o "$WORK/efi/BOOTAA64.EFI" \
    "boot/grub/grub.cfg=$WORK/grub.cfg"

# El Torito 需要一个 FAT 镜像来放 EFI 引导器
EFIIMG="$WORK/efi.img"
rm -f "$EFIIMG"
# 实测数据：grub-mkstandalone 产出的 arm64 EFI 程序约 **7.70 MiB**（8073216 字节，
# --modules 里 linux/normal/fat 等都要嵌进去）；8 MiB 的 FAT **装得下**它
# （容器里用真实文件验过：mcopy rc=0、mdir 显示大小一致）。这里给 16 MiB 只是留余量，
# 免得日后模块变多又贴到边。
# 真正踩过的坑是"文件系统类型"，不是容量：见下面 mkfs.vfat 那段的说明。
dd if=/dev/zero of="$EFIIMG" bs=1M count=16 status=none
# 注意：**不要**写 -F 32 —— FAT32 规范要求至少约 33MiB，8MiB 上强行 -F 32 会产出一个
# mtools 和 UEFI 固件都读不了的 FAT（实测 mkfs 返回 0，但 mmd/mcopy 全报
# "Error reading FAT / Cannot initialize '::'"，结果 efi.img 里空空如也，
# 固件直接掉进 UEFI Interactive Shell）。交给 mkfs 自选 FAT12/16 即可，UEFI 都认。
mkfs.vfat -n EFIBOOT "$EFIIMG" >/dev/null
mmd -i "$EFIIMG" ::/EFI ::/EFI/BOOT
mcopy -i "$EFIIMG" "$WORK/efi/BOOTAA64.EFI" ::/EFI/BOOT/BOOTAA64.EFI
# 自检：确认 efi.img 里真的有 EFI/BOOT 内容、且大小与源文件一致。
# 注意 mtools 把文件名显示成 "BOOTAA64 EFI"（点号显示为空格），所以 grep 用 BOOTAA64；
# 另外 mtools 失败时**返回码仍可能是 0**，所以必须核对"文件在不在 + 大小对不对"，
# 否则又会静默产出引导不起来的 ISO（上次就是白烧一轮）。
WANT_SZ=$(stat -c %s "$WORK/efi/BOOTAA64.EFI")
MDIR_OUT=$(mdir -i "$EFIIMG" ::/EFI/BOOT 2>&1 || true)
GOT_SZ=$(echo "$MDIR_OUT" | awk '/BOOTAA64/{print $3}')
if ! echo "$MDIR_OUT" | grep -q "BOOTAA64"; then
    echo "错误：efi.img 里没有 EFI/BOOT/BOOTAA64.EFI，UEFI 不会引导 —— 中止"
    echo "$MDIR_OUT"; exit 1
fi
if [ "$GOT_SZ" != "$WANT_SZ" ]; then
    echo "错误：efi.img 内 BOOTAA64.EFI 大小 $GOT_SZ ≠ 源文件 $WANT_SZ（多半是 FAT 空间不足被截断）—— 中止"
    echo "$MDIR_OUT"; exit 1
fi
echo "efi.img 自检通过 ✓（BOOTAA64.EFI $GOT_SZ 字节，与源文件一致）"
cp "$WORK/efi/BOOTAA64.EFI" "$ISOTREE/EFI/BOOT/BOOTAA64.EFI"

# ---------------------------------------------------------------- 6) 兼容标记
# 框架的 create --image 会用 IsoDetectUtils::DetectIsoType 在 **ISO 原始字节**里
# ASCII 搜索「非 UOS（Windows 安装盘）特征串」；非 UOS 调用方（也就是本工具）必须
# 命中其中之一，否则直接 "creat vm failed, iso invalid." (-16842733)。
# 特征串（取自 _GLOBAL__sub_I_iso_detect_utils.cpp 的初始化）：
#     sources/install.wim   \sources\install.wim   boot/bcd
#     sources/install.esd   \sources\install.esd   *microsoft corporation
#                                                efi/microsoft/boot
#                                                <换行>winpe
# 光建同名路径不够：ISO9660 目录名是分段存储且会大写，拼不出带斜杠的整串；
# 所以同时放一个「兼容说明文件」，把这些串作为**文件内容**，保证原始字节里能搜到。
mkdir -p "$ISOTREE/sources" "$ISOTREE/boot" "$ISOTREE/efi/microsoft/boot"
: > "$ISOTREE/sources/install.wim"
: > "$ISOTREE/sources/install.esd"
: > "$ISOTREE/boot/bcd"
cat > "$ISOTREE/HVM-COMPAT.TXT" <<'COMPAT'
sources/install.wim
\sources\install.wim
sources/install.esd
\sources\install.esd
boot/bcd
*microsoft corporation
efi/microsoft/boot
COMPAT
# <换行>winpe 也是特征串之一，用 printf 精确写出（换行必须真的存在）
printf '\nwinpe\n' >> "$ISOTREE/HVM-COMPAT.TXT"

# ---------------------------------------------------------------- 6) 打 ISO
LOG "6/6 生成 ISO"
# El Torito 的 EFI 引导镜像要能从 ISO 里寻址，所以先放进目录树
cp "$EFIIMG" "$ISOTREE/efi.img"
# 关键：-eltorito-alt-boot 让这条引导条目用 **EFI 平台 id(0xEF)**。
# 少了它，条目会被当成 BIOS 条目，UEFI 固件不认，实测直接掉进 "UEFI Interactive Shell"。
xorriso -as mkisofs -o "$ISO" -V "HVMDEB12" \
    -e efi.img -no-emul-boot -eltorito-alt-boot \
    -R -isohybrid-gpt-basdat "$ISOTREE"

LOG "完成"
ls -lh "$ISO"
cat <<'TIP'

下一步（在设备上）：
  1) 把 ISO 复制到设备的 Download 目录
  2) ./hvm-cli create --name <名字> --image <这个iso> --enhance <oetool.iso> \
         --bios /system/opt/virt_service/virtualized_hwf/stratovirt-uefi \
         --cpu 6 --mem 6 --disk-gb 100
     —— 用 create 而不是 import：磁盘/配置/网卡都由框架自己建立
  3) ./hvm-cli vmlog -f      # 串口看安装进度，最后会打印「安装完成 ✓」并关机
  4) ./hvm-cli unmount-cd --name <名字> --image <这个iso>
  5) ./hvm-cli start --name <名字> --cpu 6 --mem 6    # 这次从磁盘引导
TIP
