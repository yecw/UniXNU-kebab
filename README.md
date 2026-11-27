# UniXNU-kebab

一加 8T（kebab，KB2000 / 高通 SM8250）的 UniXNU 设备仓库：把 iPadOS 17.7.11（21H461）
的 4 KiB 页内核跑到这台手机上。

## 当前进度（2026-11-28，真机）

标记编号来自面板控制台，括号里是实测看到的证据。

### 已打通

| 阶段 | 状态 |
| --- | --- |
| Mu-Silicium UEFI、菜单、payload 卷、DarwinBoot | 通过 |
| 内核早期 → `bsd_init` → `vfs_mountroot` | 通过（UFS 驱动 + SHNGROOT 窗口） |
| OS / SystemOS 挂载（union）与未封签根 | 通过（`297` vroot 窗口、`299` root verified） |
| 根卷 owner-verified 窗口校验 | 通过（`kebab_verify_root=1`，`kits/VerifiedRoot`） |
| dyld 共享缓存解析 | 通过（`309` plan，61 映射、3.3 GB） |
| 缓存签名注册 | **53/53**（`324 = 53 0`），其后 dyld 映射缓存（`322`） |
| Data 卷（`/private/var`） | 通过：packed seed（32 MiB → ~1 MiB）+ `seed-data-skip-setup` |
| launchd | 起来并拉起守护进程（`watchdogd[98]`） |
| watchdogd | 由内核写 launchd `disabled.plist` 禁用（`kebab-disable-watchdogd=1`） |

### 还差什么

| 项 | 说明 |
| --- | --- |
| 控制台越界崩溃 | 深启动后 `kebab_early_putc` 会写到别名 + 24 MiB 之外（`pc 0xfffffff0077e5c70`）。修法：给帧缓冲写加边界检查，或在缓存注册后 `kebab_early_disable()` |
| 用户态第一个硬依赖 | watchdogd 之后下一个卡住的服务（大概率 `backboardd`，需要显示服务） |
| 显示 / GPU | `IOMobileFramebuffer`/DPU/Adreno650 驱动，参考 `refs/alioth` 与 sheng 的 graphics 系列 |
| SpringBoard | 依赖上一条；也是“能开机”的终点 |
| 可选 | 硬件看门狗、USB CDC 控制台、PMGR 桥 |

## 构建

```bash
cd ../UniXNU
export PATH=/opt/homebrew/bin:/opt/homebrew/opt/llvm/bin:$PATH
export UNIXNU_UNIQUE_CHIP_ID="$(cat ../secrets/UNIXNU_UNIQUE_CHIP_ID)"
export UNIXNU_NO_PRAM=1 UNIXNU_GITHUB_PROXY=https://gh-proxy.com/

./unixnu kernel  -D ../UniXNU-kebab-stevedang --uefi ../Mu-Silicium \
                 --edk2 ../Mu-Silicium/Mu_Basecore --os ipados-17.7.11-21H461 \
                 --ipsw ../ipsw/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw \
                 --target DEBUG -j 10

./unixnu payload -D ../UniXNU-kebab-stevedang --os ipados-17.7.11-21H461 \
                 --ipsw ../ipsw/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw \
                 --target DEBUG -j 10 \
                 --owner-trust-cache ../kebabwork/trustcache/kebab-merged-static.trustcache \
                 --data-ramdisk ../UniXNU/build/owner-data-fixture/owner-data-skip.dbr
```

## 刷机

boot 分区只有 **96 MiB**，镜像必须 ≤ 96 MiB。

1. 把 payload 注入 `SILICIUM_UEFI-good.fd`（`0x300000`）后，用 `scripts/pack-bootimg.sh` 打包；
2. **必须同时刷 vbmeta**：`kebabwork/vbmeta-disabled.img`
   （大端、`flags=0x3`；AVB 头是大端，写错端序会得到“系统文件被破坏”）；
3. `fastboot flash boot_b <img> && fastboot flash vbmeta_a/b <vbmeta> && fastboot --set-active=b`。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `board.toml` | 板级描述、donor IPSW、prelink roots、payload、boot image 参数 |
| `configs/kebab_defconfig` | 内核配置（含 `XNU_NATIVE_CS_HASH_CACHE`、`DATA_MOUNT`、`RAM_BLOCK`） |
| `dts/kebab.dts` | 硬件设备树（ADT 输入） |
| `patches/xnu/` | 10063 内核补丁系列（`series` 为顺序表） |
| `scripts/` | archive builder、ADT 生成器、控制台/探针、pack 脚本 |
| `docs/` | `patch-plan.md`（补丁分类）、`plan-dyld-cache.md`（缓存方案） |
