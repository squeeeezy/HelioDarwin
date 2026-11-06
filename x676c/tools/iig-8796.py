#!/usr/bin/env python3
"""Replay pinned DriverKit-286 generated OSS source for one verified XNU target.

This is not an IIG implementation. Unknown inputs, flags and configurations fail
before any output is written. The old generator and SDK are not redistributed.
"""
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tarfile
import tempfile


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main(argv):
    bundle = Path(__file__).resolve().with_name('iig-8796-source')
    manifest = json.loads((bundle / 'manifest.json').read_text())
    for key, value in manifest['config'].items():
        require(os.environ.get(key) == value, f'unsupported {key}: expected {value}')
    require(len(argv) >= 9 and argv[8] == '--', 'expected exact IIG option prefix and --')
    require(argv[0::2][:4] == ['--def', '--header', '--impl', '--framework-name'],
            'unsupported IIG options')
    definition = Path(argv[1]).resolve()
    root = definition.parents[2]
    require(definition.parent == root / 'iokit/DriverKit', 'unexpected definition directory')
    require(argv[7] == 'DriverKit', 'unsupported framework')
    require(definition.name in manifest['mapping'], 'unknown definition')
    mapping = manifest['mapping'][definition.name]
    header, impl = Path(argv[3]), Path(argv[5])
    for path, name in [(header, mapping['header']), (impl, mapping['impl'])]:
        require(path.name == name and path.resolve().parent == Path.cwd().resolve(),
                'output must be the mapped filename in the current build directory')

    tail = argv[9:]
    includes = []
    while tail and tail[0].startswith('-I'):
        item = tail.pop(0)
        if item == '-I':
            require(bool(tail), 'missing include directory')
            item += tail.pop(0)
        includes.append(Path(item[2:]).resolve())
    require(len(includes) == 5, f'unexpected IIG include count: {includes}')
    obj = includes[3].parent
    require(includes == [root / 'iokit', root / 'osfmk', root / 'bsd', obj / 'bsd',
                         root / 'EXTERNAL_HEADERS'], f'unexpected IIG include order: {includes}')
    stages = {
        root.parent / 'build/xnu-hdrs.obj': [
            'DEVELOPMENT_ARM64_VMAPPLE/iokit/DriverKit'],
        root.parent / 'build/xnu.obj': [
            'DEVELOPMENT_ARM64_SWEET/iokit/DriverKit',
            'DEVELOPMENT_ARM64_SWEET/iokit/DEVELOPMENT/DriverKit'],
    }
    require(obj in stages,
            f'unexpected object root: {obj}; expected {list(stages)}; cwd={Path.cwd()}')
    admitted_directories = [obj / relative for relative in stages[obj]]
    require(Path.cwd().resolve() in admitted_directories,
            f'unexpected IIG working directory: {Path.cwd()}; expected {admitted_directories}')
    require(len(tail) >= 2 and tail[0] == '-isysroot', 'missing SDK argument')
    require(Path(tail[1]).name in ['MacOSX15.sdk', 'MacOSX15.0.sdk'],
            'expected pinned Xcode 16.0 macOS 15.0 SDK')
    require(tail[2:] == manifest['preprocessor_flags'], 'unsupported IIG preprocessor flags')
    actual_defs = {p.name for p in (root / 'iokit/DriverKit').glob('*.iig')}
    require(actual_defs == set(manifest['mapping']), 'definition set changed')
    for name, expected in manifest['inputs'].items():
        origin, relative = name.split('/', 1)
        base = root if origin == 'source' else obj
        path = base / relative
        require(path.is_file() and digest(path.read_bytes().replace(b'\r\n', b'\n')) == expected,
                f'pinned input changed or missing: {name}')

    package = (bundle / 'generated.tar.gz').read_bytes()
    require(digest(package) == manifest['package_sha256'], 'generated source package changed')
    payloads = {}
    with tarfile.open(fileobj=io.BytesIO(package), mode='r:gz') as archive:
        members = archive.getmembers()
        require(len(members) == 34 and {m.name for m in members} == set(manifest['outputs']),
                'generated source file set changed')
        for member in members:
            require(member.isfile() and '/' not in member.name, 'invalid package member')
            metadata = manifest['outputs'][member.name]
            require(member.size == metadata['size'], 'generated source size changed')
            data = archive.extractfile(member).read()
            require(digest(data) == metadata['sha256'], 'generated source hash changed')
            payloads[member.name] = data
    # All admission checks complete before either output is opened.
    staged = []
    try:
        for path, name in [(header, mapping['header']), (impl, mapping['impl'])]:
            with tempfile.NamedTemporaryFile(dir=path.resolve().parent,
                                             prefix=path.name + '.', delete=False) as stream:
                temporary = Path(stream.name)
                staged.append((temporary, path))
                stream.write(payloads[name])
        for temporary, path in staged:
            temporary.replace(path)
    finally:
        for temporary, _ in staged:
            temporary.unlink(missing_ok=True)
    print(f'IIG pinned DriverKit-286 source: {definition.name}')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except (OSError, ValueError, KeyError, IndexError, tarfile.TarError) as exc:
        sys.exit(f'iig-8796: refusing replay: {exc}')
