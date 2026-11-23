# kebab 用户态路线图（dyld / launchd / Data）

本文是 2026-11-23 的现场记录与计划。判据一律使用面板标记（`osfmk/arm64/kebab_early.c` 的
`kebab_early_mark`），panic 的 `%d`/格式串直接可读，因为 `10063-kebab-console-direct.patch` 之后
dyld 与内核的文字都能落到面板上。

## 1. 已经确认可用的链路

| 阶段 | 判据 | 现状 |
| --- | --- | --- |
| UFS 初始化 + SHNGROOT 解析 | `217` version=1 count=2 | ✅ |
| OS 窗口 → 内层 GPT → APFS 容器 | `220` `EFI PART`、`218` `0x104400/0x16a400000`、`219` `NXSB` | ✅ |
| APFS 根卷挂载 | `212=0`、`222=0` | ✅ |
| 未封签根放行 | `243=0x50` | ✅ |
| 静态信任缓存 | launchd 的 `namespace 9 code 0x8` 消失 | ✅ |
| launchd exec | `250/251/252` | ✅ |
| SystemOS 窗口 + cryptex 卷 | `255`（媒体）、`260=0x100002` | ✅ |
| 离线 SystemOS 挂载 | `270=(6,0)`、`269=0`、无 panic | ✅ |
| 用户态执行 | `256`（ast done）、`254`（EL0 fault，pc≈0x1006axxxx）、`255`（EL0 svc） | ✅ |
| 用户态输出 | `/dev/console` 直写面板：`282` `cnopen direct`、`284` `cnwrite` | ✅ |
| 共享缓存文件可达性 | `env=76 union=? dir=0 file=0 size=491520`（panic 探针） | ✅ |

**关键结论：dyld 不读 `DYLD_SHARED_CACHE_DIR`。** 环境变量确实在（`env=76`），缓存目录与文件也确实可解析
（`dir=0`、`file=0`、`size=491520`），但 libIgnition 仍报 `no cache`。它走的是 ignition 的
boot-cryptex / 默认路径，而不是环境变量。

## 2. 已证伪的假设（不要重复）

| 假设 | 结果 |
| --- | --- |
| 打开 `CONFIG_XNU_OWNER_DATA_MOUNT=y` 只是“缺一个输入” | 构建脚本 `build-xnu-4k-baseline.sh:439` 注释后 `exit 1`，且 `offline-owner-systemos-mount.patch` 里还有 `#error` 要求 `XNU_OWNER_BOOT_MANIFEST=1`；`exit` 不触发 ERR trap，所以“静默” |
| `native_verified_block_volume_device` 返回值是 Unix 风格 | **反了**：调用方与官方实现都是 1/0；这正是 EAUTH(80) 的真因 |
| `/dev/console` 卡在 `km_tty[0]` | `vcattach()` 早就设了 `_constty`，卡的是 constty 分支；而且 `_cnputc` 会把字符交给 `boot_progress_putc()`，根本到不了面板 |
| `DYLD_SHARED_CACHE_DIR` 能让 dyld 找到缓存 | 见 §1 结论 |
| `XNU_OWNER_CACHE_STARTUP=1`（dyld flags bit 3）能绕过 | 编进去了（`_native_owner_cache_dyld_flags`），行为不变 |
| 把 cryptex 挂在 `/private/preboot` 就够 | libIgnition 要的是 preboot 卷下的 `Cryptexes/OS` 子目录（`failed to lookup preboot: 2`） |
| `kebab_skip_ignition_blob=1` 是唯一阻塞 | 旁路后 dyld 直接报 libSystem，说明不是它 |
| union 是 sheng 的实现 | 不是。union 是 XNU 核心 VFS（`vfs_lookup.c unionlookup` + `MNT_UNION`），开关 `CONFIG_UNION_MOUNTS`；`bsd/miscfs/union/` 只有头文件；sheng 是 SM8250 平台驱动族 |

## 3. 阶段计划

### P0 让 dyld 看见共享缓存（当前）

已做：`CONFIG_UNION_MOUNTS` + `XNU_OWNER_UNION_SYSTEMOS=1`，cryptex 以 `MNT_UNION` 挂到 `/`。

判据（`kebab_cacheprobe_panic=1` 的 panic 行）：

```
KEBAB probe env=<n> union=<0|1> dir=<err> file=<err> size=<bytes>
```

- `union=1` 且 `dir=0`、`file=0`、`size=491520`：union 生效、路径可达，问题在 dyld 的 ignition 逻辑
  → 下一步试 `ignition_force_dylib_root`，或把 `/System/Library/Caches/com.apple.dyld` 变成 cryptex 的真实路径。
- `union=1` 但 `dir`/`file` 非 0：union 没有合并目录（root 层同名目录遮蔽）→ 放弃 union，改镜像。
- `union=0`：`MNT_UNION` 没设上，查 `mount()` 返回码（`269`）。

备选 A（不依赖内核）：在 OS 窗口镜像里创建 `/private/preboot/Cryptexes/OS`，把 cryptex 挂到该路径。
需要 `hdiutil convert` 出可写 APFS、`mkdir -p`、重新生成 userdata 并刷 8.2 GB sparse（约 40–60 分钟）。
**这个改动可以和 P2 的 Data 第三窗口合并做，省一次刷写。**

### P1 映射 2.9 GB 共享缓存

硬约束：`board.toml` 的 `dram_size = "0xC0000000"`（约 3 GiB），而缓存主文件 491 KB + 50 余个分片共约 2.9 GB。
mmap 的 VA 不是问题，**resident** 才是。观察点：

- `275`/`276`（SystemOS / OS 窗口读计数）是否暴涨 → dyld 真的在读分片；
- 是否出现 `Kernel data abort`、zone 耗尽、`253` 停止增长（读路径卡住）；
- launchd 是否打出 `launchd logging initialized`。

可能的手段：调 `dram_size`、减少要映射的内容、给 UFS 读路径加批量/预读（当前每 64 KiB 一次同步 DMA）。

### P2 `/private/var`（Data 卷）

两条路，二选一：

1. **scratch Data ramdisk**：`./unixnu runtime --init-data` → `payload --data-ramdisk` → 内核打开
   `XNU_OWNER_DATA_MOUNT`。当前被两处 guard 挡住：构建脚本的 `exit 1` 与 patch 里的 `#error`
   （要求 `XNU_OWNER_BOOT_MANIFEST=1`，它又要求认证挂载）。
2. **userdata 第三窗口**：在镜像里做持久 APFS Data 卷（和 P0 备选 A 合并）。

判据：launchd 能建 `/private/var/run`、`/private/var/db/com.apple.xpc.launchd`。

### P3 launchd 拉起 job

`xpcproxy`、`launchd_cache_loader`、各 daemon 需要更多驱动/服务（显示、触摸、电源、存储策略）。
以周计，不在本阶段的局部修补范围内。

## 4. 镜像登记（boot_b 已刷过的诊断镜像）

| 镜像 | sha256 | 用途 |
| --- | --- | --- |
| `unixnu-kebab-console3.img` | `cb75fddd…` | `/dev/console` 直写面板生效，第一次看到 libIgnition 文字 |
| `unixnu-kebab-cacheprobe.img` | `ff750f61…` | panic 探针：`env=76 dir=0 file=0 size=491520` |
| `unixnu-kebab-openpaths.img` | `c73696f2…` | 24 条 open 路径；看到 `failed to lookup preboot: 2` |
| `unixnu-kebab-nosig.img` | `60ba0e6f…` | `CACHE_STARTUP=1`，行为不变 |
| `unixnu-kebab-igl1.img` | `c3ea91d9…` | `ignition_level=1`，libIgnition 日志消失，仍 no cache |
| `unixnu-kebab-union.img` | `fd2f66be…` | union 挂 `/`，仍 no cache |

**可回退的稳定基线**：`kebabwork/unixnu-kebab-eafix.img`（`eff1aa9d…`，EAUTH 修好、cache mount 成功、
无探针干扰）与 `kebabwork/unixnu-kebab-bigheap.img`（`29ca5330…`，最早能进菜单的固件镜像）。

## 5. 构建与刷机约定

- 构建必须带：`UNIXNU_NO_PRAM=1`、`UNIXNU_GITHUB_PROXY=https://gh-proxy.com/`、
  `UNIXNU_UNIQUE_CHIP_ID`（环境变量，不打印）；
- `prepare-xnu-source.py` 在重新克隆 `xnu-work` 后要联网补 DriverKit 头；网络只有 gh-proxy 前缀可用；
- 编译 dSYM 需要放开文件沙箱（`/dev/fd`）；
- 只刷 `boot_b`，刷机前逐次确认；每次一个变量。
