# 补丁总目录与使用计划

三条来源，按“什么时候用”组织，而不是等出问题再翻。

## A. 我们自己的 kebab 设备补丁（`patches/xnu/`，120 个）

按功能分组；标注 **[保留]** / **[临时探针]**（默认静音，`KEBAB_PROBE_VERBOSE` 可全开）。

| 组 | 代表补丁 | 用途 | 去留 |
| --- | --- | --- | --- |
| 存储/窗口 | `10063-kebab-ufs-*.patch`、`-vroot-objects`、`-manifest-weak` | SHNGROOT 解析、窗口划分、v2 data object | 保留 |
| owner 验证根 | `10063-kebab-boot-manifest`、`-carrier-path`、`-root-binding` | 签名头/哈希表、boot manifest、carrier 路径 | 保留 |
| dyld 缓存 | `10063-kebab-dyldcache`、`-dsc-markers*`、`-dsc-bootarg`、`-dsc-order` | 拆分缓存的解析/映射计划 | 保留 |
| 签名兜底 | `10063-kebab-signoffset-fallback`、`-cdhash-probe`、`-blobadd-probe`、`-blobstage-probe`、`-blobstruct-probe` | fcntl 注册路径诊断与偏移兜底 | 保留（诊断可静音） |
| 控制台/探针 | `10063-kebab-console-direct`、`kebab-early-*`、`-quiet-probes` | 面板日志 | 保留 |
| 其余 bring-up 探针 | `-afterbp-probe`、`-allocpt-probe`、`-avail-marks`、`-probe2`、`-openpaths`、`-unionprobe`、`-cssig-probe`、`-dsc-gate` 等 | 已证实的早期卡点 | 合并/删除候选 |

## B. 官方 UniXNU 补丁（`UniXNU/patches/xnu/`，59 个）

| 组 | 补丁 | 用途 | 状态 |
| --- | --- | --- | --- |
| 4K/ABI | `native-4k-user-abi`、`native-4k-bootstrap-capacity`、`native-4k-signature-hash-cache` | 4 KiB 页内核 | 已用 |
| 平台/架构 | `armvirt-platform-personality`、`native-arm-pan`、`non-apple-pmap-field`、`non-apple-iommu-bootstrap`、`zx-pmap-sync-64bit-pa` | 非 Apple SoC | 已用 |
| 存储/信任 | `native-trust-cache-runtime`、`null-root-media` | 信任缓存运行时 | 已用 |
| union | `native-union-lower-directory`、`native-union-getattrlistbulk` | union 的 lookup / 批量属性 | **已在树里**（`vfs_attrlist.c` 有 `unionread`，`vfs_lookup.c` 有 lower-dir 修正） |
| owner 链 | `offline-owner-systemos-mount`、`offline-owner-cache-carrier`、`offline-owner-boot-manifest`、`offline-owner-root-binding-audit`、`offline-owner-root-crypto-probe` | 缓存挂载与认证 | 已用（BOOT_MANIFEST 已验证） |
| 用户态辅助 | `offline-legacy-start`、`offline-component-load-probes`、`zzy-staged-system-apps`、`zzyb-native-cryptex-os-view` | 启动/限制 | 按需评估 |
| AMFI 放宽 | `zzyc-owner-skip-launch-constraints` | 加 `no-lc=1` 清掉 AMFI 的 Launch Constraint 检查 | **已在树里**，需要时加参数 |
| 其他 | `sheng-platform-power`、`source-corecrypto14-bootstrap`、`zzz-boot-progress` | 电源/加密/进度 | 已用 |

## C. 参考 kebab 移植（`refs/UniXNU-kebab/patches/xnu-8796-kebab/`，463 个）

只列与当前阶段相关的；其余是触摸/WiFi/RPMh/安装等外设。

| 补丁 | 作用 | 我们的用法 |
| --- | --- | --- |
| `0102` | 拆分 DSC 解析 + 映射计划 | **已移植** |
| `0150` | `kebab_dsc` 按字符串解析 | **已应用**（等价修复） |
| `0153` | 目录扫描丢尾部（`sizeof(struct dirent)` 边界） | 计划 ok 后若子缓存 < 52 再加 |
| `0142` | 映射 dyld 读取的 dynamic-config 页 | 映射成功后 |
| `0103` / `0137` | slide 支持 / pager slide-info | dyld 要求非零 slide 时 |
| `0143` | 计划日志 opt-in | 面板不够用时 |
| `0152` | `CS_PLATFORM_BINARY` 传播到进程 | launchd 报 “not a platform binary” 时 |
| `0154` | 已验证签名传播 `CS_SIGNED` | `cs_identity_get` 失败 / `namespace 7` 时 |
| `0100` | 自研 codesign 策略 | 决定替换 Apple CS 时 |
| `0088` | 内置 AMFI | 决定替换 Apple AMFI 时 |
| `0226` | 内核按缓存头注册 DSC 签名 | 签名注册仍不稳时（比我们的兜底更完整） |
| `0222` | owner 挂载后处理 DSC | 已用其思路（我们挪了 `kebab_dsc_boot`） |
| `0144` | launchd 从 `/usr/libexec` 起任务 | launchd 起来后 |
| `0145`/`0146` | `/dev/console` 路由 | 用户态日志 |
| `0131`/`0148`/`0149` | USB 安装流/持久化 | 若要换成可写 HFS+ 根 |
| `0080`~`0086` | HFS+ 根 | 若要放弃 APFS cryptex 方案 |

## D. 分阶段计划

### D0. 已完成

- 4K 内核、UFS/窗口、APFS 根、静态信任缓存、console。
- owner 验证根：签名头 + 哈希表（userdata `.vroot`）+ boot manifest（`/chosen/boot-manifest-hash`）。
- 共享缓存可被 dyld 打开（union + carrier）。

### D1. 当前：拆分 DSC 的解析与映射

1. `kebab_dsc_boot()` 已挪到 union 挂载之后（否则 ENOENT）；
2. 看 dry-run：`309 plan ok` 且 `subcache_count == 52`；
3. 若子缓存数不足 → 加 `0153`（扫描边界）；
4. 计划 ok 后开 `kebab_dsc_map=1`，观察 `vm_shared_region_map_file` 的 errno；
5. 若 dyld 还要求 dynamic-config 页 → 加 `0142`；要求 slide → `0103`/`0137`。

**判据**：面板出现 mapping complete，或明确的 errno。

### D2. union 的目录语义（已具备）

`native-union-lower-directory` 与 `native-union-getattrlistbulk` **已在当前树里**：
- `vfs_lookup.c` 有 lower-dir 修正（上层是符号链接时走下层目录）；
- `vfs_attrlist.c` 有 `unionread` 标签（`getattrlistbulk` 会继续读下层）。

因此 `kebab_dsc` 扫描不到目录只可能是**时机**问题（union 尚未挂载），不是 union 语义缺失。
这正是把 `kebab_dsc_boot()` 挪到 `cache_prepare` 之后的依据。

### D3. 下一关：进程码签名标志

dyld 映射成功后，launchd 可能报：
- `boot task is not a platform binary` → 加 `0152`；
- `failed to get the code signing identity` / `namespace 7` → 加 `0154`。

顺序：先 `0154`（CS_SIGNED），再 `0152`（CS_PLATFORM_BINARY），因为前者是后者的前提。

### D4. AMFI 策略（若仍拒绝）

已经可用但尚未打开的放宽开关：`no-lc=1`（`zzyc-owner-skip-launch-constraints`，已在树里），
它清掉 AMFI 的 "Launch Constraint Violation"。


若 cache/进程仍被 AMFI 拒绝，且信任缓存确认包含其 CDHash：
- 先试 `0226`（内核按缓存头注册签名）；
- 再考虑 `0100` + `0088`（整层替换）；需要把 Apple AMFI 从 prelink roots 移除。

### D5. launchd 与用户态

launchd 能起来后：`0144`（`/usr/libexec` jobs）→ `0145`/`0146`（console 路由）→ 视需要 `0131`/`0148`/`0149`（安装流）或 `0080`~`0086`（HFS+ 根）。

## E. 维护规则

1. 新补丁必须能对应到 D0–D5 的某一阶段和判据；
2. 探针默认静音，只在对应阶段打开；
3. 每阶段结束后把已证实的探针合并或删除，保持 series 可读；
4. 参考仓库补丁**先读再抄**，不按印象改 Apple 代码。
