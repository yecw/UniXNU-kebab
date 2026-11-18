# Firmware inputs for the OnePlus 8T (kebab)

These are the pieces of the Mu-Silicium tree that the working firmware needs and
that a plain clone does not carry.

## `KebabRamDiskPrebuilt/`

`KebabRamDisk.efi` is the driver exactly as the 2026-11-15 firmware shipped it:
it publishes the payload region of the FD as a `EFI_BLOCK_IO_PROTOCOL` device
itself, then FatPkg mounts it and UniXNU finds `\EFI\UniXNU\unixnu.toml`.

The source tree's driver (`Platforms/OnePlus/kebabPkg/Driver/KebabRamDisk`) was
later rewritten to publish the payload through the `RamDiskDxe` protocol and to
cap the payload at 0x4000000. Every image rebuilt from that rewrite stops at BDS
with "Failed to Find an OS!", with or without `RamDiskDxe` in the FV and with the
cap raised. The binary here is the known-good one; keep using it until the
sources are reconciled.

`KebabRamDisk.inf` wraps that PE as a prebuilt module with the same FILE_GUID
(3C4D5E6F-7A8B-49C1-9D2E-0F1A2B3C4D5E), so the DSC/FDF entry only changes its
path:

    kebabPkg/Driver/KebabRamDiskPrebuilt/KebabRamDisk.inf

## `docs/patches/mu-silicium-kebab-firmware-fixes.patch`

Applies on top of the Mu-Silicium checkout used for this board and carries the
two fixes that make a rebuilt firmware reach the menu:

* `Silicon/Silicium/SiliciumPkg/Sec/Sec.c`: drop the SEC-stage write to
  `0x17C10008`. It runs before the MMU and exception vectors exist, so the write
  faults into an unmapped address and the firmware dies before printing anything
  (a black screen with no log at all).
* `Silicon/Silicium/SiliciumPkg/SiliciumPkg.dsc.inc`: keep
  `PcdDebugPropertyMask` at 0x2F and `PcdDebugPrintErrorLevel` at 0x8007EE4F.
  With the mask at 0x00 the DEBUG macro compiles away, every module loses its
  message strings and the framebuffer log stays empty.

Apply and build:

```sh
git -C ../Mu-Silicium apply ../UniXNU-kebab/docs/patches/mu-silicium-kebab-firmware-fixes.patch
```

The rest of the patch is the board's previous firmware work (DXE heap, payload
cap, boot manager markers, fast framebuffer log); it is kept in one place so a
rebuild from a clean Mu-Silicium checkout reproduces the image that boots.
