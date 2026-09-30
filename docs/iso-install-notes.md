# squashfs 安装 ISO：把最小 Debian 12 装进「框架自建」的虚拟机

本文记录 `scripts/build-deb12iso.sh` 的设计依据、踩过的坑，以及为什么走这条路。
配套工具：`scripts/audit-iso.sh`（ISO 落地后一次性审计）。

> **当前进度**
> * 已实测通过：ISO 构建（含 `efi.img` 大小自检）、`audit-iso.sh` 全项（`efi.img` 内容 +
>   squashfs 17 项 + initrd 的 live-boot 机制）、`create --image <我们的 ISO>` 被框架接受
>   （兼容标记 8/8 命中）、**固件成功引导进 GRUB**（`Welcome to GRUB!`）。
> * 已实测通过（全部）：GRUB →内核 → live-boot 挂 squashfs → 安装器分区/解压/装 GRUB → 自动关机；
>   重启后从**磁盘**引导起装好的系统；**从设备 `ssh root@172.16.100.2` 登录成功**，
>   客户机里 `enp0s6` 上带静态地址 `172.16.100.2/24`。
> * 仍未拿到：`./hvm-cli net ip` 返回 405（框架侧那条查询没拿到客户机地址）。网络本身是通的
>   —— SSH 就是证据 —— 所以这属于框架查询路径的问题，不是镜像问题。

## 1. 为什么要做成 ISO，而不是直接给一个 qcow2

`scripts/build-deb12min.sh` 直接在宿主机上操作块设备（`qemu-nbd` + `mkfs` + `mount` +
`debootstrap`），产出一个**已经装好的 qcow2**，再用 `hvm-cli import` 导入。这条路有两个问题：

1. **导入出来的虚拟机没有网络。** 逆向 `libvm_manager.z.so` 后可以确认：
   网络配置**不是**磁盘里的东西，而是宿主侧按 `CfgInfo` 分配的 ——
   `VmManager::CreateVm` / `StartVm` → `VmAssistantManager::CheckBeforeStartVm`
   → `Engine::CheckBeforeStartVm` → `Engine::NetConfig(CfgInfo, vmName, deviceUuid)`
   → `NetManager::AllocateNet` → `DefaultNetConfig`/`BridgeNetConfig` → `VmNetProperties::SetNetConfigInfo`
   （写进一个**全局单例**，`GetVmIpv4Address`/`SetVmNetMode` 都读它）。
   而 `ImportVmDiskImage(vmName, srcPath, sha256, opts)` 的签名里**根本没有 CfgInfo**，
   所以导入出来的虚拟机在框架眼里"没有网络配置"，`net ip` / 模式切换一律返回
   405 (`VM_IP_UNAVAILABLE`)。要补的字段位于 `deviceInfo` 子对象内（jsKey 与偏移由
   napi 库的 `UnwrapNetworkDevice` 与上述读取点交叉确认）：

   | jsKey | 偏移(CfgInfo) | 类型 |
   |---|---|---|
   | `netMode` | +228 | int（0=桥接，1=NAT） |
   | `nicName` | +232 | string（桥接时要给宿主物理网卡名） |
   | `bridgeIp` | +256 | string |
   | `proxyAutoSyncEnabled` | +280 | bool |
   | `dnsAutoSyncEnabled` | +281 | bool |
   | `isHostNetworkSyncFeatureEnabled` | +282 | bool |
   | `isNetworkShareSupported` | +283 | bool |
   | `networkDevice`（子对象开关） | +284 | bool；**为真时 `Engine::NetConfig` 强制 netMode=1(NAT)**，所以桥接必须置 false |

   `hvm-cli start --net nat|bridge [--nic X] [--bridge-ip Y] [--proxy-sync] …` 就是填这些字段。
   **注意**：把它交给 `StartVm` 能否真正建出网卡，还受
   `Engine::CheckBeforeStartVm` 里那道磁盘检查门槛影响（详见 §3）。

2. **`create --image` 有"客户机类型"校验墙。** `VmManager::CreateVm` 会调用
   `IsoDetectUtils::DetectIsoType(isoPath, IsLegalUosCalling())`，它在 ISO 前
   `0x10000000` 字节里做 **ASCII 特征串搜索**：

   * 非 UOS 调用方（就是本工具）→ 必须命中 **Windows 安装盘**特征：
     `sources/install.wim`、`\sources\install.wim`、`sources/install.esd`、
     `\sources\install.esd`、`boot/bcd`、`*microsoft corporation`、`efi/microsoft/boot`、`\nwinpe`
   * UOS 调用方 → 必须命中 **Linux Live** 特征：`syslinux`、`isolinux`、`casper`、
     `vmlinuz`、`initrd`、`grub.cfg` 等

   两者必须匹配，否则直接
   `creat vm failed, iso invalid. (-16842733 / 0xFEFF0013)`。
   所以本脚本在 ISO 里放了这些特征串（见 §4 的"兼容标记"），让我们的 Linux ISO 能通过校验。

**结论**：让**框架自己建**虚拟机（配置/网卡/设备记录齐全）→ 从 ISO 引导 → 把系统装进
框架建的那块盘。这样既绕开"导入盘没有网络"，也不需要 `qemu-nbd` 之类的宿主权限。

## 2. 构建环境

只依赖 **openEuler 环境里的 podman**（aarch64 + `debian:12` 容器）：

```bash
# 在 openEuler 里（设备侧用 ./openeuler exec 进去）
sudo podman run --rm --replace --name hvm-iso-build --privileged --network host \
    -e SQ_COMP=gzip \
    -v /home/hu60/iso-work:/work debian:12 /work/build-container.sh
```

* 需要 `--privileged`：脚本要在 chroot 里 bind `/dev`、`/proc`、`/sys` 跑 `debootstrap`/`apt`；
* 需要 `--network host`：openEuler 的 podman **CNI bridge 是坏的**（报
  `cni plugin bridge failed`），走宿主网络才通；
* 产物取回**不需要任何传输工具**：openEuler 里 `/mnt/linux_share/storage/Users/currentUser`
  就是设备的 `/storage/Users/currentUser`（virtiofs），`cp` 一下就过去了。

> 构建很慢的原因：openEuler 的 podman 用 **vfs 存储驱动**，每次写文件都是整份拷贝。
> 因此脚本专门做了两处优化：`SQ_COMP` 可把 xz 换成 gzip；装包期间把 `update-initramfs`
> 临时替换成空操作（内核 postinst 那次重建时 `live-boot` 还没装，纯属白做）。

## 3. 已知的磁盘检查门槛（影响网卡）

`Engine::CheckBeforeStartVm` 的结构是：

```c
QcowDisk = Engine::CreateQcowDisk(...);          // state==5005 时立即 return 0
if (!QcowDisk) {
    StratovirtVars = Engine::CreateStratovirtVars(...);   // 同样 5005 直接 return 0
    if (!StratovirtVars) {
        VmDevice::InitDeviceInfo();
        VmDevice::QueryDeviceUuid(uuid);
        return Engine::NetConfig(cfg, vmName, uuid);      // ← 只有这里才建网络
    }
}
```

也就是说**磁盘处理任一步返回非 0，整个块（含建网）都会被跳过**。
`QcowState` 每个虚拟机一条，存在系统设置里（`SettingProvider`，key = 常量前缀 + 虚拟机名），
缺省 2；`VmAssistantManager::SetDefaultQcowState` 每次启动都会把它重置成 2，
而 **5005 = "磁盘无需处理"**（两条判断最前面就 `return 0`）。
`ImportVmDiskImage` 建出来的虚拟机没有 `CfgInfo`，因此这条路要特别留意。

## 4. 关键：网络字段必须在 **create** 时就给

实测结论（很重要）：

* `hvm-cli create --name X --image <ISO> ... --net nat` → 虚拟机**有网卡**
  （命令行里能看到 `virtio-net-pci,netdev=net0,id=nic0` 与宿主侧 tap `WVMTap…`）✓
* 同样的 `--net nat` 只加在 `start` 上 → **没有网卡** ✗

即网络配置要进 `create` 时那份**存档配置**里；`StartVm` 传的 `CfgInfo` 起不到这个作用
（与 §3 的磁盘检查门槛共同作用）。所以发布流程里 `create` 必须带 `--net nat`。

## 5. 踩过的坑（按出现顺序）

1. **`mkfs.vfat -F 32` 用在 8 MiB 映像上 ⇒ 空 efi.img。**
   FAT32 规范要求至少约 33 MiB；强行 `-F 32` 会产出一个 mtools 和 UEFI 固件都读不了的 FAT：
   `mkfs` 返回 0，但随后 `mmd`/`mcopy` 报 `Error reading FAT / Cannot initialize '::'`。
   现象：efi.img 里空空如也 → 固件直接掉进 `UEFI Interactive Shell`。
   **做法**：不指定 `-F`，让 mkfs 自选 FAT12/16（UEFI 都认）。
   另外 mtools **失败时返回码仍可能是 0**，所以脚本加了自检：既查文件在不在、又比对大小。

2. **`unsquashfs` 的 `-ex` 在 Debian 12 上表现得像"只解这些"。**
   安装器原来写 `unsquashfs ... -ex 'proc/*' -ex 'sys/*' …`，实测解出来**只有** proc/sys。
   **做法**：打包时用 `mksquashfs -e proc -e sys …` 排掉运行时目录，安装器直接全解，
   再 `mkdir -p /target/{proc,sys,dev,run,tmp,mnt}` 补回挂载点。

3. **El Torito 条目必须是 EFI 平台。** 只写 `-e efi.img -no-emul-boot` 会被当成 BIOS 条目，
   UEFI 固件不认。**做法**：加 `-eltorito-alt-boot`（平台 id = 0xEF）。

4. **standalone GRUB 的 root 是它自己所在的那个 FAT。**
   `grub-mkstandalone` 内嵌 grub.cfg，但 root 默认是 efi.img，于是
   `linux /live/vmlinuz` 报 `file '/live/vmlinuz' not found`。
   **做法**：模块表里必须有 `iso9660`；并在 grub.cfg 里
   `search --no-floppy --label HVMDEB12 --set=root` 把 root 定位到 ISO（卷标由 `-V` 设定）。

5. **`grub-install --removable` 装出的引导器只在 ESP 上找 grub.cfg。**
   而 `update-grub` 把它写在根分区 `/boot/grub/grub.cfg`，重启后 GRUB 找不到配置。
   **做法**：额外 `grub-mkconfig -o /boot/efi/EFI/BOOT/grub.cfg` 在 ESP 上再放一份
   （这份 cfg 自带 `search --fs-uuid --set=root`，会把 root 自动定位回根分区）。

6. **squashfs 里必须保留 `dev/ proc/ sys/ run/ tmp/ mnt/` 这些【目录】。**
   最初用 `mksquashfs -e dev -e proc …` 把**目录本身**也排掉了，于是 live 系统把它当根挂上后
   `/dev` `/proc` 等挂载点根本不存在 → init 无法 bind-mount、连 `/dev/console` 都打不开 →
   `Kernel panic - not syncing: Attempted to kill init!`。
   **做法**：排除写 `-e 'proc/*'`（只排内容，保留目录），安装器解压后再 `mkdir -p` 补一遍。

7. **分区设备名不能照搬 loop 的写法。**
   安装器里写 `"${DISK}p1"`，而 `DISK=/dev/vda` → 拼成 `/dev/vdap1`（不存在），
   `mkfs.vfat` 报 `No such file or directory`，脚本一直在等 `/dev/vdap2`。
   **做法**：只有结尾是数字的设备名才加 `p`（`nvme0n1p1`/`loop0p1`），
   `case "$DISK" in *[0-9]) P="${DISK}p";; *) P="${DISK}";; esac`。

8. **`sed -i` 会把文件权限重置成 0644。**
   用它改安装器脚本后，systemd 的 `ExecStart` 直接执行失败，
   而且 exec 阶段的失败**只进 journal**，串口上什么也看不到 —— 现象就是
   `[FAILED] Failed to start hvm-install…` 而脚本一动不动。
   **做法**：改完 `chmod 0755` 补回可执行位；排查时用 `ExecStart=/bin/bash -x <脚本>`。

9. **Debian 的 sshd 默认 `PermitRootLogin prohibit-password`。**
   现象：SSH **连得上**（拿到主机密钥）但只提供 `publickey`：
   `root@172.16.100.2: Permission denied (publickey)`。
   **做法**：镜像里放 `/etc/ssh/sshd_config.d/10-root-password.conf`：
   `PermitRootLogin yes` + `PasswordAuthentication yes`（发布镜像务必改掉 ROOT_PASS）。

10. **框架的 ISO 校验（见 §1.2）。** 非 UOS 调用方必须命中 Windows 安装盘特征串，
   所以脚本在 ISO 树里放了 `sources/install.wim` 等同名空文件，**并且**放一个
   `HVM-COMPAT.TXT`，把这些串作为**文件内容**写进去 —— 因为 ISO9660 目录名是分段存储
   且会大写，光靠同名路径拼不出带斜杠的整串，而校验做的是**原始字节搜索**。
   `-R`（Rock Ridge）用于保留小写原名。

## 6. 产物与验证

```bash
# 构建（在 openEuler 里）
sudo podman run … debian:12 /work/build-container.sh

# 取回设备（virtiofs 共享目录，一步到位）
./openeuler exec 'cp /home/hu60/iso-work/debian-12-unattended-arm64.iso \
    /mnt/linux_share/storage/Users/currentUser/Download/'

# 审计：efi.img 内容 + PE 头 + squashfs 关键文件 + initrd 的 live-boot 机制
docker/podman run … debian:12 bash /work/audit-iso.sh

# 上真机：用我们的 ISO 建虚拟机（框架自建，配置/网卡/设备记录齐全）
./hvm-cli create --name deb12iso --image <ISO> \
    --enhance <oetool.iso> --bios /system/opt/virt_service/virtualized_hwf/stratovirt-uefi \
    --cpu 6 --mem 6 --disk-gb 100
./hvm-cli vmlog -f      # 看 GRUB → 内核 → live-boot → 安装器全流程
```

ISO 里内嵌的 grub.cfg（已实测）：

```
set default=0
linux /live/vmlinuz boot=live components console=ttyAMA0,115200 install=1 quiet
initrd /live/initrd.img
```

`install=1` 由客户机里的 `hvm-install.service` 的
`ConditionKernelCommandLine=install=1` 触发；安装器随后分区、`unsquashfs` 解压、
`grub-install --removable` + `grub-mkconfig`、清发布卫生，最后自动关机。
