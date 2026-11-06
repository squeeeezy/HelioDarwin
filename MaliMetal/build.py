#!/usr/bin/env python3
"""Build the experimental iOS arm64 bundle from source with Zig's Clang.

Usage: python MaliMetal/build.py --sysroot DIRECTORY [--wsl] [--out DIRECTORY]
Requires ziglang, LLVM ld64.lld and matching iOS system/framework libraries.
No existing bundle is used as input. C symbols link strictly against those
libraries; Objective-C selector compatibility still requires runtime validation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import os
import shutil
import struct
import subprocess
import sys
import ziglang
from Shader.build_air import PIN as AIR_COMPILER_REVISION
from Vulkan.prepare_mesa import MESA_COMMIT, PANVK_COMMIT

HERE = Path(__file__).resolve().parent
SOURCES = (
    'MaliMetalDevice.m',
    'MaliMetalResources.m',
    'MaliMetalGraphics.m',
    'MaliMetalRender.m',
    'MaliMetalMPSBridge.mm',
    'MaliMetalAPIExtensions.m',
    'MaliMetalMSL.m',
    'MaliMetalCommandQueue.m',
    'MaliMetalSynchronization.m',
    'MaliMetalPresentation.m',
    'Vulkan/MaliVulkanSynchronization.c',
    'MaliMetalVulkan.c',
    'Shader/MaliMetalShader.c',
    'Shader/MaliMetalShaderVulkan.c',
    'Shader/MaliMetalRenderVulkan.c',
    'Shader/MaliMetalCompute.c',
    'Shader/MaliMetalComputeVulkan.c',
    'Vulkan/MaliVulkanBackend.c',
    'Vulkan/MaliVulkanResources.c',
    'Vulkan/MaliVulkanGraphics.c',
    'Vulkan/MaliKmodDarwin.c',
    'Vulkan/MaliVulkanProfile.c',
)
SYSTEM_LIBRARIES = (
    'usr/lib/libSystem.dylib',
    'usr/lib/libobjc.A.dylib',
    'System/Library/Frameworks/Foundation.framework/Foundation',
    'System/Library/Frameworks/CoreFoundation.framework/CoreFoundation',
    'System/Library/Frameworks/Metal.framework/Metal',
    'System/Library/Frameworks/IOKit.framework/IOKit',
    'System/Library/Frameworks/IOSurface.framework/IOSurface',
)
MPS_COMMON_LIBRARIES = (
    'usr/lib/libc++.1.dylib',
    'System/Library/Frameworks/MetalPerformanceShaders.framework/Frameworks/MPSCore.framework/MPSCore',
)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(arguments):
    subprocess.run([sys.executable, '-m', 'ziglang', 'cc', *arguments], check=True)


def verify_platform(binary):
    data = binary.read_bytes()
    magic, cpu, _, kind, commands = struct.unpack_from('<IIIII', data)
    if (magic, cpu, kind) != (0xfeedfacf, 0x100000c, 6):
        raise ValueError('Expected an arm64 Mach-O dynamic library')
    offset = 32
    platform = None
    for _ in range(commands):
        command, size = struct.unpack_from('<II', data, offset)
        if size < 8 or offset + size > len(data):
            raise ValueError('Invalid Mach-O load command')
        if command == 0x32:
            platform = struct.unpack_from('<I', data, offset + 8)[0]
        offset += size
    if platform != 2:
        raise ValueError(f'Expected iOS platform 2, got {platform}')


def verify_internal_symbols(binary):
    """Reject unresolved driver symbols in addition to strict system linking."""
    data = binary.read_bytes()
    count = struct.unpack_from('<I', data, 16)[0]
    offset = 32
    for _ in range(count):
        command, size = struct.unpack_from('<II', data, offset)
        if command == 2:  # LC_SYMTAB
            symoff, nsyms, stroff, strsize = struct.unpack_from('<IIII', data, offset + 8)
            if symoff + nsyms * 16 > len(data) or stroff + strsize > len(data):
                raise ValueError('Invalid Mach-O symbol table')
            strings = data[stroff:stroff + strsize]
            missing = []
            prefixes = (b'_mmvk_', b'_mmk_', b'_MMShader', b'_MMCompute', b'_mm_air_',
                        b'_MaliMetal', b'_OBJC_CLASS_$_Mali', b'_OBJC_METACLASS_$_Mali')
            for index in range(nsyms):
                name_index, kind, _, _, value = struct.unpack_from('<IBBHQ', data, symoff + index * 16)
                if kind & 0xe0 or kind & 0x0e or value:
                    continue
                if name_index >= len(strings):
                    raise ValueError('Invalid Mach-O symbol name')
                end = strings.find(b'\0', name_index)
                if end < 0:
                    raise ValueError('Unterminated Mach-O symbol name')
                name = strings[name_index:end]
                if name.startswith(prefixes):
                    missing.append(name.decode('ascii'))
            if missing:
                raise ValueError('Driver implementations missing at link: ' + ', '.join(missing))
        offset += size


def source_hashes():
    inputs = set(HERE.glob('*.h')) | {HERE / name for name in SOURCES}
    inputs.update((HERE / 'Info.plist', Path(__file__).resolve()))
    for directory in ('Shader', 'Vulkan'):
        inputs.update(path for path in (HERE / directory).rglob('*')
                      if path.is_file() and path.suffix in
                      ('.h', '.c', '.rs', '.py', '.in', '.lock', '.toml', '.patch', '.json'))
    return {path.relative_to(HERE).as_posix(): sha256(path) for path in sorted(inputs)}


def air_provider(directory):
    manifest = json.loads((directory / 'build.json').read_text(encoding='utf-8'))
    if manifest['target'] != 'aarch64-apple-ios':
        raise ValueError('AIR provider was not built for arm64 iOS')
    if manifest.get('compiler_abi') != 4 or manifest.get('compute_abi') != 2:
        raise ValueError('AIR provider must implement render compiler ABI 4 and compute ABI 2')
    if manifest['compiler_revision'] != AIR_COMPILER_REVISION:
        raise ValueError('AIR provider compiler revision differs from the pinned source')
    patches = manifest.get('compiler_patches', [])
    if not manifest.get('compiler_effective_tree') or (
            not patches and manifest['compiler_effective_tree'] != manifest['compiler_tree']):
        raise ValueError('AIR provider effective compiler tree is missing or undeclared')
    for patch in patches:
        if sha256(Path(patch['path']).resolve(strict=True)) != patch['sha256']:
            raise ValueError('AIR compiler patch changed since the provider build')
    shader = HERE / 'Shader'
    sources = {path.relative_to(shader).as_posix(): sha256(path)
               for path in sorted((shader / 'air-compiler/src').rglob('*.rs'))}
    if sources != manifest.get('adapter_sources'):
        raise ValueError('AIR provider adapter sources changed since build')
    recorded = {
        'cargo_lock_sha256': shader / 'air-compiler/Cargo.lock',
        'adapter_manifest_sha256': shader / 'air-compiler/Cargo.toml.in',
        'build_helper_sha256': shader / 'build_air.py',
    }
    for field, path in recorded.items():
        if sha256(path) != manifest.get(field):
            raise ValueError(f'AIR provider build input changed: {path.name}')
    archive = directory / 'adapter/target/aarch64-apple-ios/release/libMaliMetalAIR.a'
    digest = sha256(archive)
    if manifest['artifacts'].get(str(archive)) != digest:
        raise ValueError('AIR provider archive differs from build manifest')
    return archive, manifest


def vulkan_provider(binary):
    metadata = binary.with_name('panvk-sweetmali.json')
    manifest = json.loads(metadata.read_text(encoding='utf-8'))
    if manifest['binary'] != 'libpanvk_sweetmali.dylib' or sha256(binary) != manifest['binary_sha256']:
        raise ValueError('Vulkan ICD differs from its provenance manifest')
    if manifest['mesa']['commit'] != MESA_COMMIT or manifest['panvk_rc3']['commit'] != PANVK_COMMIT:
        raise ValueError('Vulkan ICD revisions differ from the pinned dependencies')
    if manifest['kernel_abi_sha256'] != sha256(HERE / 'MaliMetalKernelABI.h'):
        raise ValueError('Vulkan ICD was built against a different kernel ABI')
    for name in ('MaliKmodDarwin.c', 'MaliKmodDarwin.h', 'mesa/pan_kmod_xnu.c'):
        if manifest['xnu_port_inputs'].get(name) != sha256(HERE / 'Vulkan' / name):
            raise ValueError(f'Vulkan ICD transport source changed: {name}')
    verify_platform(binary)
    return metadata, manifest


def copy_component(source, bundle, relative, components):
    destination = bundle / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    components[relative] = sha256(destination)


def copy_notices(source, bundle, prefix, components):
    source = source.resolve(strict=True)
    for path in sorted(source.rglob('*')):
        if path.is_symlink():
            raise ValueError(f'License notice must be a regular file: {path}')
        if path.is_file():
            relative = prefix + '/' + path.relative_to(source).as_posix()
            copy_component(path, bundle, relative, components)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sysroot', type=Path, required=True)
    parser.add_argument('--linker', default='ld64.lld')
    parser.add_argument('--wsl', action='store_true', help='run the linker in WSL Ubuntu')
    parser.add_argument('--out', type=Path, default=HERE / 'out')
    parser.add_argument('--vulkan-headers', type=Path, required=True,
                        help='include directory containing vulkan/vulkan.h from pinned Mesa')
    parser.add_argument('--air-provider', type=Path,
                        help='explicit output directory from Shader/build_air.py')
    parser.add_argument('--vulkan-icd', type=Path,
                        help='explicit arm64 iOS PanVK library to include in the bundle')
    parser.add_argument('--mps-common', action='store_true',
                        help='enable experimental target-pinned native MPS Common qualification')
    parser.add_argument('--gpu-timing', action='store_true',
                        help='enable experimental measured GPU timing; requires admitted native kernel clock and timestamp queries')
    args = parser.parse_args()
    if args.gpu_timing and not args.vulkan_icd:
        parser.error('--gpu-timing requires an explicit matching --vulkan-icd artifact; native clock/query support is checked at runtime')
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=True)
    sysroot = args.sysroot.resolve(strict=True)
    system_library = sysroot / 'usr/lib/libSystem.dylib'
    if not system_library.is_file():
        parser.error('sysroot must contain usr/lib/libSystem.dylib and its reexports')
    system_libraries = SYSTEM_LIBRARIES + (MPS_COMMON_LIBRARIES if args.mps_common else ())
    for name in system_libraries:
        if not (sysroot / name).is_file():
            parser.error(f'sysroot is missing the matching iOS library: {name}')
    headers = args.vulkan_headers.resolve(strict=True)
    if not (headers / 'vulkan/vulkan.h').is_file():
        parser.error('--vulkan-headers must contain vulkan/vulkan.h')
    hashes = source_hashes()
    digest = hashlib.sha256(json.dumps(hashes, sort_keys=True).encode()).hexdigest()
    common = ['-target', 'aarch64-ios.16.7', '-mcpu=cortex_a55', '-O2',
              '-fno-sanitize=all', '-Wall', '-Wextra', '-Werror', '-I', str(HERE),
              '-I', str(headers),
              '-isystem', str(Path(ziglang.__file__).parent / 'lib/libc/include/any-darwin-any')]
    if args.mps_common:
        common.append('-DMM_ENABLE_MPS_COMMON_BACKEND=1')
    if args.gpu_timing:
        common.append('-DMM_ENABLE_REAL_GPU_TIMING=1')
    provider_manifest = None
    provider_archive = None
    if args.air_provider:
        provider_archive, provider_manifest = air_provider(args.air_provider.resolve(strict=True))
        common.append('-DMM_SHADER_ENABLE_AIR_COMPILER')
    objects = []
    for source in SOURCES:
        obj = output / Path(source).with_suffix('.o')
        obj.parent.mkdir(parents=True, exist_ok=True)
        flags = ['-fno-objc-arc', '-fblocks'] if source.endswith(('.m', '.mm')) else []
        if source.endswith('.mm'):
            flags += ['-std=c++17', '-fexceptions', '-fcxx-exceptions']
        run([*common, *flags, '-c', str(HERE / source), '-o', str(obj)])
        objects.append(str(obj))
    bundle = output / 'MaliMetal.bundle'
    bundle.mkdir(exist_ok=True)
    binary = bundle / 'MaliMetal'
    def linker_path(path):
        if args.wsl:
            value = str(Path(path).resolve())
            drive, tail = os.path.splitdrive(value)
            if len(drive) != 2 or drive[1] != ':':
                raise ValueError('WSL linking requires paths on a Windows drive')
            return '/mnt/' + drive[0].lower() + tail.replace('\\', '/')
        return str(path)
    launcher = ['wsl.exe', '-d', 'Ubuntu', '--exec'] if args.wsl else []
    link = [*launcher, args.linker, '-dylib', '-arch', 'arm64',
            '-platform_version', 'ios', '16.7', '16.7', '-undefined', 'error',
            '-syslibroot', linker_path(sysroot), '-install_name',
            '/System/Library/Extensions/MaliMetal.bundle/MaliMetal',
            '-adhoc_codesign', '-o', linker_path(binary),
            *(linker_path(obj) for obj in objects),
            *(linker_path(sysroot / name) for name in system_libraries)]
    if provider_archive:
        link.append(linker_path(provider_archive))
    subprocess.run(link, check=True)
    verify_platform(binary)
    verify_internal_symbols(binary)
    if source_hashes() != hashes:
        raise ValueError('Driver source changed while the build was running; rebuild')
    info = plistlib.loads((HERE / 'Info.plist').read_bytes())
    info['MaliMetalSourceDigest'] = digest
    (bundle / 'Info.plist').write_bytes(plistlib.dumps(info))
    version = subprocess.run([sys.executable, '-m', 'ziglang', 'version'],
                             check=True, capture_output=True, text=True).stdout.strip()
    manifest = {'source_digest': digest, 'inputs': hashes, 'zig_version': version,
                'libsystem_sha256': sha256(system_library),
                'target': 'aarch64-ios.16.7', 'binary_sha256': sha256(binary),
                'binary_size': binary.stat().st_size}
    manifest['vulkan_header_sha256'] = sha256(headers / 'vulkan/vulkan_core.h')
    manifest['system_libraries'] = {name: sha256(sysroot / name) for name in system_libraries}
    manifest['mps_common_backend'] = {
        'enabled': args.mps_common,
        'scope': 'experimental native Common qualification' if args.mps_common else 'disabled',
        'target_build': '20H392',
        'MPSCore_UUID': '078bcbde-b90f-39b5-aec9-f127ec42577d',
        'native_object_bytes': 0x9b8,
        'publication': 'guarded native RW acquire/release registry' if args.mps_common else None,
        'hardware_qualified': False,
    }
    manifest['gpu_timing'] = {'enabled': args.gpu_timing, 'experimental': True,
        'runtime_required': 'validated native MMK clock and matching 64-bit GPU timestamp queries',
        'kernel_scheduler_timing': False, 'hardware_qualified': False}
    manifest['air_provider'] = provider_manifest
    manifest['components'] = {}
    if args.air_provider:
        copy_notices(args.air_provider.resolve() / 'licenses', bundle,
                     'licenses/air', manifest['components'])
    if args.vulkan_icd:
        icd = args.vulkan_icd.resolve(strict=True)
        metadata, provenance = vulkan_provider(icd)
        copy_component(icd, bundle, 'libpanvk_sweetmali.dylib', manifest['components'])
        copy_component(metadata, bundle, 'panvk-sweetmali.json', manifest['components'])
        copy_notices(icd.parent / 'LICENSES', bundle, 'licenses/panvk', manifest['components'])
        manifest['vulkan_provider'] = provenance
    else:
        # An earlier build must not silently supply an unrelated GPU backend.
        (bundle / 'libpanvk_sweetmali.dylib').unlink(missing_ok=True)
    (output / 'build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
