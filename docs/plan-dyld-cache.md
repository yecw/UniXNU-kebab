# 路径 A：让 dyld 自己映射共享缓存（sheng 的做法）

目标：dyld 正常调用 `shared_region_map_and_slide_2_np()` 把拆分缓存映射进 shared region，
从缓存里解析出 `libSystem.B.dylib`，launchd 开始执行。

sheng/alioth 就是这么做的：它**没有**内核代映射的模块，靠已验证根 + `OWNER_CACHE_STARTUP`
（commpage `dyld_flags` bit 3）+ `OWNER_BOOT_MANIFEST` + `no-lc=1`，然后由 dyld 自己映射。

## 现状（2026-11-26 00:55）

| 事实 | 证据 |
| --- | --- |
| 拆分缓存可解析 | 真机 `309 plan ok`；61 条映射、`0xc45ec000` 字节 |
| 内核代映射不可行（当前） | `319 = ENODEV`：pid 1 exec 时 `vm_shared_region_trim_and_get()` 为 NULL（10063 不预建共享区） |
| dyld 的签名注册曾失败 | `dyld cache '(null)' not loaded: 0004 code signature registration ... failed`；`296 = 0x61 0x55` |
| 我们已做偏移兜底 | `289` 命中后 `293` 出现 `error=0`，但**缓存本身**那次仍失败 |
| AMFI 未拒绝 blob | `294` 从未出现 |
| 结构校验失败 | `290 blob structural`：error `0x55`、size `0x4000`、magic `0x00000000` |

## A0. 先确认 dyld 走到哪一步（不改行为，只加探针）

在 `bsd/vm/vm_unix.c` 加三个标记：

| 标记 | 位置 | 打印 |
| --- | --- | --- |
| `320` | `shared_region_map_and_slide_setup()` 入口 | `proc_getpid(p)`、`files_count`、`mappings_count` |
| `321` | `rdir_vp != rootvnode` 的 EPERM 分支 | `rdir_vp != sr_vnode` 的结果、`rdir_vp` 与 `rootvnode` |
| `322` | `vm_shared_region_map_file()` 返回后 | `kr` |

判据：

- 没有 `320` → dyld **根本没走到映射**（签名注册仍失败，回 A1）；
- `320` 有、`321` 有 → 根/SIP 门禁拒绝（回 A2）；
- `320` 有、`322 != 0` → 映射本身失败（回 A3）；
- `322 == 0` → 映射成功，进入 launchd 阶段（B 阶段）。

## A1. 若 dyld 从未调用（签名注册失败）

优先级从低风险到高风险：

1. **确认是哪一次 fcntl 失败**：给 `293` 增加 vnode 名或 fd 的路径标记（v_name 常为 NULL，
   改用 `vn_getpath()` 的最后一个分量长度 + 首字符），区分“缓存文件”和“普通 dylib”；
2. **把偏移兜底扩展到所有文件**：现在只在“读回全零 + 文件头是 dyld_v1”时重试；
   若缓存文件的签名读回不是全零而是错位，需要按 `fs_blob_size` 与头里的
   `code_signature_size` 比对后重读；
3. **内核侧注册（`0226` 的做法）**：在 `kebab_dsc_boot()` 里对主缓存和每个子缓存
   调用 `ubc_cs_blob_add`，签名按头里的 `codeSignatureOffset/Size` 读取；
   这样 dyld 的 fcntl 会发现 blob 已存在，跳过注册。

## A2. 若根/SIP 门禁拒绝

`shared_region_map_and_slide_setup()` 在 `sr_vnode == NULL` 时要求 `rdir_vp == rootvnode`。
我们是 union 根（cryptex 覆盖在 `/` 上），`rdir_vp` 应等于 `rootvnode`；若不等，
说明 dyld 的根 vnode 取自 union 的下层。

处理：把官方 `offline-owner-cache-carrier` 对 carrier 的放行逻辑，扩展到这条门禁
（同样用 `native_verified_block_root_authorized()` 证明根已认证）。

备选：移植参考仓库的 `0100-kebab-codesign`（`kebab_cs=8` 放行未封签根上的共享缓存）。

## A3. 若 `vm_shared_region_map_file` 失败

按 `kr` 处理：

| kr | 含义 | 处理 |
| --- | --- | --- |
| `KERN_INVALID_ARGUMENT` | 映射表/文件不符 | 校验 `sms_init_prot`、`sms_max_prot`、文件数与 UUID 绑定 |
| `KERN_NO_SPACE` | 共享区不足 | 检查 `sharedRegionStart/Size` 与 `0x180000000` 布局 |
| `KERN_PROTECTION_FAILURE` | 权限位 | 参考 `0103` 的 slide/`VM_PROT_SLIDE` 处理 |
| 其他 | — | 对照参考 `0142`（dynamic-config 页） |

## B. 映射成功之后（launchd 阶段）

1. `no-lc=1`（已加）→ 清 AMFI Launch Constraint；
2. launchd 报 `boot task is not a platform binary` → 参考 `0152`；
3. 报 `failed to get the code signing identity` / `namespace 7` → 参考 `0154`；
4. `/dev/console` 路由（`0145`/`0146`）让用户态日志可见；
5. launchd jobs（`0144`）。

## C. 屏幕（最后阶段，周级）

sheng 的显示参数可直接照抄（`refs/UniXNU-alioth-sheng-template/board.toml`）：

```
"offline-mobile-graphics-load=1" "offline-mobile-graphics-client=1"
"offline-legacy-register=1" "offline-legacy-gop=1" "offline-legacy-unit-scale=2"
"offline-legacy-pm=1" "offline-legacy-validate=1" "offline-legacy-transfer=1"
"gpu-power=9" "gpu-metal=1" "fb-gpu=1" "fb-bgra=1" "dpu-scanout=2"
"backlight=1" "vsync=120" "gpu-addon=1"
```

对应参考补丁：`0093`（IOSurface + IOMobileFramebuffer）、`0094`、`0095`；
GPU 侧需要 Adreno 650 驱动 + firmware + SMMU 域（我们的树里目前没有）。

## 执行顺序（明天）

1. A0 探针（1 次构建 + 刷机）→ 决定走 A1 / A2 / A3；
2. 按决定做对应最小改动，一次只改一处；
3. 每步都记录：镜像 sha、面板标记、判据；
4. 只有 A 明确走不通时，才回头做 B（内核建 shared region）。

## 2026-11-26 02:00：移植了 codesign/AMFI 层，定位到分类钩子

按“先翻 refs/ 再动手”的规则，从 `refs/UniXNU-kebab`（参考 kebab 移植）移植了两层：

| 文件 | 来源 | 作用 |
| --- | --- | --- |
| `bsd/kern/kebab_amfi.c` | `0088` + `0100` | 内置 AMFI 提供者（`amfi_interface_register`） |
| `bsd/kern/kebab_codesign.c/.h` | `0100` | `kebab_cs` 位策略：`0x2` 信任缓存分类、`0x8` 未封签根的共享缓存放行 |
| 钩子 | `0100` | `bsd_init` 调 `kebab_cs_trustcache_load()`；`ubc_subr` 调 `kebab_cs_classify_blob()`；`vm_unix` 调 `kebab_cs_shared_cache_allowed()` |

**中途的错误（已修正，记录以免重犯）**：曾把 `com.apple.driver.AppleMobileFileIntegrity`
从 `[kernel.prelink].roots` 和 `early_security_init` 移除，理由是“参考移植用内置提供者”。
实际参考移植是 **无 prelink 的独立内核**（`Route A`），与我们的 prelink 架构不同；移除后内核立刻
`amfi interface not available`。已恢复 prelink，Apple AMFI 继续存在。

**对照 sheng（`refs/UniXNU-sheng`，`c8f92b2`）**：sheng 的 `roots` **保留 AppleMobileFileIntegrity**，
它靠 `VERIFIED_ROOT_PROBE` + `ROOT_TRUST` + `no-lc=1`，不替换 AMFI。

**当前改动**：`kebab_cs_classify_blob()` 原来读根卷文件
`/System/Library/Caches/com.apple.kernelcaches/static.trustcache`（我们的根里没有），
现改为查询**内核已加载的静态信任缓存**（`query_trust_cache(kTCQueryTypeStatic, …)`），
镜像 `kebabwork/unixnu-kebab-cs-tckpi.img`（`f27012fc…`）待刷。

**已验证的事实**：主缓存的 CDHash `fcdb9268e40dc7599ae3f26c4116d5ab0329ead2`
（SHA-256 前 20 字节）**确实在合并信任缓存里**——之前用 64 字符比较得出“不在”是我算错了。

**下一步**：刷 `cs-tckpi` 镜像，看 `324` 的成功数是否从 0 变成 53（或至少主缓存成功），
再看 `320/322` 是否出现。
