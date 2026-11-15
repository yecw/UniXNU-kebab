# UniXNU-kebab

一加 8T（kebab，高通 SM8250）的 UniXNU 设备仓库。

## 状态（2026-11-15）

在 KB2000（12+256，ColorOS 14，已解锁）上实测。

| 阶段 | 状态 |
| --- | --- |
| Mu-Silicium UEFI、菜单、payload 卷、DarwinBoot | 已在真机验证：菜单正常渲染、按键可用、payload 的 `unixnu.toml` 和两个内核文件都能打开，hand-off 报告 EL1、4 KiB 粒度支持、`ExitBootServices` 成功 |
| 内核入口 | 已在真机验证：entry probe 画出绿、蓝、洋红三条色带（内核入口、MMU 打开） |
| MMU 之后的内核 | 停在 `arm_vm_init` 返回之前（boot progress 的第一个检查点方块都没出现） |
| PMGR 桥 | 已链接（`CONFIG_XNU_SHENG_PMGR`）；`package/ShengPlatformPower.cpp` 和 `scripts/build-platform-power.py` 来自 `UniXNU-pipa`，不是本仓库的原创 |
| panic 日志区 | 已编译进去（`CONFIG_XNU_DT_PANIC_REGION`）；`scripts/patch-xnu-entry-probe.py --panic-at` 可在指定符号处调用 `panic()`，panic screen 用保留的 boot video 记录绘制（`patches/xnu/10063-kebab-panic-vinfo.patch`） |
| boot progress | 已移植并编入内核（ramoops 环 + 屏幕彩色检查点方块，`patches/xnu/10063-kebab-boot-progress*.patch`），但第一个方块要等 `arm_vm_init` 之后才会画，当前还看不到 |
| 控制台 | 未到达；把 framebuffer 标记成物理地址、或换成物理 aperture 的 `phystokv` 地址都不能越过它 |
| UFS root | 还没在真机上确认 |

2026-11-15 这天加了 PMGR 桥、`XNU_DT_PANIC_REGION`、`armcpu-*`/`sheng-pmgr`/`debug=0x14e` 内核参数、panic 探针、framebuffer aperture 路径和官方 boot progress 的移植。得到两条确定的事实：

- `PE_create_console()` 在 `_arm_init` 里**先于** `machine_startup()` 执行（顺序是 `arm_vm_init` → `serial_init` → `PE_create_console` → `PE_init_printf` → `cpu_machine_idle_init` → 第二次 `PE_init_platform` → `machine_startup`），所以放在 `machine_conf` 的探针永远打不中。
- XNU 的 `initialize_screen()` 只有在 **bit 0 置位**时才把 `v_baseAddr` 当物理地址，否则当虚拟地址用。DarwinBoot 现在会置这个位，entry probe 画色带前会先屏蔽它（带标记的地址在 MMU 关闭时非对齐写会 fault）。

更早的 2026-11-01 结果是：二进制 GIC handler 装上、第一个 PPI-27 中断触发，但虚拟定时器不重复；最后一行可见输出是 `Unable to find vm tag ?+z for btlog`。现在走的是 GFander 的源码级 GICv3（`HAS_GIC_V3` + Group-1 `sleh_irq`）路线。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `board.toml` | 板级描述、donor IPSW、内核 archive、payload 与 boot image 参数 |
| `configs/kebab_defconfig` | 最小内核配置（DT platform，设备驱动还没接） |
| `scripts/` | archive builder、kebab ADT 生成器、entry/panic 探针、PMGR 桥与 boot progress 的 builder |
| `package/unixnu.toml` | 设备 kit 的 UniXNU 启动菜单配置 |
| `package/ShengPlatformPower.cpp` | `CONFIG_XNU_SHENG_PMGR` 链接的原始 PMGR 桥 |
| `package/BootRamoops.c` | boot progress 的 ramoops 写入与屏幕检查点方块 |
| `docs/bring-up.md` | 真机上量到了什么、启动经历了什么 |
| `drivers/KebabRamDisk` | 子模块：把固件内嵌 payload 暴露成 RAM disk 的 UEFI 驱动 |
| `unixnu.lock` | `unixnu fetch` 写入的源锁文件 |

## 构建（macOS 或 GitHub 的 macOS runner）

```sh
./unixnu defconfig kebab_defconfig -D ../UniXNU-kebab
./unixnu fetch
./unixnu ipsw fetch
./unixnu build kernel
```

产物：`out/kebab/kernel/kernel.macho`。

没有 Mac 时，fork `UniXNU/UniXNU`、开启 Actions，用 **macOS build** workflow：

| 输入 | 值 |
| --- | --- |
| `device_repository` | `<your-user>/UniXNU-kebab` |
| `defconfig` | `kebab_defconfig` |
| `steps` | `kernel` |
| `xcode` | 默认 |

## 能启动的镜像配方（实测）

固件卷由 Mu-Silicium 编译，再打进 Android boot image。两个细节决定手机有没有画面：

1. **DTB 必须是手机自己的 live DTB。** Mu-Silicium 自带的 `Resources/DTBs/kebab.dtb` 没有 `cont_splash_region@9c000000`，ABL 会打 `ERROR: Could not get splash memory region node`，然后不把 framebuffer 交给固件：屏幕停在原生开机 logo。请从运行中的机器 dump live tree，并用它作 `--dtb`。
2. **header v2 / 4096 字节页**，kernel = BootShim + `SILICIUM_UEFI.fd`，`--kernel_offset 0x8000 --tags_offset 0x100 --dtb_offset 0x1f00000`。

这台机器的 ABL **不支持 `fastboot boot`**（会报 `Failed to load/authenticate boot image`）；要刷到 `boot_b`（或 `boot_a`）再重启。

## 设备参数

| 项目 | 值 | 来源 |
| --- | --- | --- |
| SoC | SM8250（SD865） | live DTB |
| 调试串口 | **`0xA90000`**（QUPv3 SE12，GPIO34/35，115200 8N1） | live DTB `earlycon=msm_geni_serial,0xa90000` |
| 第二 console 节点 | `0x00988000` 是 `status = "disabled"` | live DTB |
| Framebuffer / splash | `0x9C000000`，`0x2300000`，1080 × 2400 | live DTB `cont_splash_region@9c000000` |
| 面板 | `oplus20828samsung_amb655x_1080_2400_cmd`（三星 AMB655X，command mode） | live DTB |
| 触摸 | Synaptics S3908，i2c `a94000` 地址 `0x4B`，IRQ GPIO39，reset GPIO38 | live DTB |
| 音量键 | PM8150 GPIO7（下）/ GPIO6（上） | mainline kebab DTS |
| UEFI FD | base `0x9FC00000`，size `0x300000` | Mu-Silicium `kebab.toml` |
| `userdata` 大小 | 233,492,557,824 字节（57,005,019 × 4096） | 12+256 机器上的 `fastboot getvar partition-size:userdata` |
| `userdata` 起始 LBA | 4,748,320（KB2000 9008 包的 GPT） | `gpt_main0.bin` |

## 剩余工作

* **`arm_vm_init` 之前的内核停顿**：entry probe 的色带能出，boot progress 的检查点方块一个都不出，说明 `arm_vm_init` 还没返回。下一步要在这里做二分（`boot_progress_stage` + 惰性方块，或官方记录的 `--reset-after arm_vm_init`）。
* **定时器 / GIC**：GIC handler 和第一个 PPI-27 中断能工作，但虚拟定时器不重复。`CNTV_TVAL_EL0`/`CNTV_CTL_EL0` 在 `rtclock_intr` 之后会重装，跳过 `rtclock_intr` 也一样停住。先解决周期 tick，再谈 UFS。
* **UFS**：`bsd/dev/kebab_ufs.c` 已链接并提供 BSD 的 `ufsroot0` 钩子，ADT 有根 `/kebab-ufs` 节点，启动参数记在 `board.toml`。root 发现还没走到。
* **内核显示 / 触摸驱动**，以及 `userdata` 镜像刷写。
* **Advanced Logger（logDxe）**：`AdvancedLogDxe` 已编进固件，但缺生产者（Mu 只剩 Rust 版）；要 C 版实现 `ALOG v6` + `ALM2` 消息 + 协议 container，并把 `DebugLib` 路由到 `BaseDebugLibAdvancedLogger`、给 framebuffer 串口写 `AdvancedLoggerHdwPortLib` 包装层，约 1.5~2 小时。

## 许可

BSD-2-Clause-Patent，与 UniXNU、Mu-Silicium 一致。

## 移植记录

[`porting.toml`](porting.toml) 是权威记录：13 个领域，每个都有实测事实、完成度和证据。改移植之前先读它，每次改动同步更新对应领域：

```sh
python3 scripts/porting-record.py check --device ../UniXNU-kebab
```

[`AGENTS.md`](AGENTS.md) 记着构建命令和两个"重新发现很贵"的固件事实，[`docs/bring-up.md`](docs/bring-up.md) 保留按时间顺序的测量记录。不在任何仓库里的输入列在 [`proprietary-files.txt`](proprietary-files.txt)；[`local.toml.example`](local.toml.example) 说明工作站需要的本地路径。
