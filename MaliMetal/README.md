# MaliMetal

Experimental Metal implementation for iOS 16.7.16 (20H392) on the MT6789
Mali-G57 MC2, using a Vulkan backend and an XNU Job Manager transport.

## Implementation status

The source builds for arm64 iOS. End-to-end GPU rendering with this driver has
not been validated on the phone, and Metal or Vulkan conformance is not claimed.

The 00:26:36 run on 2026-10-26 used kernel `40224078640` (`1c09edc`)
and userspace `3e97131`. The 141,245-byte console records ten successful native
JM terminal results, including fragment jobs, after hardware completion polling
and cache handling. Their captured JS/MMU/GPU status fields are zero. This is
phone evidence of GPU job execution; it does not establish correct pixels,
numerical FP16, frame rate or complete Metal/Vulkan support.

That capture identified a renderer failure: backboardd reached the adapter's
128-live-BO limit before native allocation, refusing requests of 64, 69,632 and
135,168 bytes with ENOSPC. This is not an observed device-memory allocation
failure. Released surface owners now show falling wired-byte snapshots; no
surface NoMemory failure is retained in this console. A bounded suballocation
remedy is now implemented, with kernel memory limits preserved.

PosterBoard still traps while initializing/publishing a configuration with nil
paths. Both helper registrations are accepted, but its provider query is denied
by the real host-entitlement policy: zero results are not evidence of absence.
Native PosterBoard initially discovers nine providers. The helper's standalone
legacy registration replaces two valid records without their capability
metadata; PosterBoard rejects them and loses its defaults. The custom repair
job has been disabled; original providers and user data must be preserved.

The console reaches the 600-second reset and the new vendor retry loop, followed
by Android B return around 00:39. The user still reported only the bootloader image;
a complete rendered UI and wallpaper remain unconfirmed.

The subsequent [01:29:49 capture](../logs/boot-0205-run-40224078640-ui-bo-poster.log)
used the same kernel with native BO suballocation and the retired registration
job. It records 128 logical BOs on 20 native backings, ten successful hardware
JM terminal results and no retained BO-allocation failure. PosterBoard still
traps: disabling the helper did not repair the two existing malformed provider
records. The user saw the bootloader image and black screens, without clocks or
wallpaper. Android returned automatically around 01:42; visible UI remains unverified.

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
is not an exact measure of wired memory. The 01:29:49 capture confirms packing
and continued hardware execution for that workload; broader memory behavior
and correct rendered pixels remain unverified.

The integrated render/compiler interface is ABI 4; compute dispatch is ABI 2.
The current bundle passed strict arm64 iOS linking, source/component checks and
independent static review. Runtime GPU, shared-event and source-compiler checks
remain separate acceptance gates.

The 2026-10-25 phone boot created the native device with Vulkan 1.3,
buffer device addresses, dynamic rendering and color tile reads enabled.
It then exposed a missing Metal texture-copy selector in the blit encoder;
backboardd aborted before that copy reached Vulkan. The run also ended in a
kernel panic in diagnostic stack sampling. Enabled features establish device
configuration, not successful GPU command execution.

The later 01:01:09 boot passed the former texture-copy selector and stopped at
`fixed_frag_lpf_cpf`: the phone library contains the corresponding half-precision
variants, while PanVK's inherited architecture policy reported FP16 unavailable.
The optional PanVK build flag `--qualify-g57-fp16` prepares an experimental
qualification ICD for exactly `GPU_ID=0x90930010` on arch 9. It is off by default,
leaves other devices' policy unchanged, and does not establish native numeric
FP16 correctness. The enabled profile must be captured from the candidate itself.

The 17:39:02 trial captured FP16 enabled in the actual native profile and reached
the internal render-draw ownership check, where `MaliMetalRenderDraw` lacked a
device getter. Draws now expose their retained program's actual device and
validate the pipeline/program/device identity. The former shader-name rejection
did not recur. Native pipeline preparation precedes this ownership check;
successful command execution, pixel correctness and numerical FP16 remain unverified.

The 18:14:02 trial did not repeat that draw-owner abort and reported matching
native display context geometry. SpringBoard then aborted while constructing
`MPSImageGaussianBlur`: the system MPS factory rejects devices supporting none
of Apple families 2–8. The experimental `--mps-common` bundle option registers a
genuine native `MPSDevice_Common` for the real device, using pinned MPSCore
exports, object layout and cache guards. It is off by default and leaves Apple
family reports unchanged. Published native objects retain their device for the
native process-lifetime cache; partial construction failures are quarantined.
Native registration/readback has succeeded in backboardd and SpringBoard.
Gaussian dispatch and pixels remain unverified.

When enabled, the builder requires the matching MPSCore and libc++ libraries in
the supplied sysroot and links them strongly. Compute descriptor threadgroup
width promises are accepted without enabling dependent optimization. The native
pipeline getter reports the queried execution width (16 on this G57), separately
from the translator's preferred logical width. Unsupported compilation controls
remain rejected. Default-off bundles do not acquire the MPSCore/libc++ dependency.

The 21:10:55 run reached native MPS Common registration in SpringBoard and
the real `D8F18H` compute pipeline request. The native compute descriptor lazily
creates a nonnil, empty stage-input descriptor; rejecting its mere presence
aborted pipeline construction. The frontend now accepts the verified empty
public defaults and rejects configured stage input. Other unsupported controls
report their individual reason. Both observed D8F18H specializations pass offline
compilation and SPIR-V validation; GPU dispatch, pixels and performance still
require a new device run.

The later 23:17:49 capture did not repeat the D8F18H rejection. It reached
a native buffer error with VkResult -2 and exposed the missing texture-layout
selector in SpringBoard/PosterBoard. The old shared error label does not
distinguish allocation from mapping or binding. Hardware job-completion
logging and image correctness remain separate acceptance gates.

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

The current compiler series also fixes sRGB tile-read metadata and forwards the
actual canonical color-write table. PointSize output is accepted only for the
triangle-list/triangle-strip fill path with native feature checks; this does not add a
point-drawing API. The existing 1304-request offline corpus now has 511 compile
and structural model-draw successes. Remaining guarded XR/format and texture
paths are not considered implemented by those results.

- **Kernel transport:** versioned IOKit calls for pinned buffers, GPU address
  spaces, cache maintenance, Job Manager submission, completion and fault
  recovery. Register definitions and product errata were checked against the
  phone's vendor modules and Mesa v9 descriptions. The job header is 32 bytes
  with 128-byte job alignment. Userspace does not receive raw GPU MMIO.
- **Metal objects:** resources, queues and recorded operations call the Vulkan
  backend. Resources remain retained through completion. Failed or unsupported
  operations report their actual backend errors. Indexed drawing and compute
  dispatch use native allocations and exact binding plans.
- **Shader compiler:** an optional, separately built native AIR compiler parses
  Metal libraries and produces SPIR-V. The actual QuartzCore vertex/clear shader
  pair, selected textured draws and APL/dimming compute kernels were compiled
  and their SPIR-V validated offline. Typed resource bindings,
  vertex layouts and push constants come from compiler metadata. This evidence
  does not establish support for every QuartzCore shader or arbitrary MSL.
- **Vulkan backend:** buffer/image operations, command recording, render pipelines
  and specialized compute pipelines are implemented. The pinned native PanVK
  port builds as an arm64 iOS ICD with strict symbol resolution. Its execution
  now has recorded hardware JM completion; correct pixels and broader shader coverage remain unvalidated. Device creation
  fails when the required ICD or capabilities are unavailable.
- **Presentation:** IOSurface access preserves ownership, locking and row stride.
  Sampled shared LA08 surfaces have a bounded two-channel RG8 staging import.
  Nonindexed triangle strips pass through canonical draw preparation and native
  topology variants. Both paths build locally; their displayed pixels remain
  unverified on the phone.
  The observed phone clock/interface uses the system's software rendering
  fallback and the corrected display path. It is not evidence of MaliMetal GPU
  execution.

`MaliMetalVulkan.c` contains Metal-to-Vulkan enum conversion.

Run `40083985422` passed `util_cache_granularity` after rebuilding startup
assembly with the configured cache-query permission. `vkCreateDevice` then
returned `VK_ERROR_OUT_OF_HOST_MEMORY`: the logical-device path duplicated the
transport's deliberately invalid Unix descriptor instead of using IOKit.
The current source opens and owns a separate IOKit client for each logical
device, preserves the caller's query connection, and reports connection
initialization failures separately from host allocations. Run `40090751567`
created the Vulkan device after the kernel connection-discovery and physical
address fixes, then stopped at Metal's device-protocol assertion.

The frontend now declares both `MTLDevice` and `MTLDeviceSPI` on its own class.
Its `initLimits` uses native Vulkan limits and implemented frontend bounds;
the target Metal ivar encoding is checked before accessing its storage.
Metal's normal registration checks remain active. The driver update passed
those checks and reached CoreAnimation's command-queue setup, then crashed
when a label accessor read offset zero as an object pointer. The standalone
`NSObject` declaration now reserves its `isa` word; label access uses the
runtime's atomic copy property accessors at offset eight. This object-layout
fix passed command-queue setup in the following phone boot, which reached
CoreAnimation shader loading. QuartzCore then called the absent
`newFunctionWithDescriptor:error:` selector. The library now accepts the target
function descriptor's name, constants and specialized name through its existing
AIR-function path. Unsupported descriptor options, archives, private linking
and deferred constants return errors. The following phone boot passed this
descriptor path and stopped at render-pipeline validation: CoreAnimation
requested multiple color attachments and source-over blending.

The source now carries up to eight color slots, within the native device limit,
through AIR compilation, the typed execution plan, Vulkan pipeline/render-pass
creation and command encoding. Each attachment retains its own format, write
mask and supported blend state. The failing CoreAnimation shader pair compiled
offline and passed SPIR-V validation with the new path. This uses an offline
Vulkan baseline profile, not a captured phone profile. The MRT/blending update
and GPU job execution still need phone validation.

The MRT candidate passed the earlier attachment-count guard on the phone, then
the AIR compiler rejected the native Vulkan profile's format-property structure.
The serializer now nests queried flags under `VkFormatProperties`, matching the
compiler's Khronos reader. Queried flags and sample counts are preserved. The
schema correction passed the next phone attempt. That attempt exposed the
static binding adapter's refusal of Whole/signed buffer windows. The current
render path uses the compiler's canonical per-draw planner instead: logical
buffer lengths, offsets, views and sampler facts determine descriptor ranges,
push data, specialization and native pipeline variants.

Both observed pipeline requests compile and instantiate structural draws on
the captured enabled phone profile. The decoded corpus contains 1079 static
names and 1304 conditional requests; not every request is supported or relevant
to the same device configuration. Exact fetch shaders also pass SPIR-V checks
with an explicitly derived tile-enabled profile. That is not a runtime capture
of the new feature selection.

## Dependencies and build

Build a fresh iOS artifact from the current source. Generated bundles and native
dependency binaries are kept outside version control.

The cross-build needs Python with `ziglang`, LLVM `ld64.lld`, Vulkan headers from
the pinned Mesa checkout, and matching iOS system libraries. Supply `libSystem`
and its reexports, `libobjc`, Foundation, CoreFoundation, Metal, IOKit and IOSurface
under their normal sysroot paths. `usr/lib/libSystem.dylib` must contain or point
to `libSystem.B.dylib`. These proprietary inputs must be supplied separately.

The native Vulkan dependency is prepared by `Vulkan/prepare_mesa.py` and built
with `Vulkan/build_mesa.py`. The port includes the pinned v9 Job Manager work and
an XNU transport overlay; upstream Mesa's G57 support alone is insufficient.
The build records revisions, actual transport inputs, kernel ABI, strict-link
audit and license notices beside `libpanvk_sweetmali.dylib`.

For native AIR compilation, build the pinned external compiler using
`Shader/build_air.py`. That helper records the compiler revision, Rust inputs,
toolchain and artifact hashes. Its output preserves the compiler and dependency
license notices; the compiler retains its own license.

Rebuild the ABI 4 provider and ABI 2 compute consumer together. Prepare the pinned
compiler checkout with only the declared patches, in the order below; the helper
verifies that exact source state using a temporary Git index. It records the
baseline, effective tree and patch hashes, and rejects undeclared changes.

```powershell
python MaliMetal/Shader/build_air.py --compiler-source PINNED_COMPILER_CHECKOUT --compiler-patch MaliMetal/Shader/compiler-patches/0001-coherent-tile-image-color-fetch.patch --compiler-patch MaliMetal/Shader/compiler-patches/0002-static-init-boolean-fc-absorption.patch --compiler-patch MaliMetal/Shader/compiler-patches/0003-tile-image-srgb-fetch-contract.patch --shader-tile-image --out AIR_BUILD
python MaliMetal/build.py --sysroot IOS_SYSROOT --vulkan-headers MESA_INCLUDE --air-provider AIR_BUILD --vulkan-icd PATH_TO_IOS_ICD --wsl --linker /usr/bin/ld64.lld-18
```

The AIR helper documents the supported Rust versions and its explicit option
for the older toolchain that was locally compile-checked. `PATH_TO_IOS_ICD` must
have its matching `panvk-sweetmali.json` and `LICENSES` directory alongside it.
Omitting the ICD prevents Vulkan device execution.

Output defaults to `MaliMetal/out/MaliMetal.bundle` with a `build.json` manifest.
The builder verifies the arm64/iOS platform, source and dependency hashes, and
resolution of the driver's own symbols. System symbols link strictly against
the matching supplied iOS libraries. Limited Darwin declarations are used rather
than a complete iOS SDK; Objective-C selector compatibility still needs runtime
validation. The bundle includes dependency provenance and license notices.
The local encoder protocol declarations do not enumerate native method
requirements, so compilation alone cannot check Metal selector completeness.

## Coverage limits

| Path | Implemented source and current boundary |
| --- | --- |
| Render binding | Canonical direct/indexed draw planning, Whole/signed windows, actual view/sampler facts and specialized native pipeline caching. The basic frontend still has pipeline/input restrictions; native helper coverage alone does not remove them. |
| Graphics | Indexed MRT, effective blend/masks, native topology/raster/depth/stencil/MSAA/resolve helpers and dynamic rendering. Color tile fetch requires the patched compiler, actual advertised extension, enabled feature and selected API 1.3. Broader facade paths and runtime behavior remain unvalidated. |
| Resources | Queried buffers, heaps/aliasing, texture views, mip/array/cube/3D staging, conditional BDA and typed registry identities. Logical samplers are resolved per use. Shared/no-copy imports, compressed/multiplanar variants and managed storage remain limited or unavailable. |
| XR | Distinct logical formats, raw representations and CPU/IOSurface plane transfers. Canonical XR rendering, ordered shadows, plane copies and decoded mip filtering remain gated. |
| Compute | Canonical dispatch variants, writable descriptors, exact nonuniform regions, indirect clamp/dispatch and barriers. Actual-profile AIR/structural SPIR-V checks do not establish GPU results. |
| Events | Ordinary native timelines/fences and a shared-event bridge. Shared factories require the live IOSurface kernel readiness/right/mapping probe. Unknown retirement retains submitted owners while reporting the real logical error. |
| API | Async methods, specialized reflection and argument encoders. Resource argument consumers remain disabled until canonical root/resident table consumption is connected. |
| MSL source | A linked adapter to the genuine system Metal frontend, without an Apple GPU identity/plugin. Native loading, source output and service lifetime still require device validation; unsupported controls return errors. |

Resource allocation validates protection options; protected allocations remain
unsupported. Encoder debug groups retain real nesting state, and disabled
visibility mode is checked; nonzero visibility queries are not implemented.
GPU/kernel timing getters remain unavailable to profiling paths. Root-based
publication avoids duplicate readbacks of unchanged IOSurfaces, and Scheduled
handlers run outside the device mutex. These are source-level changes awaiting
assembled runtime verification.

No complete Apple GPU family is advertised. Device limits and selected Vulkan
features are checked separately from API/helper presence. Explicit unavailable
paths are not considered working features.

IOSurface interoperability currently uses CPU staging and readback with ownership
and stride checks, not zero-copy imported GPU memory. The kernel serializes GPU
jobs and uses bounded completion polling. These choices establish implementation
boundaries, not performance or conformance guarantees.

## Offline installation

Install explicitly into an offline mounted root image:

```sh
sudo python3 tools/sweet-root-tweaks.py MOUNTED_ROOT --malimetal-build MaliMetal/out --malimetal-only
```

The installer checks the source digest, binary and component hashes, iOS
platform and generated bundle metadata. `--malimetal-only` installs the verified
bundle without applying daemon or preference changes. Omitting
`--malimetal-build` leaves the installed driver untouched.
