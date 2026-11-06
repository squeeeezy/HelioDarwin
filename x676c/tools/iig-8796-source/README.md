# Pinned XNU-8796 generated DriverKit source

This package contains only the 17 generated headers and 17 generated C++ sources
from the successful existing SWEET build tree: 829,449 bytes of text, compressed
to 101,385 bytes. It contains no IIG executable, SDK header tree or compiled code.
The headers preserve the Apple source license notices and original source markers.
The source artifact came from [successful CI run 39984534077](https://github.com/squeeeezy/HelioDarwin/actions/runs/39984534077),
repository revision `f2e9307049a23c8f302eedab18e753038f735842`.

`manifest.json` pins the exact output map, sizes and SHA-256 hashes, the 17 open
source `.iig` definitions, reachable open source headers, the two generated BSD
include dependencies, the IORPC headers, build recipes and master version. It
records the original CI log digest, local source checkpoint and generator:
Xcode 15.0.1, `DriverKit-286`, macOS 14.0 SDK, with the recorded IIG flags.

The historical SDK headers were not retained. This is reuse of immutable generated
source, not a claim that the old generator has been reproduced. Dependency analysis
used a substitute Darwin standard-header set to identify the open source include
closure; those substitute headers are not packaged. External includes resolve
fixed-width integer types, whose widths and the target RPC packing/offsets/wire
versions are admitted again by `../iig-8796-abi.h` in the actual kernel C++ build.
The workflow passes that force-include through a make command-line assignment in
`MAKEFLAGS`, because XNU resets an environment-only `OTHER_CXXFLAGS` value.

`../iig-8796.py` accepts only macOS source selection 13.5, DEVELOPMENT, SWEET, the
recorded definitions, input hashes, include order and preprocessing flags, and
the explicitly selected Xcode 16.0 macOS 15.0 SDK path. It refuses changes before
writing outputs. Each output is replaced atomically after the complete package
and all inputs have been verified. It does not invoke a newer generator, alter
`IORPC`, remove generated fields, or modify runtime IPC behavior.

The pinned upstream script has two native object roots. Initial `installhdrs`
uses `build/xnu-hdrs.obj`, with IIG running in
`DEVELOPMENT_ARM64_VMAPPLE/iokit/DriverKit`. The later kernel uses `build/xnu.obj`,
with IIG in `DEVELOPMENT_ARM64_SWEET/iokit/DriverKit` or
`DEVELOPMENT_ARM64_SWEET/iokit/DEVELOPMENT/DriverKit`. Only those exact root and
working-directory pairs are admitted; the two generated BSD input hashes are
checked in the selected root. The workflow's global `MACHINE_CONFIG=SWEET` does
not replace the header stage's native VMAPPLE configuration.

Changing the source interfaces, generator flags or target requires a new reviewed
source package. A full current-toolchain kernel compile/link remains mandatory;
successful replay alone does not qualify the new runner or toolchain.
