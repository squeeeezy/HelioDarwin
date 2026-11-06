# HelioDarwin engineering status

Target: Infinix Note 12 2023 (`X676C`), MediaTek Helio G99 (`MT6789`).
Kernel baseline: XNU `xnu-8796`; userspace: iOS 16.7.16 (`20H392`, D22AP).
This page summarizes the current implementation and observed milestones.

## Observed milestones

- LK enters the shim at EL1 with MMU off. The payload loader places XNU and its
  ADT/boot arguments in the configured rank-1 memory window.
- XNU reaches IOKit/BSD startup, mounts the named HFS+ root over the native UFS
  driver and executes launchd. Captures include SpringBoard and backboardd.
- XNU has read Mali `GPU_ID=0x90930010` and availability registers. Register
  access and verified PMIC/MFG power-on establish the GPU power-on path.
  Ten hardware JM completions, including fragment jobs, were recorded in
  the 2026-10-26 run; pixel and numerical correctness remain unverified.
- Following patch 0190, the physical panel displayed the clock/interface.
  A bounded diagnostic boot also returned automatically to Android B using
  TOPRGU. The observed UI used system software rendering.

## Current source

The source now leases hardware address-space slots per validated job while
preserving each client's private VM and buffer ownership and the global
one-active-job rule. Optional measured GPU timing is disabled by default and
requires a matching kernel clock interface, native ICD and bundle. Kernel CI
`42175315331`, native ICD builds and bundle linking passed. These are build
checks; hardware qualification of AS reuse and timing publication remains
pending. The corrupt-geometry cause remains unknown,
and bounded diagnostics are available.

The ADT publishes six A55 cores, GICR base `0x0c040000`, 16 KiB CPU pages and
RAM base `0x140000000` with size `0xffff0000`. Both A76 cores remain excluded.
The old secondary-core display pump and its lifetime timer are not started.

The GPU source uses the shared split-SWINF PMIC interface, preserves inherited
voltage selectors and implements a versioned JM memory/submission/completion
transport. Rail-enable readbacks and native Vulkan device creation have been
observed on the phone. Native JM work has completed on mapped GPU memory.
Isolation, fault recovery, numerical correctness and rendered pixels still require
validation. Full DVFS and dynamic GPU power management remain open.

Input source targets the confirmed FT5452J controller over I2C0/APDMA, the PMIC
power key and keypad volume keys. UFS startup starts the readers; HID receives
contact collections and consumer-button events. Kernel logs contain physical
touch and power/volume-key press/release events, and touch interaction was
confirmed. Multi-touch, volume behavior and display sleep/wake remain unvalidated.

MaliMetal includes native AIR translation, Vulkan commands and an iOS PanVK build.
Selected real CoreAnimation render/compute shaders pass offline checks. Complete
Metal coverage and assembled GPU rendering remain unfinished. PosterBoard and
native UIKit geometry have explicit, build-gated compatibility tools awaiting
device validation at that stage. An earlier diagnostic reached CoreAnimation shader loading;
a function-descriptor API gap passed a phone check, exposing missing MRT/blending
support, followed by native-profile schema and buffer-window failures. The
integrated ABI 4/compute ABI 2 candidate now passes strict native linking and
structural planning for both observed pipelines. Later captures established
hardware JM completion; expanded API/helper behavior and correct pixels still
require validation. The chronological diagnostics below
identify additional MPS, registration and file-mapping contracts.

The 2026-10-25 integrated boot selected Vulkan 1.3, buffer device addresses,
dynamic rendering and color tile reads on the native device. It exposed a
missing blit texture-copy selector, causing repeated backboardd aborts and
dependent display-query failures. The eventual kernel panic was localized to
an unsafe diagnostic stack read. This run does not establish GPU rendering.

The following 01:01:09 boot stopped at the absent `fixed_frag_lpf_cpf` shader
selected when FP16 was reported unavailable. A default-off, exact-G57 native
qualification build and an sRGB/color-table compiler correction are prepared;
FP16 arithmetic and the resulting frames remain unverified on the device.
PosterBoard and native geometry preparation now use guarded dependencies in
the two pinned root executables, preserving their existing entitlement blobs
and providing exact rollback. Installed components still require runtime
constructor, provider-delivery and UIKit-context evidence.

The 17:39:02 trial confirmed enabled FP16 and native-display constructor loading.
It exposed a draw-owner device contract gap and an exact stock donor inset pair
rejected by the geometry input guard. Both bounded fixes compile and preserve
native validation. The poster registration call was still pending when the user
returned manually; that capture ended before the phase timeout. Rendered output,
native context matching, wallpaper delivery and performance remain unverified.

The 18:14:02 retry reported the requested native display context and corner
radius with `match=1`, then SpringBoard aborted in MPS GaussianBlur initialization.
An experimental, default-off native MPS Common backend is now compiled for the
exact framework ABI; it does not advertise an Apple GPU family. Patch 0200 also
separates hardware-watchdog service from display refresh. New-kernel liveness,
actual MPS registration/dispatch, pixels and performance require device evidence.

The subsequent full diagnostic reached the 600-second reset and returned
automatically to Android. Native MPS Common registration/readback passed in
backboardd, while SpringBoard waited on LaunchServices database initialization.
The sampled installation-journal path requested MobileGestalt identity and waited
for missing Wi-Fi metadata. Private provisioned Wi-Fi/Bluetooth inputs now feed
the ADT; runtime identity lookup, seeding, rendered frames and wallpaper delivery
remain the next checks. No wireless-driver or authentic Apple hardware identity
claim is implied.

The 21:10:55 run reached native MPS Common registration in SpringBoard and
the real `D8F18H` compute pipeline request. The native compute descriptor lazily
creates a nonnil, empty stage-input descriptor; rejecting its mere presence
aborted pipeline construction. The frontend now accepts the verified empty
public defaults and rejects configured stage input. Other unsupported controls
report their individual reason. Both observed D8F18H specializations pass offline
compilation and SPIR-V validation; GPU dispatch, pixels and performance still
require a new device run.

Patch 0201 answers `F_GETPROTECTIONCLASS` with class 0 only for a regular,
known-unprotected file on a consistent, non-CoreStorage, non-protected HFS mount.
It checks both volume and file protection keys, including orphaned attribute
extents; unavailable or malformed metadata keeps the operation unsupported.
Attribute-key decoding now validates the exact canonical length before any
B-tree iterator copy. This targets Foundation's observed whole-file read fallback
and driverkitd per-process-limit kills; it does not enable encrypted storage,
raise memory limits or establish that the next run avoids those kills.

The 21:10:55 run returned from the ExtensionKit query with zero providers;
both built-in poster-provider registrations returned false. The inspected native
registration path validates the signed extension cache, then queries the missing
`AppleMobileFileIntegrity` service for static-trust-cache membership.

Patch 0202 supplies that specific service and selector-6 membership contract.
It requires the caller's existing `com.apple.private.amfi.can-check-trust-cache`
entitlement and task binding. The kernel independently pins the original 20H392
static trust cache by SHA256, validates its framing, and owns an immutable copy.
Absent/invalid data, unauthorized callers and nonmembers fail closed. Existing
signature validation and global MAC policy remain unchanged. This is a bounded
software compatibility service, not authenticated Apple boot or a general AMFI
implementation. Runtime cache admission, provider registration and wallpaper
delivery still require the assembled candidate.

The 23:17:49 run on 2026-10-25 used kernel `40219933214` and userspace
`0de001f`. Its original 163,421-byte console records the pinned 2,760-entry
trust set ready and successful real membership calls from `lsd` and `installd`.
PosterBoard and AegirPoster subsequently ran. Native geometry again read back
`match=1`; the previous D8F18H rejection did not recur. This does not establish
completed GPU rendering: backboardd aborted with `buffer memory binding:
VkResult -2`, a label then shared by allocation, mapping and binding failures.
Separate helper logs identify the missing
`newTextureLayoutWithDescriptor:isHeapOrBufferBacked:` selector. Later surface
creation failed with NoMemory. No new driverkitd per-process-limit report was
retained; the exact mmap branch and memory-failure cause remain unproven.

The user saw the bootloader image with artifacts and a spinner, then black screens, and
returned manually to Android. The console reaches sync and the single diagnostic
reset write; its subsequent `reset returned` print also occurs in earlier
automatic-return captures, so it does not identify the reset failure by itself.
Successful GPU job completions were not individually logged in this build.
Buffer swaps and a visible spinner alone do not identify the frame producer.

The next candidate implements the inspected 20H392 texture-layout protocol
for sampled 2D ASTC LDR textures. Its versioned host block representation has its
own watermark and checked mip/region bounds. Shared, shader-read-only
`newTextureWithBytesNoCopy:length:descriptor:deallocator:` imports retain the
caller's backing and deallocator until final native-reference retirement;
properly synchronized host changes are uploaded at queue consumption. Vulkan
images remain optimal-tiled allocations. This is an explicit upload path, not
GPU zero-copy. Apple-family reports remain unchanged; ASTC capability reporting
requires the enabled native feature and all 28 LDR format queries.

Heap/buffer-backed layouts, imported GPU writes/blits, HDR, PVRTC and general
compressed-format coverage are unsupported. Strict compilation, native method
encodings, source/component checks and independent static review pass. Actual
ASTC upload/sampling, host-update pixels, wallpaper and performance remain device
acceptance checks.

Kernel patch 0203 retires a released IOSurface view's obsolete pixel owner
while retaining its independently mapped state page and genuine peer, port and
framebuffer owners. The wired cap is unchanged. The recorded early Vulkan error
is not attributed to this defect without further evidence.

Patch 0204 separates bounded allocation-stage diagnostics and records the first
four terminal JM results per client after existing completion/cache handling.
Only `MMK_JOB_COMPLETE` from the hardware completion path counts as successful
execution; cancellation, fault or timeout records do not. A successful job alone
does not establish correct pixels or identify the spinner's frame producer.
The Vulkan/Darwin transport also distinguishes allocation, host mapping and
buffer binding errors without changing allocation policy.

Patch 0205 follows the inspected MT6789 vendor restart loop: repeat the
SWRST command every 5 ms without returning. It preserves MODE, LENGTH and the
independent watchdog heartbeat. Compilation, patch composition and instruction
checks pass. The 00:26:36 run subsequently reached this retry and returned to
Android B around 00:39. Recovery from other hangs remains unvalidated.

The 00:26:36 run on 2026-10-26 used kernel `40224078640` (`1c09edc`)
and userspace `3e97131`. The 141,245-byte console records ten successful native
JM terminal results, including fragment jobs, after hardware completion polling
and cache handling. Their captured JS/MMU/GPU status fields are zero. This is
phone evidence of GPU job execution; it does not establish correct pixels,
numerical FP16, frame rate or complete Metal/Vulkan support.

The next renderer failure is now precise: backboardd reaches the adapter's
128-live-BO limit before native allocation, refusing requests of 64, 69,632 and
135,168 bytes with ENOSPC. This is not an observed device-memory allocation
failure. Released surface owners now show falling wired-byte snapshots; no
surface NoMemory failure is retained in this console. The later bounded suballocation
implementation preserves the kernel memory limits.

In that October 26 00:26:36 capture, PosterBoard still trapped while
initializing/publishing a configuration with nil paths. Both helper
registrations had been accepted, but its provider query was denied by the real
host-entitlement policy: zero results are not evidence of absence. Native
PosterBoard initially discovered nine providers. The helper's standalone legacy
registration replaced two valid records without their capability metadata;
PosterBoard rejected them and lost its defaults. That custom boot registration
job was subsequently disabled; original providers and user data are preserved.

The console reaches the 600-second reset and the new vendor retry loop, followed
by Android B return around 00:39. The user still reported only the bootloader image;
a complete rendered UI and wallpaper remain unconfirmed.

The current adapter packs small logical BOs into genuine native backing
slabs. On this 16 KiB CPU-page target, each slab contains 64 cells (1 MiB);
eligible requests up to 16 cells (256 KiB) use a contiguous slice. Larger and
ALLOC_ON_FAULT requests stay direct. EXEC permissions and exclusive VM ownership
remain separate. Mesa identities are unique local IDs; every native allocation,
mapping, sync and submission uses the real backing handle and checked offset.

The all-resident submit list still contains at most 128 native backings, and the
kernel's per-object and total-byte limits are unchanged. Rounded reused slices
are zeroed and synchronized before publication. Cleanup precedes recycling;
lost or uncertain native ownership is quarantined. Empty slabs are released.
This addresses the observed count amplification without assuming unlimited
memory or complete per-submit resource tracking. Fragmentation, many large
allocations and real kernel-budget exhaustion can still fail. Diagnostics count
logical objects and known owned backings separately; Vulkan logical heap usage
is not an exact measure of wired memory. The subsequent October 26 01:29:49
capture recorded packing and continued execution; broader memory behavior and
correct pixels remain unverified.

The custom poster helper now checks the stock query-authorization method
before discovery interpretation or any registration/prewarm action. Its installed
binary has no poster-host entitlement; denial is reported as failure, never as
proof that providers are missing. No entitlement is added. Its managed job is
diagnostic-only: helper-only arguments, RunAtLoad=false and KeepAlive=false.
Native PosterBoard discovery, readiness and SpringBoard prewarm remain intact.

The captured legacy registration path first unregisters a provider, then passes
a flat installation dictionary where a plugin-identifier map is expected. The
replacement can report success while losing genuine signing/capability metadata.
Stopping that custom boot action preserves the stock registration path. Removed
provider stores were retained in DeletedExtensions, with a verified native
restore path. The November 6 run restored all nine providers and selected native
configuration delivery to SpringBoard. Correct wallpaper and durable state
remain unverified. No cache wipe or custom store is used; user data and original
capability metadata are preserved.

## Priorities

1. Qualify per-job AS reuse and optional measured GPU timing on the new kernel,
   while investigating correct pixels separately. Record actual failures and
   distinguish loading, rendering and scanout.
2. Resolve shader/API coverage required by the observed system calls; then extend
   graphics features and application compatibility.
3. Restore services as their keybag, storage and hardware dependencies become
   available. Wi-Fi, Bluetooth, NFC, cellular and audio need separate bring-up.

No NFC, payment, biometric, modem or wireless functionality follows merely from
a controller identification or a preserved daemon definition.

Detailed contracts: [wiki home](wiki/Home.md), [display](wiki/Display-Pipeline.md),
[GPU](wiki/MT6789-GPU-Power-Sequence.md), [Metal](wiki/MaliMetal-Driver.md),
[input](wiki/Input-and-HID.md), [services](wiki/System-Services-and-Posters.md).
