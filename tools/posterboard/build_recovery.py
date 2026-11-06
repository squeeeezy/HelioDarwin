#!/usr/bin/env python3
"""Strictly build the opt-in 20H392 mobile-lsd metadata recovery dependency."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import ziglang

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import sweet_macho_dependency


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sysroot', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--wsl', action='store_true')
    parser.add_argument('--linker', default='ld64.lld')
    args = parser.parse_args()
    source = Path(__file__).with_name('SweetRepairPosterMetadata.c')
    source_digest = hashlib.sha256(source.read_bytes()).hexdigest()
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=True)
    obj = output / 'SweetRepairPosterMetadata.o'
    binary = output / 'SweetRepairPosterMetadata.dylib'
    sysroot = args.sysroot.resolve(strict=True)
    headers = Path(ziglang.__file__).parent / 'lib/libc/include/any-darwin-any'
    compile_command = [sys.executable, '-m', 'ziglang', 'cc', '-target', 'aarch64-ios.16.7',
                       '-mcpu=cortex_a55', '-O2', '-fno-sanitize=all', '-Wall', '-Wextra',
                       '-Werror', '-fblocks', '-isystem', str(headers), '-c', str(source), '-o', str(obj)]
    subprocess.run(compile_command, check=True)

    def link_path(path):
        value = str(path)
        if not args.wsl:
            return value
        drive, tail = os.path.splitdrive(value)
        if len(drive) != 2 or drive[1] != ':':
            raise ValueError('WSL linker paths must be on a Windows drive')
        return '/mnt/' + drive[0].lower() + tail.replace('\\', '/')

    launcher = ['wsl.exe', '-d', 'Ubuntu', '--exec'] if args.wsl else []
    link_command = [*launcher, args.linker, '-dylib', '-install_name',
                    '/usr/local/lib/sweet/SweetRepairPosterMetadata.dylib', '-arch', 'arm64',
                    '-platform_version', 'ios', '16.7', '16.7', '-syslibroot', link_path(sysroot),
                    '-adhoc_codesign', '-o', link_path(binary), link_path(obj), '-lSystem']
    subprocess.run(link_command, check=True)
    data = binary.read_bytes()
    info = sweet_macho_dependency.image_info(data, filetype=6)
    if info.get('install_name') != '/usr/local/lib/sweet/SweetRepairPosterMetadata.dylib':
        raise ValueError('Unexpected recovery install name')
    if hashlib.sha256(source.read_bytes()).hexdigest() != source_digest:
        raise ValueError('Recovery source changed during build')
    (output / 'SweetRepairPosterMetadata.build.json').write_text(json.dumps({
        'os_build': '20H392', 'source_sha256': source_digest,
        'binary_sha256': hashlib.sha256(data).hexdigest(),
        'kind': 'dylib', 'host': '/usr/libexec/lsd', 'host_uid': 501,
        'host_uuid': 'cfa9bb6b-7d4b-3397-94f6-595d6f1eddaa',
        'host_preimage_sha256': 'c46e8e7330ab1c008aa50873d230a44f68eb57d675e3fe0b01e0917f9596b2e1',
        'compile_command': compile_command, 'link_command': link_command,
        'runtime_validation': 'pending',
    }, indent=2) + '\n', encoding='utf-8')
    print(binary)


if __name__ == '__main__':
    main()
