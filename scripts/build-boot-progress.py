#!/usr/bin/env python3
"""Archive the board's boot progress hooks for the kernel link: kits/kebab/BootRamoops.c.

BootRamoops.c is UniXNU-pipa's zone writer (the kernel's log in the stock
ramoops console zone at 0xB0000000, which this board's live device tree also
describes) with pipa's restart/PMIC/cpufreq paths disabled: this board's PMIC
wiring is not confirmed yet. build-xnu-4k-baseline.sh runs this script before
the kernel build and force-loads build/boot-progress/libBootProgress.a. The
source declares the few kernel interfaces it calls, so it builds without the
kernel's headers.
"""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
source = ROOT / 'kits/kebab/BootRamoops.c'
out = ROOT / 'build/boot-progress'
out.mkdir(parents=True, exist_ok=True)
obj, archive = out / 'BootRamoops.o', out / 'libBootProgress.a'
subprocess.run(['xcrun', '--sdk', 'iphoneos', 'clang', '-arch', 'arm64', '-mkernel', '-miphoneos-version-min=17.0',
                '-mgeneral-regs-only', '-Os', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-Wno-unknown-warning-option',
                '-c', str(source), '-o', str(obj)], check=True)
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj)], check=True)
print(archive)
