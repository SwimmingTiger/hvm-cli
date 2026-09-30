# hvm-cli

鸿蒙 PC（HarmonyOS PC）**虚拟机与 Linux 兼容环境控制工具**，纯 C++ 实现。

**不需要 root、不需要 HAP** —— 全部通过 `dlopen` 直接调用系统自带库。

> ⚠️ **必须在系统自带的 HiShell 终端中运行。**
>
> **原因：只有 HiShell 终端在虚拟机白名单内。**
> 虚拟机服务 `vm_manager`（SA 65621）对*每一个*请求都做调用者身份校验，
> 白名单里只放行少数系统身份（系统服务与专用 HAP），
> **能被用户敲命令的终端只有 HiShell 一个**。
> 因此从 MKCode / BitFun / WorkBuddy / CodeArts Agent 等第三方应用的内置终端运行，
> 会在服务端被直接拒绝（日志 `... permission denied`），拿不到任何虚拟机能力。
> 详见[权限模型](#权限模型)；内部细节（调用者身份校验、各放行身份与 uid）
> 见 [维护者笔记](docs/maintainer-notes.md)。

本仓库构建**两个命令**，对应两条完全独立的技术栈：

| 命令 | 用途 | 底层通路 |
|---|---|---|
| `hvm-cli` | 控制**我们自己创建**的虚拟机：状态/能力/电源/快照/共享目录/网络/磁盘/显示 | `vm_manager`（SA 65621）+ StratoVirt |
| `openeuler` | 连入**融合开发引擎**（内部代号 RGM / LinuxFusion）的 openEuler 环境执行命令 | `/system/lib64/ndk/libfusion_pty_ndk.so`（virtio-vsock PTY） |

> 两者互不依赖：`hvm-cli` 走 Binder IPC 到 `vm_manager`；`openeuler` 走 LinuxFusion
> 的 PTY 通道。系统里 `hvm-cli` 的"当前虚拟机"与 `openeuler` 连进去的 openEuler
> 环境**不是同一个东西**（前者由 vm_manager 管理，后者由 LinuxFusion 管理）。

## hvm-cli：虚拟机管理

命令覆盖：`hwf`、`info`、`list`、`vms`、`create`、`start`、`range`、`mount-cd`、`unmount-cd`、`destroy`、`pause`、`lock-guest`、`lx-ota`、`lx-snapshot`、`rgm-status`、`recover-user-data`、`autopause`、`linux-data-delete`、`rgm-image-delete`、`gallery-share`、`guest-disk-share`、`pasteboard`、`screen-lock-task`、`tablet`、`vminfo`、`stratovirt-mem`、`host-sn`、`capability`、`active-name`、`active-status`、`vmstat`、`process-exist`、`feature`、`open-euler-version`、`quick-start`、`is-installing`、`stop`、`force-stop`、`quit-by-reboot-host`、`require-big-mem`、`resolution`、`touch-mem`、`swap-2d`、`net ip|proxy|share-on|share-off|dns-on|dns-off|mode|ports|localhost-ports|proxy-status-on|proxy-status-off|proxy-auto-on|proxy-auto-off`、`share list|enable|disable|add|remove|setup`、`snapshot list|create|restore|destroy|rename`、`disk capacity|path|size|expand|delete-data`、`export`、`import`、以及开发/验证命令（`buffer`、`ctor`、`displays`、`hash-name`、`linux-path`、`perf`、`selftest`、`serial-read`、`serial-write`、`share-volumes`、`view-state`，见[维护者笔记](docs/maintainer-notes.md#7-开发与验证命令)）。

> **常用：导出 / 导入虚拟机磁盘** —— 把某台虚拟机的磁盘导出成文件，或把一个镜像文件导入成一台新虚拟机，见[导出与导入虚拟机磁盘](#hvm-cli导出与导入虚拟机磁盘)。

## openeuler：融合开发引擎里的 openEuler 环境

HiShell 终端有个"openEuler"标签页，能连进 openEuler 执行命令；本命令用的是**同一个
系统接口**（`@ohos:fusion_pty_napi` 背后的 NDK 库）：

```console
$ ./openeuler selftest
dlopen=ok;manager=ok;open=ok;send=ok;image=ok;share=ok

$ ./openeuler exec 'uname -r; cat /etc/os-release | head -2'
6.6.0
NAME="openEuler"
VERSION="24.03 (LTS-SP3)"

$ ./openeuler shell            # 交互式 shell（Ctrl-D 退出）
$ ./openeuler status           # 通道/会话/共享目录概览
$ ./openeuler share status     # 共享目录开关
$ ./openeuler image install    # 安装/更新 openEuler 镜像
```

`exec` 的退出码即远端命令退出码；`--json` 输出 `{"stdout":...,"exitCode":N}`。

`shell` **不做任何行规程**：本地 tty 切 raw 后双向透传字节，行编辑、回显、历史、
补全、Ctrl-C/Ctrl-D 全部由远端 bash/readline 处理；CLI 只额外把本地窗口尺寸变化
同步给远端（SIGWINCH → `SetWinSize`）。

## hvm-cli：虚拟机的创建 / 启动 / 暂停 / 停止 / 删除

> 必须在 **HiShell 终端**里运行（原因见[权限模型](#权限模型)）。
> 下面命令里的路径都是**本机实测可用**的具体取值，换个文件名就能直接粘贴执行。

### 1. 先看可用范围

```console
$ ./hvm-cli range
CPU 数范围     : 6 .. 8
内存范围       : 6 .. 18          # 单位 GB
```

磁盘下限为 65536 MB（= 64 GB），由服务端校验，不在上面这条输出里。

### 2. 创建虚拟机（两种方式）

创建一台虚拟机有两条路：**2.1 从 ISO 全新安装**，或 **2.2 导入现有 qcow2 磁盘**。

#### 2.1 从 ISO 全新安装

```console
$ ./hvm-cli create \
      --name myvm \
      --image   /storage/Users/currentUser/Download/debian-12-unattended-arm64.iso \
      --enhance /storage/Users/currentUser/Download/oetool.iso \
      --bios    /system/opt/virt_service/virtualized_hwf/stratovirt-uefi \
      --cpu 6 --mem 8 --disk-gb 128 \
      --net nat
CreateVm 返回 rc=0 (OK)
```

> `--image` 用**我们自己构建的 ISO**（由 `scripts/build-deb12iso.sh` 产出：debootstrap 目录树
> → mksquashfs → grub-mkstandalone 的 EFI 引导 → El Torito → xorriso，并在 ISO 内放了通过
> 框架 `CreateVm` 客户机类型校验所需的特征串）。构建与踩坑见
> [docs/iso-install-notes.md](docs/iso-install-notes.md)。

- **`CreateVm` 会顺带把虚拟机启动起来**（安装阶段就是这一次）：实测返回 rc=0 之后
  `vms` 里状态立刻是运行中，且**安装盘与扩展盘两张都挂在这一刻**
  —— 所以创建之后通常**不需要**再 `start`（详见 [3.1](#31-光盘怎么挂重要)）。
  用完记得停机：`./hvm-cli force-stop myvm`；
- `--image` 是安装盘 ISO（上例就是我们自己构建的那份）。这个路径会被**自动转换**成媒体库视图
  `/storage/media/100/local/files/Docs/Download/debian-12-unattended-arm64.iso`
  （这是服务端与虚拟机引擎两侧都能读到的写法，转换时会打印一行提示）。
  账号 id（这里是 `100`）取自 `$USER`，多账号设备上第二个账号是 `101`，可用 `HVM_USER_ID` 覆盖；
  媒体库视图的细节见 [维护者笔记](docs/maintainer-notes.md)；
- **`--net nat`（或 `--net bridge`）必须在 `create` 时给** —— 省略它就拿不到网络：
  网络不是磁盘里的东西，因此只把 `--net nat` 加在 `start` 上**没有网卡**，
  实测 `create … --net nat` 的虚拟机才有网卡。桥接用 `--net bridge --nic <宿主物理网卡名>`。
  **限定**：实测通过的是 **NAT**；**桥接还没验证过** —— 此前在 `start` 上加 `--net bridge --nic …`
  一律得到常量错误 `-151060477`，`create` 时未测。
  > 为什么必须在 `create` 时给、桥接还要满足什么条件：内部细节见
  > [维护者笔记](docs/maintainer-notes.md)。
- `--enhance` 是**扩展盘（enhance ISO）**，**创建时必填**且必须是 `.iso` 文件
  （缺省会被服务端拒绝，报 `create vm fail, enhance file path is null`）。
  它对应客户机里的 `unattend` 槽位。**它不能与 `--image` 指向同一个文件** ——
  实测两者填同一个 ISO 会被拒：`CreateVm 返回: unknown (-16842748)`；填成两个不同文件即正常
  （本仓库的用法是安装盘填构建出来的 ISO、扩展盘始终填 `oetool.iso`）；
- 创建成功后会**自动登记**到本地清单
  `/data/storage/el2/base/preferences/hvm-cli-vms.list`（一行一个名字），
  销毁时自动移除 —— 因为服务端**没有枚举接口**（见[能力边界](#能力边界)），
  这份清单是 `hvm-cli list` 枚举的依据 ✓；
- **磁盘不用自己准备**：框架按 `--disk-gb` 生成稀疏的
  `/data/service/el0/virt_service/100/vm_manager/<hash>/myvm/img/vm.qcow2`。

#### 2.2 导入现有 qcow2 磁盘

把**现成的 qcow2**（别处装好的系统、备份出来的磁盘、别人给的镜像）导入成一台新虚拟机。
这条路**不需要**先 `create` —— 导入这个动作本身就会创建虚拟机。

```console
# 1) 把镜像放到服务端读得到的位置：用户下载目录即可，CLI 会自动转换成服务端视图
# 2) 导入：--name 是新虚拟机名（必须尚不存在），--src 是镜像文件的完整路径
$ ./hvm-cli import --name debian13 \
      --src /storage/Users/currentUser/Download/debian12.qcow2
已提交导入：/storage/Users/currentUser/Download/debian12.qcow2 → 虚拟机 debian13

# 3) 导入出来的虚拟机还没有配置参数，启动时必须显式给 CPU / 内存
$ ./hvm-cli start --name debian13 --cpu 6 --mem 6
StartVm 返回 rc=0 (OK)
```

- **导入即建机**：名字必须**尚不存在**（已存在会报目标磁盘已存在），导入成功后会登记进本地清单；
- 导入出来的虚拟机**没有 CPU/内存配置**，所以 `start` 必须带上 `--cpu`、`--mem`；
- 想知道它有没有正常开机，看串口：`./hvm-cli vmlog`（看到 `Debian GNU/Linux ... ttyAMA0`
  登录横幅即为成功）；
- 导入完成后它就和你自己装的虚拟机完全一样：`stop` / `force-stop` / `snapshot` / `disk`
  等命令都能用。

> 进阶用法（自己提供摘要、导出方向、服务端校验细节）见
> [hvm-cli：导出与导入虚拟机磁盘](#hvm-cli导出与导入虚拟机磁盘) 与
> [api-notes 第 11 节](docs/api-notes.md)。

**两条路怎么选**：

| | 2.1 从 ISO 安装 | 2.2 导入 qcow2 |
|---|---|---|
| 适用 | 全新系统、要跑安装器 | 已有现成磁盘；迁移 / 恢复 / 备份还原 |
| 需要准备 | 安装 ISO（可用 `scripts/build-deb12iso.sh` 自建）+ 扩展盘 ISO（`--enhance` 必填，且**不能与 `--image` 同文件**） | 一个 qcow2 + 它的 SHA-256（大写） |
| 建好后的状态 | 创建时已顺带启动过一次（正在安装） | 仍是停止状态，需 `start --cpu/--mem` 才开机 |
| 磁盘 | 框架按 `--disk-gb` 自动生成 | 就是导入的那个 qcow2（导入后会拷进服务区） |

### 3. 启动（开机）

```console
$ ./hvm-cli start --name myvm --cpu 6 --mem 6
```

- `--mem` **至少要给**：服务端不接受内存为 0（只给 `--name` 会返回
  `invalid memory size: 0`）。范围见 `range`（本机是 6..18 GB）；
- 其余参数可以省略：省略的字段服务端用创建时存档的值（实测：省略 `--cpu` 时
  仍按存档的 6 核启动，省略 `--bios` 时仍用存档的固件路径）。

虚拟机启动后，用 `vmlog` 看**客户机串口输出**（GRUB 菜单、内核日志、systemd 启动过程、
登录横幅等）：

```console
$ ./hvm-cli vmlog            # 打印已有的串口输出
$ ./hvm-cli vmlog -f         # 持续跟随（先补上最后 20 行，再跟着刷）
```

`vmlog` 只打印**客户机串口**：stratoVirt 自己的日志行与其它模块的日志都会被过滤掉，
ANSI 控制序列也会去掉 —— 所以不用再 `strings` 一遍。

反过来，如果你想看的是**虚拟机引擎本身**的日志（stratoVirt / hwf_service 自己的记录，
例如引擎报错、光盘打开失败、设备初始化之类），那就直接读原始文件：

```console
$ strings /data/log/hwf_service/vmlog
```

> 两者是同一个文件的两个侧面：客户机的控制台文本就夹在引擎自己的日志行之间
> （落点配置等内部细节见 [维护者笔记](docs/maintainer-notes.md)）。

> 注意：`create` 本身就会启动一次（安装阶段），所以刚创建完的虚拟机已经在跑；
> 这里的 `start` 用于**之后**的启动。挂盘规则见下一节。

启动之后想看这台虚拟机在不在跑，用 `vms`：

```console
$ ./hvm-cli vms myvm
当前虚拟机: myvm
名字                     状态     当前     磁盘镜像
myvm                     9        是       /data/service/el0/virt_service/100/vm_manager/<hash>/myvm/img/myvm.qcow2
```

（状态 9 = 运行中，`vminfo` 还能给出 PID。）

> 光盘 / 网卡到底有没有真的挂上，属于引擎内部行为；怎么在引擎侧确认（验证配方）
> 见 [维护者笔记](docs/maintainer-notes.md)。

> 服务端可能返回 `405 (VM_IP_UNAVAILABLE)`：那只是"启动后立刻查客户机 IP 没查到"，
> **不代表启动失败**，虚拟机通常已经跑起来了。

### 3.1 光盘怎么挂（重要）

挂盘发生在 **`create`**，不在 `start`：

| 操作 | 光盘 |
|---|---|
| **`create`** | 安装盘（`--image`）+ 扩展盘（`--enhance`）**两张都会挂上** |
| **之后的任何 `start`** | **一张都不挂** —— 即使命令行里再传 `--image` / `--enhance` |

也就是说：**`CreateVm` 会顺带完成一次启动**（安装阶段就是这一次），安装介质也只在这一次挂上；
以后再启动要挂盘，用**热插拔**接口：

```console
$ ./hvm-cli mount-cd --name myvm \
      --image /storage/Users/currentUser/Download/oetool.iso
已挂载: /storage/media/100/local/files/Docs/Download/oetool.iso
服务端返回: 1
```

（`服务端返回: 1` 是服务端分配的设备 id，不是错误码。）

> 两点说明：
> 1. 因此 `create` 之后**通常不需要再 `start`** —— 它已经在跑（安装阶段）；
> 2. "`CreateVm` 为什么会启动"**尚未确认**，这里只记录实测到的行为。

> 实测证据（两张盘怎么确认挂上、热插拔命令、`CreateVm` 为什么会启动的线索）见
> [维护者笔记](docs/maintainer-notes.md)。

### 4. 暂停 / 恢复

```console
$ ./hvm-cli pause                  # 暂停当前虚拟机
$ ./hvm-cli resume myvm            # 恢复
```

### 5. 停止

```console
$ ./hvm-cli stop myvm              # 请求客户机自行关机（走客户机里的 GuestAgent）
$ ./hvm-cli stop myvm --clean      # 同上，clean=true
$ ./hvm-cli force-stop myvm        # 强制关机（不需要客户机配合）
```

两者差别**很关键**（实测）：

| 命令 | 生效条件 |
|---|---|
| `stop` | **需要客户机里的 GuestAgent 在线** —— 它本质是"请客户机自己关机"。客户机没起来（例如停在 GRUB）会返回 `405` |
| `force-stop` | 直接关掉，实测任何状态下都能停（我们自己那台停在 GRUB 时也只有它能停） |

> 服务端 API 名与日志线索见 [维护者笔记](docs/maintainer-notes.md)。

### 6. 删除（连磁盘一起删）

```console
$ ./hvm-cli destroy myvm
已销毁 myvm
```

### 7. 日常查看

```console
$ ./hvm-cli list                         # 枚举我们创建过的虚拟机（读本地清单）
$ ./hvm-cli vms                          # 已知虚拟机一览
$ ./hvm-cli active-name                  # 当前虚拟机
$ ./hvm-cli vmstat myvm                   # 状态码（0=未运行 9=运行中；已销毁会说"已不存在"）
$ ./hvm-cli --vm myvm disk path           # 磁盘镜像路径
$ ./hvm-cli --vm myvm disk capacity       # 磁盘容量
$ ./hvm-cli --vm myvm snapshot list       # 快照列表
$ ./hvm-cli --vm myvm net ip              # 客户机 IPv4（需客户机已联网）
```

> **需不需要给虚拟机名？** 写操作（`stop` / `force-stop` / `disk expand` / `share add`
> / `snapshot restore` / `net mode` …）**必须**显式给名字（位置参数或 `--vm <名字>`），
> 省略会直接报错；读操作（`disk path` / `net ip` / `snapshot list` …）可以省略，
> 此时按"当前虚拟机"处理，并会先把你用到的名字打印出来。
>
> **怎么知道一台虚拟机是不是已经被销毁？** 注意状态码 `0` 对「已停止」和「已销毁」
> 是**同一个值**，不能拿它判断存在性。可靠依据是磁盘镜像：
> `list` / `vms` 会把没有磁盘的条目标成 `(无，已失效)`，
> `vmstat <名字>` 也会直接回答 `已不存在（已销毁）`。

安装过程中的客户机文本输出（GRUB 菜单、控制台日志）见
[「3. 启动」](#3-启动开机)一节的 `vmlog` / `vmlog -f` 用法。

### 参数规则（实测确认）

| 参数 | 单位 | 约束 |
|---|---|---|
| `--cpu` | 个数 | 本机 6..8（见 `range`） |
| `--mem` | **GB** | 本机 6..18（见 `range`），且不能为 0 |
| `--disk` / `--disk-gb` | MB / GB | 至少 64 GB，且不超过宿主磁盘 |
| `--bios` | 路径 | 必须存在且可读；如 `/system/opt/virt_service/virtualized_hwf/stratovirt-vars` |
| `--image` | 路径 | 必须存在且**服务端进程**可读，且是 ISO 镜像 |
| `--enhance` | 路径 | 必须存在且可读，扩展名必须是 `.iso`；缺省会导致 `create vm fail, enhance file path is null` |

> 服务端校验的内幕（各校验函数的名称与行为、实测日志）见
> [维护者笔记](docs/maintainer-notes.md)。

#### 路径必须写成「媒体库视图」（`hvm-cli` 会自动转换）

`--image` / `--enhance` 的路径要写成**媒体库视图**形式：

```
/storage/media/<账号 id>/local/files/Docs/Download/x.iso
```

`hvm-cli` 会自动把 `/storage/Users/currentUser/...` 或
`file://docs/storage/Users/currentUser/...` 转换成该形式。

路径里的数字是 **OS 账号 id**，本机 HiShell 终端里就是环境变量 **`$USER`（=100）**；
多账号设备上第二个账号是 `101`，也可用 `HVM_USER_ID` 显式覆盖（便于引用别的账号视图下的文件）。
发生转换时会打印一行提示，例如
`（账号 id=100，取自 $USER（回落到 uid 20020085 / 200000））`。

磁盘**不需要**自己准备 qcow2 —— 框架会按 `--disk-gb` 自行创建：

```
$ ./hvm-cli --vm win11 disk path
/data/service/el0/virt_service/100/vm_manager/<hash>/win11/img/vm.qcow2
```

> **实测已能完整创建虚拟机**（`CreateVm` 返回 0）：当前 VM 变成新名字、
> 框架自动生成磁盘 `.../vm_manager/<hash>/<vm>/img/vm.qcow2`（稀疏，随写增长）。

> ✅ **安装介质能挂上**（实测）：ISO 路径写成上面那种**媒体库视图**即可 ——
> `CreateVm` 返回 0（它顺带完成一次启动），那次启动里安装盘与扩展盘两张都挂上，
> `vmlog` 里 `Permission denied` 计数为 0。

> 内部细节：为什么只有这一种写法可用（服务端与虚拟机引擎两侧的可读性、
> 路径白名单、抓到的第三方虚拟机命令行、账号 id 的推导规则、逐条验证配方）见
> [维护者笔记](docs/maintainer-notes.md) 以及 [`api-notes.md` 第 10 节](docs/api-notes.md)。

## hvm-cli：导出与导入虚拟机磁盘

把某台虚拟机的磁盘**导出**成文件（备份、拿到别的工具里挂载查看），
或者把一个镜像文件**导入**成一台新虚拟机。

```console
# 导出：--src 是「目标目录」（必须已存在），--dst 是「文件名」
#       服务端会写成 目录/文件名，导出完成后可在文件管理器的下载目录里看到
$ ./hvm-cli export --name debian12 \
      --src /storage/Users/currentUser/Download \
      --dst debian12.qcow2
已提交导出：/data/service/el2/100/hmdfs/... -> debian12.qcow2

# 导入：--src 是「源镜像的完整文件路径」
#       （--dst 可省略：省略时本工具会用内置 SHA-256 多线程现算，见下面第 2 点）
$ ./hvm-cli import --name debian13 \
      --src /storage/Users/currentUser/Download/debian12.qcow2
已提交导入：/storage/Users/currentUser/Download/debian12.qcow2 → 虚拟机 debian13

# 进阶：自己给摘要（例如已经在别处算好）。大小写随意，本工具会自动转成大写
$ SHA=$(sha256sum debian12.qcow2 | cut -d' ' -f1)
$ ./hvm-cli import --name debian13 --src .../debian12.qcow2 --dst "$SHA"

# 导入出来的虚拟机还没有配置参数，启动时必须显式给 CPU / 内存
$ ./hvm-cli start debian13 --cpu 6 --mem 6
StartVm 返回 rc=0 (OK)

# 看客户机串口确认是否正常开机
$ ./hvm-cli vmlog
```

三个容易踩的点：

1. **两个方向的参数含义不同** —— `export` 是「目标目录 + 文件名」，
   `import` 是「源镜像的完整文件路径」（摘要可省，见下条）；
2. **摘要可选，且大小写自动处理** —— 不传 `--dst` 时，本工具用**内置 SHA-256** 现算：
   多线程预读 + ARMv8 加密指令（实测约 1.8 GB/s，633 MB 的镜像 0.35 秒算完）；
   传了 `--dst` 也会**自动转成大写**（服务端逐字节比较，小写会被判成「镜像损坏」）；
   单独验证/测速可用 `./hvm-cli sha256 <文件> [线程数]`；
3. **导入用的虚拟机名必须尚不存在** —— 导入这个动作本身就会创建这台虚拟机；
   名字若已存在会报「目标磁盘已存在」。导入之后还要 `start <名字> --cpu N --mem M` 才会开机。

镜像文件放在**用户下载目录**即可，CLI 会自动把它转换成服务端能读到的路径；
导出的磁盘若要在别处挂载，标准 `qemu-nbd` / `qemu-img` 都能直接打开。

## 能力边界

完整的证据与推导见 [`docs/api-notes.md` 第 13 节](docs/api-notes.md)。一句话版本：

| 想要的能力 | 结论 |
|---|---|
| 在 HiShell 终端里调 vm_manager（借 HiShell 身份过白名单） | ✅ 可以 |
| 创建/启动/停机/销毁**我们自己**的虚拟机 | ✅ 可以 |
| 给自己的虚拟机挂安装盘（ISO 放在下载目录） | ✅ 可以（路径会自动转成媒体库视图） |
| 查询/管理**别的应用**的虚拟机（如 OSEasy 那台） | ❌ 服务端按应用隔离，只能看到名字 |
| 看到自己虚拟机的**画面**、给它**发按键** | ❌ 画面要合成进"应用窗口"，而窗口只有 UIAbility 能创建 |
| 自己写 HAP 绕过上面两条 | ❌ appIdentifier 不在白名单 |
| 让系统四指右滑进自己虚拟机的全屏 | ❌ 同上（系统合成的对象是应用窗口） |
| 读客户机的**文本**控制台（GRUB、安装器输出） | ✅ 可以（客户机串口落在 `/data/log/hwf_service/vmlog`；用 `./hvm-cli vmlog` 读；引擎自身日志用 `strings` 读同一文件） |
| 多台虚拟机并存 / 同时只运行一台 | ✅ / ⛔ 服务端限制 |

> 因此"交互式装系统 + 看画面"只能用**厂商合作应用的界面**（OSEasy / Sanway）；
> 本仓库负责**自动化控制我们自己的虚拟机**。

## 权限模型

`vm_manager` 对每个请求做调用者校验：白名单里只放行少数系统身份
（系统服务与专用 HAP，外加系统自带的 HiShell 终端），因此从 **HiShell** 启动的进程可以直接调用：

> 不 root、不做 HAP，直接调用系统自带的库，权限由**进程身份**决定。

反过来，从 MKCode / BitFun / WorkBuddy / CodeArts Agent 等第三方应用的内置终端
启动会被拒绝（`permission denied`）。

**结论：本工具只能在系统自带的 HiShell 终端里运行 —— 因为白名单里能被用户使用的
终端只有它一个**（其余放行身份都是系统服务或专用 HAP，用户无法在其中开终端）。

> 内部细节（调用者校验、各放行身份与 uid、服务端日志格式）见
> [维护者笔记](docs/maintainer-notes.md)。

## 构建

```bash
make              # 同时生成 hvm-cli 与 openeuler
make check        # 分别自检两条通路
make check-headers  # 语法检查 include/ 里的公共头文件
make syms         # 重新生成 vm_manager_kits.h / .syms.h
make symcheck     # 核对生成的 mangled 名与设备符号快照是否 121/121 一致
make check-repo   # 确认仓库里没有二进制文件
make install      # 可选，装到 ~/.local/bin
```

## 生成最小 Debian 12 镜像（可导入、可发布）

`scripts/build-deb12min.sh` 在一台 **arm64 Linux** 上从零做一个最小 Debian 12 磁盘
（qcow2）：只含基础系统 + 内核 + GRUB + sshd，可直接导入本工具当虚拟机用。

```bash
# 在一台 arm64 Debian/Ubuntu 上（需要 qemu-utils debootstrap gdisk dosfstools parted）
sudo apt-get install -y qemu-utils debootstrap gdisk dosfstools parted
# 用法：[输出路径] [整盘虚拟大小] [根文件系统]
sudo scripts/build-deb12min.sh /tmp/deb12min.qcow2 8G          # 默认 ext4
sudo scripts/build-deb12min.sh /tmp/out.qcow2     100G btrfs   # 根用 btrfs，整盘 100G

# 把 qcow2 放到设备上，然后导入并启动（名字必须尚不存在）
./hvm-cli import --name deb12min --src /storage/Users/currentUser/Download/deb12min.qcow2
./hvm-cli start  --name deb12min --cpu 6 --mem 6
./hvm-cli vmlog -f          # 看串口：出现 "Debian GNU/Linux 12 … ttyAMA0" 即成功
```

镜像内容与设计（细节见脚本头部注释）：

- **分区**：512 MiB ESP（FAT32）+ 根分区，并放好 `EFI/BOOT/BOOTAA64.EFI`（stratovirt 走 UEFI 引导）；
  **根分区自动用满 ESP 之外的全部剩余空间**（脚本会把算出来的大小打印出来）；
- **根文件系统**：第 3 个参数指定，支持 `ext4`（默认）/ `xfs` / `btrfs` ——
  脚本会按类型选择 `mkfs` 参数、fstab 选项与 fsck pass，并在客户机里附上对应的维护工具
  （`xfsprogs` / `btrfs-progs`）；配好 fstab 后会**重建一次 initramfs**，
  确保根文件系统的驱动真的在 initrd 里；
- **内核参数**：`console=ttyAMA0,115200`（串口，`vmlog` 读的就是它）
  加 `modprobe.blacklist=vmwgfx`（屏蔽该模块，与 `modprobe.d` 里的 blacklist 双保险）；
- **网络**：`systemd-networkd` 配**静态 IP** `172.16.100.2/24`、网关 `172.16.100.1`、
  DNS `114.114.114.114`（框架的虚拟网络就是 `172.16.100.0/24`、网关 `.1`；**不设 DHCP**
  —— 实测框架侧不保证提供 DHCP）。可用 `VM_IP` / `VM_GW` / `VM_DNS` 覆盖；
- **登录**：默认 `root` / `root`（可用环境变量 `ROOT_PASS` 覆盖 —— **发布前请务必改掉**）；
- **发布卫生**：清空 `/etc/machine-id`、删除预生成的 SSH 主机密钥，并加一个 `ssh-keygen -A`
  的 drop-in（先用空赋值清空主单元继承来的 `ExecStartPre`，再用 `/bin/sh -c` 走 PATH 调用
  —— Debian 的 `ssh-keygen` 在 `/usr/bin`，写死 `/usr/sbin` 会让它静默失败），
  让每台实例首启生成自己的密钥；
- **体积**：只装 `--no-install-recommends`，装完清 apt 缓存与 lists，最后用 `fstrim`
  配合 `qemu-nbd --discard=unmap` 把已删文件的块真正还给 qcow2。

虚拟大小默认 8 GiB；之后需要更大空间可用 `./hvm-cli --vm <名字> disk expand <GB>` 扩容。


## 维护者文档

只对维护者 / 逆向工作有意义的内容已从本 README 移出：

| 文档 | 内容 |
|---|---|
| [`docs/maintainer-notes.md`](docs/maintainer-notes.md) | README 移出的内部细节：服务端参数校验内幕、网络字段分配、光盘挂载引擎内幕、媒体库视图为什么只有一种写法、验证配方、**调试（lldb）**、开发与验证命令、目录结构、实现状态 |
| [`docs/api-notes.md`](docs/api-notes.md) | 逆向笔记 §1–§14：白名单、ABI 陷阱、线上格式、安装介质路径、通道、能力边界、接口覆盖 |
| [`docs/iso-install-notes.md`](docs/iso-install-notes.md) | 自建 squashfs 安装 ISO 的设计与构建踩坑 |
| [`include/README.md`](include/README.md) | 逆向还原的公共头文件 |

