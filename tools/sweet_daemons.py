#!/usr/bin/env python3
"""Inspect and restore launchd jobs in an offline SWEET iOS root.

Restoring a job only restores its launchd definition. Its hardware and kernel
dependencies must be implemented before the corresponding service can work.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import plistlib
import struct
import tempfile
import sweet_macho_dependency


CACHE = 'System/Library/xpc/launchd.plist'
ARCHIVE = 'private/var/db/sweet-root-tweaks/launchd-original-jobs.plist'
POSTER_LIBRARY = '/usr/local/lib/sweet/SweetPosterBoard.dylib'
POSTER_DEPENDENCY_STATE = 'private/var/db/sweet-root-tweaks/poster-dependency.plist'
POSTER_LOG = '/private/var/mobile/Library/Logs/SweetPosterBoard.log'
POSTER_REPAIR_LIBRARY = '/usr/local/lib/sweet/SweetRepairPosterMetadata.dylib'
POSTER_REPAIR_STATE = 'private/var/db/sweet-root-tweaks/poster-metadata-dependency.plist'
DEPENDENCIES = {
    'com.apple.CommCenter': 'baseband transport and CoreTelephony modem interface',
    'com.apple.locationd': 'location providers, authorization database and sensor interfaces',
    'com.apple.wifid': 'wlan/marconi-wifi device tree and Wi-Fi driver interface',
    'com.apple.bluetoothd': 'Bluetooth controller transport; also hosts BTAudioHALPlugin.xpc',
    'com.apple.nfcd': 'NFC controller and secure-element interfaces',
    'com.apple.awdd': 'WirelessDiagnostics clients and persistent diagnostics storage',
    'com.apple.analyticsd': 'CoreAnalyticsMessenger matching and diagnostics storage',
    'com.apple.passd': 'keychain persistence; NFC/secure element required for payment paths',
    'com.apple.biometrickitd': 'biometric sensor and secure-enclave interface',
    'com.apple.ScreenTimeAgent': 'keybag first-unlock, ScreenTime databases and account services',
    'com.apple.nanotimekitcompaniond': 'Watch pairing, NanoRegistry and snapshot rendering',
    'com.apple.fairplayd.H2': 'FairPlay key services and protected-content hardware contracts',
    'com.apple.ptpd': 'IOUSBDeviceInterface with USBDeviceFunction=PTP',
    'com.apple.searchd': 'Spotlight index storage, LaunchServices and keybag lifecycle',
}


def read(path):
    with Path(path).open('rb') as stream:
        return plistlib.load(stream)


def write(path, value, *, mode=0o600, owner=(0, 0)):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + '.', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'wb') as stream:
            plistlib.dump(value, stream, fmt=plistlib.FMT_BINARY)
            stream.flush()
            os.fsync(stream.fileno())
        os.chown(temporary, *owner)
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def preserve_and_disable(root, labels):
    """Keep original definitions so a workaround can be reversed precisely."""
    root = Path(root)
    path = root / CACHE
    state = path.stat()
    cache = read(path)
    archive_path = root / ARCHIVE
    archive = read(archive_path) if archive_path.exists() else {}
    changed = []
    for key, job in cache['LaunchDaemons'].items():
        if job.get('Label') in labels:
            archive.setdefault(key, copy.deepcopy(job))
            job['Disabled'] = True
            changed.append(job['Label'])
    # Save the recovery data before modifying the launchd cache.
    write(archive_path, archive)
    panic_keys = 0
    for job in cache['LaunchDaemons'].values():
        if '_PanicOnCrash' in job:
            job['_PanicOnCrasX'] = job.pop('_PanicOnCrash')
            panic_keys += 1
    write(path, cache, mode=state.st_mode & 0o7777,
          owner=(state.st_uid, state.st_gid))
    print(f'launchd.plist: {panic_keys} panic keys renamed; '
          f'{len(changed)} jobs disabled with definitions preserved')
    missing = set(labels) - set(changed)
    if missing:
        print('Previously removed jobs require explicit restoration: ' + ', '.join(sorted(missing)))


def restore(root, label, source_cache=None):
    root = Path(root)
    path = root / CACHE
    state = path.stat()
    cache = read(path)
    source = Path(source_cache) if source_cache else root / ARCHIVE
    if not source.exists():
        raise ValueError('No preserved jobs; provide --source-cache from the same iOS build')
    originals = read(source)
    originals = originals.get('LaunchDaemons', originals)
    matches = [(key, job) for key, job in originals.items()
               if isinstance(job, dict) and job.get('Label') == label]
    if len(matches) != 1:
        raise ValueError(f'Expected one original definition for {label}; found {len(matches)}')
    key, original = matches[0]
    if not isinstance(key, str) or not key.startswith('/System/Library/'):
        raise ValueError('Original job cache key must be an absolute system plist path')
    jobs = cache['LaunchDaemons']
    duplicates = [k for k, job in jobs.items() if job.get('Label') == label and k != key]
    if duplicates:
        raise ValueError(f'Conflicting cached definitions for {label}: {duplicates}')
    job = copy.deepcopy(original)
    if job.get('Disabled'):
        raise ValueError('The original definition is disabled; select a valid original source')
    if '_PanicOnCrash' in job:
        job['_PanicOnCrasX'] = job.pop('_PanicOnCrash')
    jobs[key] = job
    write(path, cache, mode=state.st_mode & 0o7777, owner=(state.st_uid, state.st_gid))
    print(f'Restored definition: {label}')
    print('Required dependencies: ' + DEPENDENCIES.get(label, 'inspect the original service contract'))


def remove_poster_injection(environment):
    """Remove this component's obsolete injection token, preserving other libraries."""
    value = environment.get('DYLD_INSERT_LIBRARIES')
    if value is None:
        return
    if not isinstance(value, str):
        raise ValueError('Unexpected PosterBoard DYLD_INSERT_LIBRARIES value')
    libraries = value.split(':')
    if POSTER_LIBRARY not in libraries:
        return
    remaining = [item for item in libraries if item != POSTER_LIBRARY]
    if any(remaining):
        environment['DYLD_INSERT_LIBRARIES'] = ':'.join(remaining)
    else:
        environment.pop('DYLD_INSERT_LIBRARIES', None)


def prepare_posters(root, build):
    """Install UUID-gated readiness and an explicitly invoked query diagnostic."""
    if os.name != 'posix':
        raise ValueError('Poster preparation requires an offline POSIX root shell')
    root, build = Path(root), Path(build)
    version = read(root / 'System/Library/CoreServices/SystemVersion.plist')
    if version.get('ProductBuildVersion') != '20H392':
        raise ValueError('Poster preparation has only been inspected for iOS build 20H392')
    artifacts = []
    for stem, filename, kind, destination in (
        ('SweetPosterBoard', 'SweetPosterBoard.dylib', 6, 'usr/local/lib/sweet/SweetPosterBoard.dylib'),
        ('SweetRegisterPosters', 'SweetRegisterPosters', 2, 'usr/local/libexec/SweetRegisterPosters'),
    ):
        manifest = json.loads((build / (stem + '.build.json')).read_text(encoding='utf-8'))
        data = (build / filename).read_bytes()
        source = Path(__file__).with_name('posterboard') / (stem + '.c')
        if manifest.get('os_build') != '20H392' or hashlib.sha256(data).hexdigest() != manifest.get('binary_sha256'):
            raise ValueError(f'Invalid build manifest/binary: {filename}')
        if hashlib.sha256(source.read_bytes()).hexdigest() != manifest.get('source_sha256'):
            raise ValueError(f'Source changed since build: {source.name}')
        if len(data) < 32 or struct.unpack_from('<II', data) != (0xfeedfacf, 0x100000c) or struct.unpack_from('<I', data, 12)[0] != kind:
            raise ValueError(f'Invalid arm64 Mach-O kind: {filename}')
        command_count, command_bytes = struct.unpack_from('<II', data, 16)
        if command_bytes > len(data) - 32:
            raise ValueError(f'Truncated Mach-O commands: {filename}')
        offset, platform, dependencies = 32, None, []
        for _ in range(command_count):
            if offset + 8 > 32 + command_bytes:
                raise ValueError(f'Truncated Mach-O command: {filename}')
            command, size = struct.unpack_from('<II', data, offset)
            if size < 8 or size > 32 + command_bytes - offset:
                raise ValueError(f'Invalid Mach-O command: {filename}')
            if command == 0x32 and size >= 24:
                platform = struct.unpack_from('<I', data, offset + 8)[0]
            if command == 0xc and size >= 24:
                start = struct.unpack_from('<I', data, offset + 8)[0]
                if start < 24 or start >= size:
                    raise ValueError(f'Invalid dependency name: {filename}')
                dependencies.append(data[offset + start:offset + size].split(b'\0', 1)[0].decode())
            offset += size
        if platform != 2:
            raise ValueError(f'Artifact does not target iOS: {filename}')
        if stem == 'SweetPosterBoard' and '/System/Library/PrivateFrameworks/PosterBoard.framework/PosterBoard' not in dependencies:
            raise ValueError('Readiness guard requires a strong PosterBoard initialization dependency')
        artifacts.append((root / destination, data))

    cache_path = root / CACHE
    state = cache_path.stat()
    cache = read(cache_path)
    matches = [(key, job) for key, job in cache['LaunchDaemons'].items()
               if job.get('Label') == 'com.apple.PosterBoard']
    if len(matches) != 1:
        raise ValueError('Expected exactly one existing PosterBoard launchd job')
    poster_key, poster_job = matches[0]
    if poster_job.get('ProgramArguments') != ['/Applications/PosterBoard.app/PosterBoard'] or poster_job.get('UserName') != 'mobile':
        raise ValueError('Unexpected PosterBoard launchd contract')
    if not isinstance(poster_job.get('EnvironmentVariables', {}), dict):
        raise ValueError('Unexpected PosterBoard environment contract')
    remove_poster_injection(copy.deepcopy(poster_job.get('EnvironmentVariables', {})))
    preparation_key = '/System/Library/LaunchDaemons/org.heliodarwin.prepare-posters.plist'
    existing = cache['LaunchDaemons'].get(preparation_key)
    if existing and existing.get('Label') != 'org.heliodarwin.prepare-posters':
        raise ValueError('Conflicting poster preparation job')
    preparation_file = root / preparation_key.lstrip('/')
    if preparation_file.exists() and read(preparation_file).get('Label') != 'org.heliodarwin.prepare-posters':
        raise ValueError('Conflicting poster preparation plist')
    archive_path = root / ARCHIVE
    archive = read(archive_path) if archive_path.exists() else {}
    archive.setdefault(poster_key, copy.deepcopy(poster_job))
    if not isinstance(archive[poster_key], dict) or archive[poster_key].get('Label') != 'com.apple.PosterBoard':
        raise ValueError('Conflicting original PosterBoard job archive')
    write(archive_path, archive)
    for path, data in artifacts:
        missing = []
        directory = path.parent
        while not directory.exists():
            missing.append(directory)
            directory = directory.parent
        path.parent.mkdir(parents=True, exist_ok=True)
        for directory in missing:
            os.chown(directory, 0, 0)
            os.chmod(directory, 0o755)
        descriptor, temporary = tempfile.mkstemp(prefix=path.name + '.', dir=path.parent)
        try:
            with os.fdopen(descriptor, 'wb') as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            os.chown(temporary, 0, 0)
            os.chmod(temporary, 0o755)
            os.replace(temporary, path)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
    # The verified library must exist before the executable acquires its normal
    # dependency. Keep the old original-job archive when migrating injection.
    sweet_macho_dependency.install(
        root, 'Applications/PosterBoard.app/PosterBoard', POSTER_LIBRARY,
        'ec7ac4c9-f35c-3f28-a3eb-e50973113683',
        '644656cf6f6da07dc8a42b49f95b188aa1973a7d5a1fd12f5c1e8d40cea315be',
        POSTER_DEPENDENCY_STATE)
    environment = poster_job.setdefault('EnvironmentVariables', {})
    remove_poster_injection(environment)
    environment['SWEET_POSTERBOARD_READINESS'] = '1'
    log_directory = root / 'private/var/mobile/Library/Logs'
    if not log_directory.exists():
        log_directory.mkdir(parents=True)
        os.chown(log_directory, 501, 501)
        os.chmod(log_directory, 0o755)
    poster_job.setdefault('StandardErrorPath', POSTER_LOG)
    job = {
        'Label': 'org.heliodarwin.prepare-posters', 'UserName': 'mobile',
        # This helper is not an entitled PosterKit host. Native PosterBoard
        # owns discovery and prewarm; a denied helper query cannot justify
        # unregistering and replacing its genuine provider records at boot.
        'ProgramArguments': ['/usr/local/libexec/SweetRegisterPosters'],
        'RunAtLoad': False, 'KeepAlive': False, 'ProcessType': 'Background',
        'StandardOutPath': '/private/var/mobile/Library/Logs/SweetRegisterPosters.log',
        'StandardErrorPath': '/private/var/mobile/Library/Logs/SweetRegisterPosters.log',
    }
    write(root / preparation_key.lstrip('/'), job, mode=0o644)
    cache['LaunchDaemons'][preparation_key] = job
    write(cache_path, cache, mode=state.st_mode & 0o7777, owner=(state.st_uid, state.st_gid))
    print('Poster readiness installed; helper retained for explicit diagnostics; native services own discovery and prewarm')


def remove_poster_preparation(root):
    if os.name != 'posix':
        raise ValueError('Poster removal requires an offline POSIX root shell')
    root = Path(root)
    path = root / CACHE
    cache = read(path)
    key = '/System/Library/LaunchDaemons/org.heliodarwin.prepare-posters.plist'
    existing = cache['LaunchDaemons'].get(key)
    file = root / key.lstrip('/')
    if existing and existing.get('Label') != 'org.heliodarwin.prepare-posters':
        raise ValueError('Conflicting poster preparation job')
    if file.exists() and read(file).get('Label') != 'org.heliodarwin.prepare-posters':
        raise ValueError('Conflicting poster preparation plist')
    originals = read(root / ARCHIVE)
    matches = [(name, job) for name, job in originals.items()
               if isinstance(job, dict) and job.get('Label') == 'com.apple.PosterBoard']
    if len(matches) != 1:
        raise ValueError('Expected one original PosterBoard job for preparation rollback')
    poster_key, original = matches[0]
    current = cache['LaunchDaemons'].get(poster_key)
    if not current or current.get('Label') != 'com.apple.PosterBoard':
        raise ValueError('PosterBoard cached job changed outside this preparation')
    if sum(job.get('Label') == 'com.apple.PosterBoard' for job in cache['LaunchDaemons'].values()) != 1:
        raise ValueError('Conflicting cached PosterBoard job definitions')
    if not isinstance(current.get('EnvironmentVariables', {}), dict):
        raise ValueError('Unexpected PosterBoard environment contract')
    # Restore the executable before removing any opt-in or startup policy. The
    # common module refuses unrelated executable changes and preserves recovery.
    sweet_macho_dependency.remove(root, POSTER_DEPENDENCY_STATE)
    state = path.stat()
    environment = current.get('EnvironmentVariables', {})
    remove_poster_injection(environment)
    original_environment = original.get('EnvironmentVariables', {})
    if environment.get('SWEET_POSTERBOARD_READINESS') == '1':
        if 'SWEET_POSTERBOARD_READINESS' in original_environment:
            environment['SWEET_POSTERBOARD_READINESS'] = original_environment['SWEET_POSTERBOARD_READINESS']
        else:
            environment.pop('SWEET_POSTERBOARD_READINESS', None)
    if not environment:
        current.pop('EnvironmentVariables', None)
    if current.get('StandardErrorPath') == POSTER_LOG:
        if 'StandardErrorPath' in original:
            current['StandardErrorPath'] = original['StandardErrorPath']
        else:
            current.pop('StandardErrorPath', None)
    cache['LaunchDaemons'].pop(key, None)
    write(path, cache, mode=state.st_mode & 0o7777, owner=(state.st_uid, state.st_gid))
    if file.exists():
        file.unlink()
    print('Poster dependency restored and startup policy removed; tools, logs and provider data retained')


def repair_poster_metadata(root, build):
    """Opt in to scoped native MI/LS metadata repair in the existing mobile lsd."""
    if os.name != 'posix' or os.geteuid() != 0:
        raise ValueError('Poster metadata dependency installation requires an offline POSIX root shell')
    root, build = Path(root).resolve(strict=True), Path(build)
    if read(root / 'System/Library/CoreServices/SystemVersion.plist').get('ProductBuildVersion') != '20H392':
        raise ValueError('Poster metadata repair supports only the inspected 20H392 root')
    stem = 'SweetRepairPosterMetadata'
    manifest = json.loads((build / (stem + '.build.json')).read_text(encoding='utf-8'))
    source = Path(__file__).with_name('posterboard') / (stem + '.c')
    data = (build / (stem + '.dylib')).read_bytes()
    expected_uuid = 'cfa9bb6b-7d4b-3397-94f6-595d6f1eddaa'
    expected_sha = 'c46e8e7330ab1c008aa50873d230a44f68eb57d675e3fe0b01e0917f9596b2e1'
    if (manifest.get('os_build') != '20H392' or manifest.get('host') != '/usr/libexec/lsd' or
            manifest.get('host_uid') != 501 or manifest.get('host_uuid') != expected_uuid or
            manifest.get('host_preimage_sha256') != expected_sha or
            hashlib.sha256(source.read_bytes()).hexdigest() != manifest.get('source_sha256') or
            hashlib.sha256(data).hexdigest() != manifest.get('binary_sha256') or
            sweet_macho_dependency.image_info(data, filetype=6).get('install_name') != POSTER_REPAIR_LIBRARY):
        raise ValueError('Recovery artifact does not match its current source and supported host')
    target = sweet_macho_dependency.checked_path(root, 'usr/libexec/lsd')
    state_path = sweet_macho_dependency.checked_path(root, POSTER_REPAIR_STATE)
    backup = sweet_macho_dependency.checked_path(root, POSTER_REPAIR_STATE + '.executable')
    sweet_macho_dependency.file_metadata(target)
    original = backup.read_bytes() if state_path.exists() else target.read_bytes()
    patched = sweet_macho_dependency.add_dependency(original, POSTER_REPAIR_LIBRARY, expected_uuid, expected_sha)
    if target.read_bytes() not in (original, patched):
        raise ValueError('lsd differs from the genuine preimage and exact dependency candidate')
    destination = sweet_macho_dependency.checked_path(root, POSTER_REPAIR_LIBRARY.lstrip('/'))
    if destination.exists():
        sweet_macho_dependency.file_metadata(destination)
        if destination.read_bytes() != data:
            raise ValueError('A different recovery library already exists; roll back before replacing it')
    else:
        missing, directory = [], destination.parent
        while not directory.exists():
            missing.append(directory)
            directory = directory.parent
        destination.parent.mkdir(parents=True, exist_ok=True)
        for directory in missing:
            os.chown(directory, 0, 0)
            os.chmod(directory, 0o755)
        sweet_macho_dependency.atomic_write(destination, data, 0o755, 0, 0)
    if destination.read_bytes() != data:
        raise ValueError('Recovery library failed readback')
    sweet_macho_dependency.install(root, 'usr/libexec/lsd', POSTER_REPAIR_LIBRARY,
                                   expected_uuid, expected_sha, POSTER_REPAIR_STATE)
    print('Installed opt-in scoped mobile-lsd dependency; native preflight and repair remain runtime-pending')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--restore', metavar='LABEL')
    parser.add_argument('--prepare-posters', type=Path, metavar='BUILD_DIR',
                        help='opt in to verified poster readiness and an explicitly invoked query diagnostic')
    parser.add_argument('--remove-poster-preparation', action='store_true',
                        help='restore the original PosterBoard job and remove the preparation job')
    parser.add_argument('--source-cache', type=Path,
                        help='original launchd cache from the same iOS build')
    parser.add_argument('--repair-poster-metadata', type=Path, metavar='BUILD_DIR',
                        help='opt in to genuine scoped MI/LS metadata recovery in mobile lsd')
    parser.add_argument('--remove-poster-metadata-repair', action='store_true',
                        help='restore the exact lsd preimage using recorded metadata and rollback guards')
    args = parser.parse_args()
    operations = (bool(args.restore), bool(args.prepare_posters), args.remove_poster_preparation,
                  bool(args.repair_poster_metadata), args.remove_poster_metadata_repair)
    if sum(operations) > 1 or ((args.repair_poster_metadata or args.remove_poster_metadata_repair) and args.source_cache):
        parser.error('Select one write operation')
    if args.repair_poster_metadata:
        repair_poster_metadata(args.root, args.repair_poster_metadata)
        return
    if args.remove_poster_metadata_repair:
        restored = sweet_macho_dependency.remove(args.root, POSTER_REPAIR_STATE)
        print('Restored exact lsd preimage; recovery library and provider data retained' if restored else
              'No recorded lsd recovery dependency state; no executable restoration performed')
        return
    if args.remove_poster_preparation:
        if args.prepare_posters or args.restore or args.source_cache:
            parser.error('--remove-poster-preparation cannot be combined with other write operations')
        remove_poster_preparation(args.root)
        return
    if args.prepare_posters:
        if args.restore or args.source_cache:
            parser.error('--prepare-posters cannot be combined with daemon restoration')
        prepare_posters(args.root, args.prepare_posters)
        return
    if args.restore:
        restore(args.root, args.restore, args.source_cache)
        return
    jobs = read(args.root / CACHE)['LaunchDaemons']
    overrides_path = args.root / 'private/var/db/com.apple.xpc.launchd/disabled.plist'
    overrides = read(overrides_path) if overrides_path.exists() else {}
    for label, dependency in DEPENDENCIES.items():
        matches = [job for job in jobs.values() if job.get('Label') == label]
        status = 'removed' if not matches else 'present'
        if matches and overrides.get(label, matches[0].get('Disabled', False)):
            status = 'disabled'
        print(f'{label}: {status}; requires {dependency}')


if __name__ == '__main__':
    main()
