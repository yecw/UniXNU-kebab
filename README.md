# UniXNU-kebab

UniXNU device repository for the **OnePlus 8T (kebab, Qualcomm SM8250)**.

## Status (2026-11-15)

Tested on a KB2000 (12+256, ColorOS 14, unlocked).

| Stage | State |
| --- | --- |
| Mu-Silicium UEFI, menu, payload volume, DarwinBoot | device-verified; the menu renders, the keys work, the payload TOML and both kernel files open, and the hand-off reports EL1, 4 KiB granule support and a successful `ExitBootServices` |
| Kernel entry | device-verified; the entry probe paints the green, blue and magenta bands (kernel entry, MMU enable) |
| Kernel after the MMU | stops inside `PE_create_console`, before `PE_init_printf` and `machine_startup` |
| PMGR bridge | linked (`CONFIG_XNU_SHENG_PMGR`); `package/ShengPlatformPower.cpp` and `scripts/build-platform-power.py` come from `UniXNU-pipa`, not from this repository |
| Panic log region | compiled in (`CONFIG_XNU_DT_PANIC_REGION`); `scripts/patch-xnu-entry-probe.py --panic-at` calls `panic()` at a chosen symbol and the panic screen draws from a retained boot video record (`patches/xnu/10063-kebab-panic-vinfo.patch`) |
| Console | not reached; `PE_create_console` stops even with the frame buffer marked physical, and with the function stubbed the kernel still stops before `machine_startup` |
| UFS root | not confirmed on hardware yet |

The 2026-11-15 work added the PMGR bridge, `XNU_DT_PANIC_REGION`, the
`armcpu-*`/`sheng-pmgr`/`debug=0x14e` kernel arguments, the panic probe and the
frame-buffer aperture path. Two facts came out of it:

- `PE_create_console()` runs **before** `machine_startup()` in `_arm_init`
  (`arm_vm_init`, `serial_init`, `PE_create_console`, `PE_init_printf`,
  `cpu_machine_idle_init`, the second `PE_init_platform`, `machine_startup`), so
  the console is created before `machine_conf` and a probe there can never fire.
- XNU's `initialize_screen()` takes a virtual frame buffer address unless bit 0
  marks it physical. DarwinBoot now sets that bit in `Args->Video[0]`, and the
  entry probe masks it before painting (an odd base faults with the MMU off).

The next diagnostic is the official boot progress hooks: `zzz-boot-progress.patch`
applies with fuzz except two owner-root hunks and one PMGR hunk, the build guard
requires `NATIVE_UFS` and `OWNER_ROOT_AUDIT`, and its ramoops zone `0xB0000000`
matches this board's live device tree.

The earlier 2026-11-01 result was: binary GIC handler installed, first PPI-27
IRQ fired, but the virtual timer did not repeat; the last visible line was
`Unable to find vm tag ?+z for btlog`. The source-GIC path replaces the binary
stub with GFander's `HAS_GIC_V3` / Group-1 `sleh_irq` design and is the active
path now.

## Layout

| Path | Contents |
| --- | --- |
| `board.toml` | board description, donor IPSW, kernel archives, payload and boot image parameters |
| `configs/kebab_defconfig` | minimal kernel configuration (DT platform, no device drivers yet) |
| `scripts/` | archive builders, the kebab ADT generator, the entry probe/panic probe and the PMGR bridge builder |
| `package/unixnu.toml` | UniXNU boot menu configuration for the device kit |
| `package/ShengPlatformPower.cpp` | original PMGR bridge linked by `CONFIG_XNU_SHENG_PMGR` |
| `docs/bring-up.md` | what was measured on hardware and what it took to boot |
| `drivers/KebabRamDisk` | submodule: UEFI driver that exposes the firmware-embedded payload as a RAM disk |
| `unixnu.lock` | pinned source commits written by `unixnu fetch` |

## Build (macOS or the GitHub macOS runner)

```sh
./unixnu defconfig kebab_defconfig -D ../UniXNU-kebab
./unixnu fetch
./unixnu ipsw fetch
./unixnu build kernel
```

Output: `out/kebab/kernel/kernel.macho`.

Without a Mac, fork `UniXNU/UniXNU`, enable Actions and run the **macOS build**
workflow with:

| Input | Value |
| --- | --- |
| `device_repository` | `<your-user>/UniXNU-kebab` |
| `defconfig` | `kebab_defconfig` |
| `steps` | `kernel` |
| `xcode` | default |

## Boot image recipe (what actually boots)

The firmware volume is built from Mu-Silicium and packed into an Android boot
image. Two details decide whether the phone shows anything:

1. **The DTB must be the phone's own live DTB.** `Mu-Silicium`'s
   `Resources/DTBs/kebab.dtb` has no `cont_splash_region@9c000000`, so ABL logs
   `ERROR: Could not get splash memory region node` and never hands the
   framebuffer to the firmware: the screen stays on the stock boot logo. Dump
   the live tree (`adb pull` of the running device tree) and pass that file as
   `--dtb`.
2. **Header v2 / 4096-byte pages**, kernel = BootShim + `SILICIUM_UEFI.fd`,
   `--kernel_offset 0x8000 --tags_offset 0x100 --dtb_offset 0x1f00000`.

`fastboot boot` is rejected by ABL (`Failed to load/authenticate boot image`);
flash to `boot_a` instead and reboot.

## Device values

| Item | Value | Source |
| --- | --- | --- |
| SoC | SM8250 (SD865) | live DTB |
| Debug UART | **`0xA90000`** (QUPv3 SE12, GPIO34/35, 115200 8N1) | live DTB `earlycon=msm_geni_serial,0xa90000` |
| Second console node | `0x00988000` is `status = "disabled"` | live DTB |
| Framebuffer / splash | `0x9C000000`, `0x2300000`, 1080 × 2400 | live DTB `cont_splash_region@9c000000` |
| Panel | `oplus20828samsung_amb655x_1080_2400_cmd` (Samsung AMB655X, command mode) | live DTB |
| Touch | Synaptics S3908, i2c `a94000` addr `0x4B`, IRQ GPIO39, reset GPIO38 | live DTB |
| Volume keys | PM8150 GPIO7 (down) / GPIO6 (up) | mainline kebab DTS |
| UEFI FD | base `0x9FC00000`, size `0x300000` | Mu-Silicium `kebab.toml` |
| `userdata` size | 233,492,557,824 bytes (57,005,019 × 4096) | `fastboot getvar partition-size:userdata` on the 12+256 unit |
| `userdata` first LBA | 4,748,320 (GPT of the KB2000 9008 package) | `gpt_main0.bin` |

## Remaining work

* **Timer / GIC**: the GIC handler and the first PPI-27 IRQ work, but the
  virtual timer does not repeat. `CNTV_TVAL_EL0`/`CNTV_CTL_EL0` re-arms after
  `rtclock_intr`, and a run that skips `rtclock_intr`, both remain static. Fix
  the periodic tick (or route a different timer source) before chasing UFS.
* **UFS**: `bsd/dev/kebab_ufs.c` is linked and exposes the BSD `ufsroot0` hook;
  the ADT has the root `/kebab-ufs` node and the boot args are documented in
  `board.toml`. Root discovery is not reached yet, so the driver has no hardware
  result.
* **Display / touch drivers** for the kernel, and the `userdata` image flash.

## License

BSD-2-Clause-Patent, matching UniXNU and Mu-Silicium.
## Porting record

[`porting.toml`](porting.toml) is the authoritative record: thirteen areas with
the measured facts, completion level and evidence for each. Read it before
changing the port, and update the affected area with every change:

```sh
python3 scripts/porting-record.py check --device ../UniXNU-kebab
```

[`AGENTS.md`](AGENTS.md) carries the build commands and the two firmware facts
that are expensive to rediscover. [`docs/bring-up.md`](docs/bring-up.md) keeps
the chronological measurements.
Inputs that are not in any repository are named in
[`proprietary-files.txt`](proprietary-files.txt); [`local.toml.example`](local.toml.example)
shows the local paths a workstation needs.
