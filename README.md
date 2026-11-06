# HelioDarwin

Running Apple's open-source XNU kernel (`xnu-8796`, Darwin 22 / iOS 16.7.16 `20H392`
from iPhone X `D22AP`) and iOS 16 userland on the **MediaTek Helio G99
(`MT6789`)**, target device **Infinix Note 12 2023 (`X676C`)**. Forked from
[MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz).

This is experimental bring-up on physical hardware. Booting the system, displaying
its interface, and executing graphics on the GPU are separate milestones. The
earlier clock/interface used the system's software rendering fallback. The
2026-10-26 capture confirms native Mali JM completion, including fragment jobs;
correct UI pixels, wallpaper and performance remain unvalidated.
Userspace configuration changes and optional compatibility adapters are described
in the [system services documentation](wiki/System-Services-and-Posters.md).

## Hardware status

| Subsystem | Evidence and current limits |
| :--- | :--- |
| Boot & CPU | Physical boots reach iOS userland. The ADT publishes six Cortex-A55 cores with 16 KiB pages; the two A76 cores are excluded. [Boot and Shim](wiki/Boot-and-Shim.md). |
| Storage | Native UFS reads the `iosroot` HFS+ volume and has performed writes and journal replay. Root-volume backups must be kept separately. |
| Display | Patch 0190 serializes the vendor DSI refresh sequence. A subsequent phone boot showed the clock/interface and no sampled DSI busy condition. A 60 Hz software timer is not a measured frame-rate guarantee. [Display Pipeline](wiki/Display-Pipeline.md). |
| UIKit geometry | Physical scanout is 1080×2400. Native cutout, safe-area and status-bar compatibility work is separate from scanout and requires runtime validation. |
| GPU | Power-on is confirmed: PMIC rail-enable readbacks, MFG power status and GPU ID `0x90930010` were observed. Native Vulkan device creation also succeeded. Ten native JM completions, including fragment jobs, are recorded in run `40224078640`. Correct rendered frames, performance and full DVFS remain unvalidated or unfinished. [GPU power sequence](wiki/MT6789-GPU-Power-Sequence.md). |
| MaliMetal | Metal objects, native AIR translation, a Vulkan backend and an arm64 iOS PanVK port are implemented for a bounded feature set. Selected real CoreAnimation shaders pass offline translation checks. Hardware JM execution is confirmed; correct UI presentation and broader Metal support remain unverified/incomplete. [Driver details](wiki/MaliMetal-Driver.md). |
| Touch & keys | Kernel HID logs contain real touch and power/volume-key press/release events; touch interaction was confirmed on the phone. Multi-touch, volume behavior and display sleep/wake still need validation. [Input and HID](wiki/Input-and-HID.md). |
| System services | PosterBoard startup preparation and reversible job management are available. Disabled services are not considered working merely because their definitions are restored. [Services and posters](wiki/System-Services-and-Posters.md). |
| Reset & fallback | The opt-in X676C TOPRGU reset returned a diagnostic boot to Android slot B. A/B retry metadata alone cannot reset a hung system. [Dual-boot safety](wiki/Dual-Boot-Safety.md). |
| Wi-Fi / BT / NFC / cellular / audio | Functional MT6789 support is not established. Hardware addresses or discovered vendor files do not constitute working drivers. |

## Repository layout

- `x676c/` — device shim, ADT/payload/boot/flash tooling.
- `xnu/patches/` — numbered XNU source patches applied by CI before the kernel build.
- `MaliMetal/` — userspace Metal implementation, AIR compiler adapter and Vulkan/XNU backend.
- `tools/` — offline root preparation and explicit compatibility adapters.
- `wiki/` — public hardware and implementation documentation, mirrored to the separate wiki.
- `logs/` — selected raw `pstore` captures; [latest completed runs](logs/README.md) include versions, results and retention limits.

## Building and flashing

Requirements: LLVM/Clang 15+ (AArch64), Python 3.10+ with Zig, Android platform-tools
(`adb`) with root (Magisk). The kernel is built in CI; `boot-xnu.ps1` downloads the
artifact, builds the ADT/payload/boot image, flashes `boot_a` and selects slot A with
one try. The next reset can return to Android slot B. Arrange a reset deadline or
manual recovery explicitly; the default command is not a hang watchdog.

```bash
pwsh x676c/tools/boot-xnu.ps1 -Run <CI_RUN_ID>
```

## GPU / Metal status

The current source implements Vulkan resource and command paths. Build matching
iOS artifacts from source, including the pinned AIR compiler and PanVK dependency.
Generated binaries are kept outside version control.
See [MaliMetal build instructions](MaliMetal/README.md).

Run `40090751567` created a native Vulkan device after fixes to assembly
freshness, IOKit connection discovery and physical cache-address conversion.
The updated frontend passed Metal registration and reached CoreAnimation shader
loading. The function-descriptor call subsequently passed on the phone and
reached a missing MRT/blending path. That path is now implemented and compiled,
along with canonical per-draw/dispatch binding and native resource, graphics,
synchronization and API modules. The integrated ABI 4/compute ABI 2 bundle passes
strict linking and static review, with phone validation pending. A separate shared-cache
metadata failure affected SpringBoard and several services. Remaining malformed
poster-provider records and compositor failures are under investigation.
Those earlier milestones did not establish job completion; the
later [00:26:36 capture](logs/boot-0205-run-40224078640-ui-texture-layout.log) records successful hardware JM work.
The [01:29:49 capture](logs/boot-0205-run-40224078640-ui-bo-poster.log) records
128 logical BOs on 20 native backings after suballocation, continued hardware
completion and persistent PosterBoard failures. Correct rendered UI output
remains unconfirmed.

The 2026-11-06 device run restored nine-provider discovery and selected
configuration delivery to SpringBoard. It still showed corrupt geometry;
framebuffer samples place the corruption before physical scanout. Correct
clock, wallpaper and UI output remain unresolved.

Compilation and offline shader validation do not prove that the stock Metal loader,
PanVK, kernel GPU transport and presentation work together on the phone. Full
Metal or Vulkan conformance is not claimed. Resource argument consumers, XR render
semantics, broader formats and advanced API paths remain limited or gated. Native
tile/depth/MSAA helpers and successful linking do not establish full frontend
coverage or GPU execution.

## Development

HelioDarwin is developed by **[@squeeeezy](https://github.com/squeeeezy)**. AI assistance is utilized strictly for boilerplate generation, build script orchestration, and documentation formatting. All kernel architecture, register reverse engineering, driver implementations, and hardware test validation are conducted directly on physical Infinix Note 12 2023 (`X676C`) test hardware.

## Credits

- [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) — XNU booting on Android silicon (Snapdragon / Redmi Note 10 Pro).
- [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz), [topaz-woa](https://github.com/MakrSas/topaz-woa) — upstream bring-up scripts and drivers for Redmi Note 12.
- Apple — open-source XNU (`xnu-8796`) and Apple Device Tree formats.
- Mesa / Panfrost / PanVK — open-source Vulkan for Mali Valhall.
- Linux MediaTek contributors — device trees, power domains, clock drivers.

## Legal

Independent, non-commercial research project. This repository contains original code
(boot shim, drivers, tooling), reverse-engineering notes, and source patches against
Apple's open-source XNU. It does **not** include or redistribute Apple or MediaTek
proprietary binaries, firmware, or operating-system images; those must be supplied by
the user from their own device/licences. Apple's XNU source is used under the Apple
Public Source License 2.0. All trademarks belong to their respective owners.
See [`LICENSE`](LICENSE).
