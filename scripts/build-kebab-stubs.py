#!/usr/bin/env python3
"""Compile the kebab weak probe stubs into linkable archives."""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
src = ROOT / 'scripts' / 'kebab_stubs.c'
out = ROOT / 'build' / 'kebab-stubs'
out.mkdir(parents=True, exist_ok=True)
obj = out / 'kebab_stubs.o'
src_bind = ROOT / 'scripts' / 'kebab_bindings.c'
obj_bind = out / 'kebab_bindings.o'
lib = out / 'libKebabStubs.a'

subprocess.run(['clang', '-target', 'arm64-apple-ios17.0', '-mkernel', '-O2',
                '-fno-stack-protector', '-c', str(src), '-o', str(obj)], check=True)
subprocess.run(['clang', '-target', 'arm64-apple-ios17.0', '-mkernel', '-O2',
                '-fno-stack-protector', '-c', str(src_bind), '-o', str(obj_bind)], check=True)
if lib.exists():
    lib.unlink()
subprocess.run(['ar', 'rcs', str(lib), str(obj), str(obj_bind)], check=True)
print(f'built {lib}')
