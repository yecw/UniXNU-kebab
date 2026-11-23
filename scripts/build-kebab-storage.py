#!/usr/bin/env python3
"""Compile the kebab IOKit block-storage driver against IOStorageFamily 337.100.1.

The storage family itself stays in the prelinked collection: only
IOBlockStorageDevice is compiled from Apple's public source, with its metaclass
taking the runtime-resolved superclass (the class the prelinked
IOBlockStorageDriver already knows). KebabBlockStorageDevice and the lookup land
in libKebabStorage.a, which the kernel link includes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

TREE = Path.cwd()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compile_command(record, source, output, includes, extra):
    arguments = list(record['arguments'])
    if str(source).endswith('.S'):
        arguments = ['-xassembler-with-cpp' if a == '-xc++' else a
                     for a in arguments if a != '--driver-mode=g++' and not a.startswith('-std=gnu++')]
    elif str(source).endswith('.c'):
        arguments = ['-xc' if a == '-xc++' else
                     '--driver-mode=gcc' if a == '--driver-mode=g++' else
                     '-std=gnu17' if a.startswith('-std=gnu++') else a
                     for a in arguments]
    command = [arguments[0], *includes]
    index = 1
    replaced = False
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
            replaced = True
        elif value == '-o':
            command.extend(['-o', str(output)])
            index += 1
        elif value in ('-MF', '-MT', '-MQ', '-MJ'):
            index += 1
        elif value in ('-MD', '-MMD', '-MP'):
            pass
        elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    if not replaced:
        raise SystemExit(f'{source}: the kernel recipe has no source argument')
    command += ['-Wall', '-Wextra',
                '-Wno-unused-parameter', '-Wno-c99-designator', '-Wno-reorder-init-list',
                '-Wno-deprecated-enum-enum-conversion', '-Wno-unknown-warning-option',
                '-Wno-unnecessary-virtual-specifier', '-Wno-zero-as-null-pointer-constant',
                '-Wno-suggest-override', '-Wno-suggest-destructor-override',
                '-Wno-shadow-field', '-Wno-unused-private-field',
                '-mgeneral-regs-only', *extra]
    return command


def main():
    out = TREE / 'build/kebab-storage'
    out.mkdir(parents=True, exist_ok=True)
    recipe = TREE / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    record = json.loads(recipe.read_text().strip().rstrip(','))
    headers = TREE / 'build/storage-abi/include'
    if not (headers / 'IOKit/storage/IOBlockStorageDevice.h').is_file():
        raise SystemExit(f'{headers}: run prepare-storage-headers.py first')
    storage = TREE / 'refer/IOStorageFamily'
    base = out / 'IOBlockStorageDevice.cpp'
    original = (storage / 'IOBlockStorageDevice.cpp').read_text()
    macro = 'OSDefineMetaClassAndAbstractStructors(IOBlockStorageDevice,IOService)'
    if original.count(macro) != 1:
        raise SystemExit('IOStorageFamily registration macro changed')
    base.write_text(original.replace(macro, '''
extern "C" const OSMetaClass *native_source_storage_parent(void);
OSDefineMetaClassWithInit(IOBlockStorageDevice, IOService, )
IOBlockStorageDevice::MetaClass::MetaClass()
    : OSMetaClass("KEBABBlockStorageDevice", native_source_storage_parent(), sizeof(IOBlockStorageDevice)) {}
OSDefineAbstractStructors(IOBlockStorageDevice, IOService)
OSDefineOperatorMethods(IOBlockStorageDevice)
'''))
    device = Path(os.environ['UNIXNU_DEVICE'])
    kit = TREE / 'kits/VerifiedRoot'
    verified_include = kit / 'include'
    includes = ['-I', str(headers), '-I', str(TREE / '.cache/xnu-work/osfmk'),
                '-I', str(TREE / '.cache/xnu-work/iokit'),
                '-I', str(verified_include),
                '-I', str(device / 'drivers/VirtioMmioBlock')]
    sources = [base,
               device / 'drivers/KebabBlockStorage/KebabSourceStorageParent.cpp',
               device / 'drivers/KebabBlockStorage/KebabBlockStorageDevice.cpp',
               kit / 'src/verified_root.c',
               kit / 'adapters/xnu_crypto.c',
               device / 'drivers/KebabBlockStorage/KebabVerifiedRootData.c']
    material = TREE / 'build/verified-root-OS'
    material_asm = out / 'KebabRootMaterial.S'
    blob = material / 'manifest.bin'
    if not blob.is_file():
        blob = material / 'manifest.header.bin'
    blob_systemos = TREE / 'build/verified-root-SystemOS/manifest.header.bin'
    if not blob_systemos.is_file():
        blob_systemos = TREE / 'build/verified-root-SystemOS/manifest.bin'
    if (blob.is_file() or blob_systemos.is_file()) and (material / 'trusted-key.bin').is_file():
        size = blob.stat().st_size
        material_asm.write_text(
            '.section __DATA_CONST,__const\n'
            '.p2align 12\n'
            '.globl _verified_root_manifest\n'
            '_verified_root_manifest:\n'
            f'.incbin "{blob}"\n'
            '.p2align 4\n'
            '.globl _verified_root_key\n'
            '_verified_root_key:\n'
            f'.incbin "{material / "trusted-key.bin"}"\n'
            '.section __DATA,__data\n'
            '.globl _verified_root_manifest_bytes\n'
            '_verified_root_manifest_bytes:\n'
            f'.quad {size}\n')
        if blob_systemos.is_file():
            material_asm.write_text(material_asm.read_text() +
                '.section __DATA_CONST,__const\n'
                '.p2align 12\n'
                '.globl _verified_root_manifest_systemos\n'
                '_verified_root_manifest_systemos:\n'
                f'.incbin "{blob_systemos}"\n'
                '.section __DATA,__data\n'
                '.globl _verified_root_manifest_systemos_bytes\n'
                '_verified_root_manifest_systemos_bytes:\n'
                f'.quad {blob_systemos.stat().st_size}\n')
        sources.append(material_asm)
    objects = []
    objcopy = shutil.which('llvm-objcopy') or '/opt/homebrew/opt/llvm/bin/llvm-objcopy'
    for source in sources:
        obj = out / (source.stem + '.o')
        command = compile_command(record, source, obj, includes,
                                  ['-DNATIVE_VERIFIED_BLOCK=1'])
        result = subprocess.run(command, capture_output=True, text=True, cwd=record['directory'])
        (out / (source.stem + '.log')).write_text(result.stderr)
        if result.returncode != 0:
            sys.stderr.write(result.stderr[-4000:])
            raise SystemExit(f'{source}: compile failed')
        if source.name == 'KebabRootMaterial.S':
            objects.append(obj)
            continue
        if source.name in ('IOBlockStorageDevice.cpp', 'KebabBlockStorageDevice.cpp'):
            # The kernel would run these constructors from __mod_init_func during
            # OSRuntimeInitializeCPP, before any prelinked kext class exists; the
            # base class would then be an orphan root. Strip them here and let
            # kebab_storage_register_classes() run them after the family loads.
            late = out / (source.stem + '.late.o')
            subprocess.run([objcopy, '--remove-section=__DATA,__mod_init_func',
                            f'--globalize-symbol=__GLOBAL__sub_I_{source.name}', str(obj), str(late)], check=True)
            obj = late
        objects.append(obj)
    archive = out / 'libKebabStorage.a'
    archive.unlink(missing_ok=True)
    for obj in objects:
        subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj)], check=True)
    report = dict(archive=str(archive), archive_sha256=sha(archive),
                  objects=[dict(path=str(o), sha256=sha(o)) for o in objects],
                  base_sha256=sha(base))
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'KEBAB STORAGE: {archive}')


if __name__ == '__main__':
    main()
