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
import subprocess
import sys

TREE = Path.cwd()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compile_command(record, source, output, includes, extra):
    arguments = record['arguments']
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
    includes = ['-I', str(headers), '-I', str(TREE / '.cache/xnu-work/osfmk'),
                '-I', str(TREE / '.cache/xnu-work/iokit')]
    device = Path(os.environ['UNIXNU_DEVICE'])
    sources = [base,
               device / 'drivers/KebabBlockStorage/KebabSourceStorageParent.cpp',
               device / 'drivers/KebabBlockStorage/KebabBlockStorageDevice.cpp']
    objects = []
    for source in sources:
        obj = out / (source.stem + '.o')
        command = compile_command(record, source, obj, includes, [])
        result = subprocess.run(command, capture_output=True, text=True, cwd=record['directory'])
        (out / (source.stem + '.log')).write_text(result.stderr)
        if result.returncode != 0:
            sys.stderr.write(result.stderr[-4000:])
            raise SystemExit(f'{source}: compile failed')
        objects.append(obj)
    archive = out / 'libKebabStorage.a'
    for obj in objects:
        subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj)], check=True)
    report = dict(archive=str(archive), archive_sha256=sha(archive),
                  objects=[dict(path=str(o), sha256=sha(o)) for o in objects],
                  base_sha256=sha(base))
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'KEBAB STORAGE: {archive}')


if __name__ == '__main__':
    main()
