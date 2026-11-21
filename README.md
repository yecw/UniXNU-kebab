# UniXNU-kebab

一加 8T（kebab，高通 SM8250）的 UniXNU 设备仓库。

## 状态（2026-11-22）

在 KB2000（12+256，已解锁）上实测。

| 阶段 | 状态 |
| --- | --- |
| Mu-Silicium UEFI、菜单、payload 卷、DarwinBoot | 真机验证通过：菜单渲染、按键、payload 读取、hand-off、`ExitBootServices` 全部正常 |
| 内核早期（`arm_vm_init` → `machine_startup`） | 通过。视频/ramoops 都在 physmap 窗口外，一律改用 `ml_io_map`，不再用 `ml_static_ptovirt` |
| 控制台 | 面板控制台可用（`kebab_early`，字体 1 倍、60 行滚动，`kebab-probe=1` 打开逐节点明细） |
| `machine_lockdown` | 通过（跳过会触发 `pmap_assert_free` 的低地址 `ml_static_mfree`） |
| `PE_init_iokit` / `InitIOKit` / `ConfigureIOKit` | 通过（ADT 不再输出 `interrupts`/`interrupt-parent`；`/cpus` 带 AppleARMCPU 需要的 `cpu-id`、`no-aic-ipi-required`） |
| `PE_lockdown_iokit` → `kperf_init_early` | 通过 |
| `bsd_init` | 通过。`throttle_init`、`printf`、`kauth_init`、`vfsinit`、`bsd_autoconf`、`net_init_run`、`inittodr` 都返回 |
| UFS | **打通**：`ufs_take_over_host()` 成功、GPT 扫描找到 `userdata`（返回 0） |
| `vfs_mountroot` | 返回 `0x13`（ENODEV）：`userdata` 里是 Android 的 f2fs/ext4，还没有 APFS 容器 |
| 根文件系统 | 未挂载。需要把 iOS 根（`OS` + `SystemOS`）写进 `userdata` |
| USB CDC 控制台（固件侧） | 控制器状态全对（device、RunStop=1、PHY 未挂起、link 已起）但主机看不到设备；EUD 已排除，剩余嫌疑是 Type-C/PD 角色状态 |
| USB 日志通道 | 建议改走内核侧 `QcomXnuUsbDebug`（初始化点在 `PE_init_kprintf`，早于当前所有卡点） |

## UFS 的关键修复

内核侧 `bsd/dev/kebab_ufs.c` 一直卡在 `UTRLRSR=1` 之后的首次取指（读寄存器直接挂死）。根因不是时钟、不是 16 KiB 页、也不是 apps SMMU：

- 本固件把 vendor `UFSDxe` 注释掉了，而它是唯一给 UFS 建 IOMMU 域的组件；
- 没有域 → SID `0x4E0/0x0E0` 没有 SMR 条目 → 第一次取请求表就 fault → 事务 stall → 下一次寄存器读挂死总线；
- 修法：在固件里用 HAL_IOMMU（`54B6D3B4-…`）`create → configure(dom,0) → attach(\_SB_.UFS0, ARID 0x00/0x10)`，再做停表、清门铃、`HCE`、`DME_LINKSTARTUP`（`CdcUfsIommuSetup` / `CdcUfsQuiesce`，在 `UsbCdcConsole` 里执行，输出可见）。

修好后内核一次越过该点，`ufs_find_partition()` 返回 0。

## 构建

```bash
cd ../UniXNU
PATH=/opt/homebrew/bin:/opt/homebrew/opt/llvm/bin:$PATH \
  ./unixnu kernel  -D ../UniXNU-kebab-stevedang --uefi ../Mu-Silicium \
                   --edk2 ../Mu-Silicium/Mu_Basecore --os ipados-17.7.11-21H461 \
                   --ipsw ../ipsw/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw \
                   --target DEBUG -j 10
PATH=/opt/homebrew/bin:/opt/homebrew/opt/llvm/bin:$PATH \
  ./unixnu payload -D ../UniXNU-kebab-stevedang --os ipados-17.7.11-21H461 \
                   --ipsw ../ipsw/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw \
                   --target DEBUG -j 10
```

生成 ADT 时必须带 `UNIXNU_NO_PRAM=1`（`/pram` 在 `0x17F00000`，读它会挂这块板）。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `board.toml` | 板级描述、donor IPSW、内核 archive、payload 与 boot image 参数 |
| `configs/kebab_defconfig` | 最小内核配置 |
| `dts/kebab.dts` | 硬件设备树（ADT 的输入） |
| `patches/xnu/` | 10063 内核补丁系列（`series` 是顺序表） |
| `scripts/` | archive builder、ADT 生成器、entry/panic 探针、PMGR 桥与 boot progress 的 builder |
| `package/unixnu.toml` | 设备 kit 的 UniXNU 启动菜单配置 |
| `package/BootRamoops.c` | boot progress 的 ramoops 写入与屏幕检查点方块 |
| `docs/bring-up.md` | 真机上量到了什么、启动经历了什么 |
| `drivers/KebabRamDisk` | 子模块：把固件内嵌 payload 暴露成 RAM disk 的 UEFI 驱动 |
| `unixnu.lock` | `unixnu fetch` 写入的源锁文件 |

## 探针标记

内核早期阶段在面板上打编号，看到最后出现的编号就知道停在哪：

| 编号 | 位置 |
| --- | --- |
| 8–28 | `PE_init_platform`、`arm_vm_init` |
| 86–100 | 视频基址、`boot_progress_start` |
| 101–117 | `PE_create_console`、`PE_init_printf`、`cpu_machine_idle_init`、看门狗 |
| 111/115 | `PE_init_platform(TRUE)`、`machine_startup` |
| 120–131 | `PE_init_iokit`、commpage、`mac_policy_initmach`、`machine_lockdown`、`PE_lockdown_iokit` |
| 132–143 | `arm_vm_prot_finalize`（低地址释放、XRAM） |
| 150–155 | `PE_init_iokit` 内部、`InitIOKit`、`ConfigureIOKit` |
| 163–171 | `IODeviceTreeAlloc`（`kebab-probe=1` 时逐节点打印） |
| 172–188 | `bsd_init`（`throttle_init` 到 `inittodr`） |
| 190–212 | UFS 初始化与 `vfs_mountroot` |

## 已知限制

- `userdata` 仍按 Android 分区使用，没有 APFS 根；
- USB CDC 的固件侧被 Type-C/PD 状态挡住，暂不做；
- 内核参数里 `debug=0x14e`、`startup_debug=1` 仍保留（调试用）。
