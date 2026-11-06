# HelioDarwin Technical Wiki

Implementation reference for XNU `xnu-8796` and iOS 16.7.16 (`20H392`, D22AP)
on the Infinix Note 12 2023 (`X676C`, MediaTek MT6789 / Helio G99).

## Current status

Physical boots reach iOS userland. After patch 0190, the panel displayed the
clock/interface and a diagnostic boot returned automatically to Android B.
The observed interface uses the system's software rendering fallback.

**GPU power-on is already confirmed:** PMIC rail-enable readbacks, MFG power
status and readable GPU identification have been observed. Native Vulkan device
creation has also succeeded. Run `40224078640` records ten hardware JM
completions, including fragment jobs. Correct rendered frames, performance and
full DVFS remain unvalidated or unfinished.

The new MaliMetal source includes Metal objects, native AIR translation, an
arm64 iOS PanVK build and a versioned XNU GPU transport. Offline compilation
and selected shader checks pass. The first integrated phone boot loaded PanVK
and read its GPU identity, then backboardd repeatedly trapped on `CTR_EL0`.
Patch 0193 addresses the user cache-query permission and I2C0 gate selection.
Run `40083985422` passed the cache-register trap and recorded real touch
press/release events in the kernel HID path. It then failed to create a Vulkan
logical device because that path still expected a Unix descriptor. The current
source uses a separately owned IOKit connection for each logical device.
Run `40090751567` subsequently created a native Vulkan device. Frontend updates
passed Metal registration and reached CoreAnimation shader loading; the observed
function-descriptor API gap passed a phone check and exposed missing MRT/blending.
The integrated ABI 4/compute ABI 2 candidate now uses canonical per-draw and
dispatch planning, with native graphics/resource/synchronization modules and
declared compiler patches. Strict linking/static review pass and hardware
JM completion is recorded; expanded API behavior and correct pixels still require validation.
Later diagnostics identified MPS compute-descriptor, signed extension-cache
registration and HFS file-protection query gaps. The current candidate addresses
those bounded contracts; runtime acceptance remains pending. Touch interaction was confirmed and kernel HID
logs include physical power/volume-key events. GPU pixel correctness, multi-touch, volume and
sleep/wake behavior, and the installed PosterBoard/native display adapters still
require runtime validation.

The preceding [signed-cache capture](https://github.com/squeeeezy/HelioDarwin/blob/x676c/logs/boot-0202-run-40219933214-ui-signed-cache.log)
records successful AMFI membership queries and PosterBoard startup.
That earlier capture retained texture-layout and native memory failures and no
confirmed rendered UI; the user returned manually to Android.

The [00:26:36 capture](https://github.com/squeeeezy/HelioDarwin/blob/x676c/logs/boot-0205-run-40224078640-ui-texture-layout.log)
on 2026-10-26 records ten hardware JM completions, then identifies the
adapter's 128-live-BO refusal and PosterBoard configuration failures.
Correct UI output and wallpaper remain unconfirmed.

The later [01:29:49 capture](https://github.com/squeeeezy/HelioDarwin/blob/x676c/logs/boot-0205-run-40224078640-ui-bo-poster.log)
records 128 logical BOs on 20 native backings after suballocation, ten successful
hardware JM terminal results and no retained BO-allocation failure. PosterBoard
still traps on the existing malformed provider records. Android returned
automatically around 01:42; the user saw no clock or wallpaper.

On 2026-11-06, run `42153642642` restored nine-provider discovery and native
configuration delivery to SpringBoard. Hardware jobs continued completing, but
the display showed corrupt geometry; sampled panel-memory frames were already
corrupt before scanout. Correct UI output remains unresolved.

The source now adds per-job hardware AS leasing and optional measured GPU timing, with kernel CI `42175315331` passed and hardware
qualification pending. The corrupt-frame cause remains unknown and further
diagnostics are available.

## Documentation boundaries

Hardware observations, implemented source and offline checks are identified
separately. A discovered register, present service, compiled shader or successful
build does not by itself establish a working subsystem. Full Metal/Vulkan
conformance and complete hardware support are not claimed.

iOS executables and frameworks are supplied separately. Root preparation changes
configuration; the optional compatibility libraries adapt inspected private API
contracts at runtime. Their scope and rollback are documented explicitly.

## Articles

| Article | Scope |
| --- | --- |
| [Hardware Architecture](Hardware-Architecture.md) | CPU topology, RAM and peripheral bases. |
| [Boot and Shim](Boot-and-Shim.md) | LK handoff, payload, ADT and kernel entry. |
| [Display Pipeline](Display-Pipeline.md) | DSI presentation and native UI geometry. |
| [Input and HID](Input-and-HID.md) | FT5452J, PMIC/keypad buttons and event delivery. |
| [GPU Power Sequence](MT6789-GPU-Power-Sequence.md) | PMIC, MFG clocks, GPU identity and JM transport. |
| [MaliMetal Driver](MaliMetal-Driver.md) | Metal, AIR, Vulkan and kernel integration. |
| [System Services and Posters](System-Services-and-Posters.md) | Service dependencies, PosterBoard and native display preparation. |
| [Pstore Debugging](Pstore-Debugging-on-MediaTek.md) | Console retention and raw capture. |
| [Dual-Boot Safety](Dual-Boot-Safety.md) | Slot selection, reset and deployment scope. |

## Target hardware

The SoC has six Cortex-A55 and two Cortex-A76 cores. The current ADT publishes
only the six A55 cores, with 16 KiB CPU pages and a rank-1 RAM window. The board
has a 1080×2400 ili7835 command-mode panel and a Mali-G57 MC2 Job Manager GPU.
The complete installed RAM and CPU inventory is distinct from the resources
currently managed by XNU.

## Credits

[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet),
[MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz), Apple open-source XNU,
Mesa/Panfrost/PanVK, the referenced AIR/v9 compiler projects and Linux MediaTek
contributors. Third-party code retains its own license notices.
