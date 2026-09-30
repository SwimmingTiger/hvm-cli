#!/bin/bash
# 增量重建 ISO：拿已有的 ISO，只换掉「GRUB 引导程序(efi.img)」和「squashfs 里的安装器脚本」，
# 跳过 debootstrap/apt（那是完整构建里最慢的 20 分钟）。用于快速迭代引导与安装逻辑。
#
# 在 openEuler 的容器里跑：
#   sudo podman run --rm --privileged --network host \
#     -v /home/hu60/iso-work:/work \
#     -v <宿主机上放旧 ISO 的目录>:/iso \
#     debian:12 bash /work/fast-rebuild-iso.sh
set -euo pipefail

SRC=/iso/deb12iso.iso            # 旧 ISO（含好的 catalog/标记/squashfs）
OUT=/work/deb12iso2.iso           # 写到 openEuler 自己的工作目录，避免共享挂载的时序问题
T=/tmp/fast
LABEL=HVMDEB12
SQCOMP="${SQ_COMP:-gzip}"

apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq xorriso squashfs-tools mtools dosfstools grub-efi-arm64-bin >/dev/null 2>&1

rm -rf "$T"; mkdir -p "$T/tree" "$T/iso/live" "$T/iso/EFI/BOOT" "$T/efi"
echo "=== 1) 从旧 ISO 取 squashfs / 内核 / initrd ==="
for f in live/filesystem.squashfs live/vmlinuz live/initrd.img; do
    xorriso -osirrox on -indev "$SRC" -extract "/$f" "$T/$(basename $f)" >/dev/null 2>&1
    ls -l "$T/$(basename $f)" | awk '{print "  "$5"  "$9}'
done

echo "=== 2) 解开 squashfs ==="
unsquashfs -f -d "$T/tree" "$T/filesystem.squashfs" >/dev/null
echo "  解开 $(du -sh "$T/tree" | cut -f1)"

echo "=== 2.5) 补建运行时目录（关键：squashfs 里必须有 dev/proc/sys/run/tmp/mnt 这些挂载点）==="
# 打包时用 -e dev -e proc … 会把【目录本身】也排掉，于是 live 系统把 squashfs 当根挂上后
# /dev /proc /sys /run 根本不存在 → init 无法 bind-mount、连 /dev/console 都打不开 → init 退出 → panic。
mkdir -p "$T/tree/dev" "$T/tree/proc" "$T/tree/sys" "$T/tree/run" "$T/tree/tmp" "$T/tree/mnt"
chmod 1777 "$T/tree/tmp" 2>/dev/null || true
ls -d "$T/tree/dev" "$T/tree/proc" "$T/tree/sys" "$T/tree/run" "$T/tree/tmp" "$T/tree/mnt" | sed 's/^/  /'

echo "=== 3) 给安装器补上「在 ESP 上也放一份 grub.cfg」（本次唯一要改的脚本内容）==="
INST="$T/tree/usr/local/sbin/hvm-install-to-disk.sh"
grep -q "EFI/BOOT/grub.cfg" "$INST" && echo "  （已包含，跳过）" || \
awk '
  /^chroot \/target update-grub/ {
      print "chroot /target update-grub || chroot /target grub-mkconfig -o /boot/grub/grub.cfg || LOG \"生成 grub.cfg 失败\""
      print "# --removable 的引导器只在 ESP 的 /EFI/BOOT 找 grub.cfg，所以那里也放一份"
      print "mkdir -p /target/boot/efi/EFI/BOOT"
      print "chroot /target grub-mkconfig -o /boot/efi/EFI/BOOT/grub.cfg 2>/dev/null || cp /target/boot/grub/grub.cfg /target/boot/efi/EFI/BOOT/grub.cfg || true"
      print "[ -s /target/boot/efi/EFI/BOOT/grub.cfg ] && LOG \"ESP 上的 grub.cfg 就绪 ✓\" || LOG \"警告：ESP 上没有 grub.cfg\""
      next
  }
  { print }
' "$INST" > "$INST.new" && mv "$INST.new" "$INST" && echo "  ✓ 已插入 ESP grub.cfg 处理"

echo "=== 3.5) 给安装器加全量追踪（把输出打到控制台 + set -x），并把「找不到盘就 exec bash」换成可见的停顿 ==="
# oneshot 服务里没有 tty：exec /bin/bash 会立刻退出且无声失败 —— 改成明确报错退出。
# （不要用 sleep 之类"挂住"的办法：没人会去排查，只会白占虚拟机槽位。）
sed -i '2i exec >>/dev/console 2>&1\nset -x' "$INST"
sed -i 's|exec /bin/bash|LOG "错误：安装无法继续，中止"; exit 1|g' "$INST"
# ★ 分区设备名修正：/dev/vda → /dev/vda1（p 后缀只用于结尾是数字的设备名）
sed -i 's|^DISK=.*|&\ncase "$DISK" in *[0-9]) P="${DISK}p";; *) P="${DISK}";; esac|' "$INST"
sed -i 's|${DISK}p|${P}|g' "$INST"
grep -n '^case "\$DISK"' "$INST" | head -2 | sed 's/^/  /'
grep -n "set -x\|安装无法继续" "$INST" | head -4 | sed 's/^/  /'
# ★ sed -i 会重建文件并把权限重置成 0644 → systemd 的 ExecStart 直接执行失败，
#   而且这种 exec 失败只进 journal，串口上什么也看不到（实测踩到：
#   "[FAILED] Failed to start hvm-install…"，脚本一动不动）。必须补回可执行位。
chmod 0755 "$INST"
ls -l "$INST" | awk '{print "  权限: "$1}'
# 让服务用 bash -x 启动：万一 exec 阶段出问题也能看到原因
UNIT="$T/tree/etc/systemd/system/hvm-install.service"
sed -i 's|^ExecStart=.*|ExecStart=/bin/bash -x /usr/local/sbin/hvm-install-to-disk.sh|' "$UNIT"
grep -n "^ExecStart" "$UNIT" | sed 's/^/  /'

echo "=== 3.8) 打开 root 密码登录（Debian 默认 prohibit-password，实测 SSH 只提供 publickey）==="
mkdir -p "$T/tree/etc/ssh/sshd_config.d"
printf 'PermitRootLogin yes\nPasswordAuthentication yes\n' > "$T/tree/etc/ssh/sshd_config.d/10-root-password.conf"
cat "$T/tree/etc/ssh/sshd_config.d/10-root-password.conf" | sed 's/^/  /'

echo "=== 4) 重新打包 squashfs（$SQCOMP）==="
mksquashfs "$T/tree" "$T/iso/live/filesystem.squashfs" -comp "$SQCOMP" -noappend \
    -e 'proc/*' -e 'sys/*' -e 'dev/*' -e 'run/*' -e 'tmp/*' -e 'mnt/*' >/dev/null
ls -l "$T/iso/live/filesystem.squashfs" | awk '{print "  "$5"  "$9}'
cp "$T/vmlinuz" "$T/iso/live/vmlinuz"; cp "$T/initrd.img" "$T/iso/live/initrd.img"

echo "=== 5) 重建 EFI 引导程序（补 iso9660/search 模块 + 按卷标定位 root）==="
cat > "$T/grub.cfg" <<EOF
set timeout=5
set default=0
search --no-floppy --label $LABEL --set=root
menuentry "install debian12 to disk (auto, squashfs)" {
    linux  /live/vmlinuz boot=live components console=ttyAMA0,115200 ${INSTALL_ARG-install=1} quiet
    initrd /live/initrd.img
}
menuentry "live system (no install)" {
    linux  /live/vmlinuz boot=live components console=ttyAMA0,115200 quiet
    initrd /live/initrd.img
}
EOF
M="part_gpt part_msdos fat ext2 iso9660 udf normal linux search search_label serial efi_gop terminal"
grub-mkstandalone -O arm64-efi --modules="$M" --install-modules="$M" \
    -o "$T/efi/BOOTAA64.EFI" "boot/grub/grub.cfg=$T/grub.cfg"
EB="$T/efi.img"
rm -f "$EB"; dd if=/dev/zero of="$EB" bs=1M count=16 status=none
mkfs.vfat -n EFIBOOT "$EB" >/dev/null          # 注意：不指定 -F（见 docs/iso-install-notes.md）
mmd -i "$EB" ::/EFI ::/EFI/BOOT
mcopy -i "$EB" "$T/efi/BOOTAA64.EFI" ::/EFI/BOOT/BOOTAA64.EFI
W=$(stat -c %s "$T/efi/BOOTAA64.EFI"); G=$(mdir -i "$EB" ::/EFI/BOOT | awk '/BOOTAA64/{print $3}')
[ "$W" = "$G" ] && echo "  efi.img 自检 ✓（$G 字节）" || { echo "  ✗ efi.img 自检失败（$G != $W）"; exit 1; }
cp "$EB" "$T/iso/efi.img"
cp "$T/efi/BOOTAA64.EFI" "$T/iso/EFI/BOOT/BOOTAA64.EFI"

echo "=== 6) 兼容标记（过框架 CreateVm 校验）==="
mkdir -p "$T/iso/sources" "$T/iso/boot" "$T/iso/efi/microsoft/boot"
: > "$T/iso/sources/install.wim"; : > "$T/iso/sources/install.esd"; : > "$T/iso/boot/bcd"
cat > "$T/iso/HVM-COMPAT.TXT" <<'C'
sources/install.wim
\sources\install.wim
sources/install.esd
\sources\install.esd
boot/bcd
*microsoft corporation
efi/microsoft/boot
C
printf '\nwinpe\n' >> "$T/iso/HVM-COMPAT.TXT"

echo "=== 7) 打 ISO ==="
xorriso -as mkisofs -o "$OUT" -V "$LABEL" -e efi.img -no-emul-boot -eltorito-alt-boot \
    -R -isohybrid-gpt-basdat "$T/iso" >/dev/null
ls -l "$OUT" | awk '{print "  "$5"  "$9}'
# 再留一份在 /work（工作目录），万一共享目录那边有缓存/时序问题，可以从这里再拷
cp -f "$OUT" /work/deb12iso2.iso && ls -l /work/deb12iso2.iso | awk '{print "  /work 副本: "$5}'
echo "完成 ✓"
