# MaliMetal Driver

MaliMetal implements a bounded Metal-to-Vulkan path for iOS 16.7.16 (`20H392`)
on the X676C Mali-G57 MC2. The source and native dependencies compile for arm64
iOS. Selected real CoreAnimation render and compute shaders translate to
validated SPIR-V. **Hardware JM completion, including fragment work, is now confirmed on the phone.**
Correct rendered output remains unvalidated.
Full Metal or Vulkan conformance is not claimed.

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

The subsequent [01:29:49 capture](https://github.com/squeeeezy/HelioDarwin/blob/x676c/logs/boot-0205-run-40224078640-ui-bo-poster.log)
used the same kernel with native BO suballocation and the retired registration
job. It records 128 logical BOs on 20 native backings, ten successful hardware
JM terminal results and no retained BO-allocation failure. PosterBoard still
traps: disabling the helper did not repair the two existing malformed provider
records. The user saw the bootloader image and black screens, without clocks or
wallpaper. Android returned automatically around 01:42; visible UI remains unverified.

## First integrated device observation

Run `39972968304` loaded the native PanVK library and read Mali-G57 identity and
texture features. Backboardd then repeatedly received SIGILL while PanVK queried
`nonCoherentAtomSize`: `util_cache_granularity` reads `CTR_EL0`, but the kernel
left user access disabled. Patch 0193 configures the read-only hardware cache-type
query for SWEET; its configured X676C cluster contains only Cortex-A55 cores.
Run `39984534077` still assembled the old `SCTLR_EL1=0x3454593d`, while a fresh
local assembly of its archived source and headers produces `0x34d4d93d`.
Assembly now bypasses the restored compiler cache. CI and deployment check the
startup constant in the Mach-O before booting. The generic illegal-instruction
handler remains unchanged. These observations do not establish Vulkan device
creation, GPU memory mappings or GPU jobs.

Run `40083985422` passed the cache-register query and kept backboardd running.
It then failed at `vkCreateDevice`: the logical-device constructor used
`os_dupfd_cloexec(-1)`, although the XNU transport holds an IOKit connection.
The current port opens a separate client on the same accelerator with
[IOServiceOpen](https://developer.apple.com/documentation/iokit/1514515-ioserviceopen)
and closes that owned connection when the logical device is destroyed. The
physical-device query connection remains borrowed from its caller. Transport
initialization failures use `VK_ERROR_INITIALIZATION_FAILED`; allocation failures
retain their allocation error. The later boot with run `40090751567` created
the native device; that capture did not record GPU completion.

The first retry of that path reached IOKit service discovery and returned
`kIOReturnUnsupported` (`0xe00002c7`). The kernel user client inherited
`IOUserClient::getService`, which returns `NULL`. Patch 0195 returns the attached
GPU provider, completing the connection-discovery contract. The next boot passed
service discovery; no GPU-completion evidence was retained in that capture.

Run `40089627425` passed service discovery, then panicked during GPU-buffer
cache-mode setup. `pmap_sync_page_attributes_phys` shifted a 32-bit page number
before converting it to an address, dropping physical-address bits above 4 GiB.
Patch 0196 uses `ptoa_64` and `flush_dcache64`. The corrected local object uses
a 64-bit shift, and the following boot passed that cache-mode setup.

Run `40090751567` subsequently created the Vulkan device (`VkResult 0`), but
Metal aborted in `MTLAddDevice` because the frontend class did not declare
`MTLDevice`. That function also requires `MTLDeviceSPI`, then invokes
`initLimits` and `initFeatureQueries`. The current frontend declares both
protocols and provides its own native limits rather than selecting an Apple
GPU profile. The 264-byte limits layout and named field encoding are checked
against the loaded Metal runtime. A retry with that frontend passed the
registration checks and reached `CA::OGL::new_metal_context`, then crashed in
`MaliMetalObject setLabel:`. Its compiled accessor used offset zero because
the standalone `NSObject` declaration omitted `isa`; it tried to release the
packed object header. The declaration now reserves that word and synthesized
atomic copy accessors use offset eight. Derived frontend objects share the
corrected layout; ivars following the opaque `_MTLDevice` base still use runtime
offsets. The following phone boot passed that queue setup and reached shader
loading, where QuartzCore called the absent `newFunctionWithDescriptor:error:`.
The library now routes the target descriptor's name and constant values through
the existing AIR-function creation path and preserves a copied specialized name.
Unsupported options, binary archives, private linking, deferred constants and
other descriptor modes return errors. A phone boot with that update reached
render-pipeline validation and failed because CoreAnimation requested multiple
color attachments. The crash payload identifies a downsample/blur pipeline
using two BGRA8 targets and source-over blending.

The current source propagates those attachment formats, masks and blend states
through the compiler, typed plan, Vulkan render pass/pipeline and command encoder.
It preserves slot indices and checks dimensions, aliasing and pipeline/pass
compatibility. The exact failing shader pair compiled offline and passed SPIR-V
validation using a Vulkan baseline profile; that profile is not a phone capability
capture. The updated MRT path and GPU command execution remain unvalidated on
the phone.

The next phone boot passed the attachment-count guard but failed native-profile
parsing: format flags had been serialized without their `VkFormatProperties`
wrapper. The corrected serializer preserves the queried capabilities and sample
counts in the structure the Khronos reader expects. An isolated offline check
reproduced the exact rejection before the correction and passed profile parsing
afterward. The following phone attempt passed that schema check and exposed
the static adapter's refusal of Whole/signed buffer windows.

The current integrated candidate replaces static binding guesses with canonical
per-draw planning, including logical resource bounds, typed views, sampler facts,
pushes and actual pipeline variants. The render/provider ABI is 4 and compute
dispatch ABI is 2. Its strict native bundle and static review pass; the later
00:26:36 capture records hardware JM completion. Pixel correctness remains unverified.

The 2026-10-25 phone capture confirms selected Vulkan 1.3, buffer device
addresses, dynamic rendering and `VK_EXT_shader_tile_image` color reads.
backboardd then aborted on a missing Metal texture-copy selector before the
copy reached Vulkan. A separate kernel panic was traced to diagnostic stack
sampling. These observations establish the selected native configuration;
they do not establish successful GPU execution or complete encoder coverage.

The 01:01:09 retry then reached a different failure: QuartzCore requested absent
`fixed_frag_lpf_cpf` because native FP16 was reported unavailable. The real
library index is intact. A default-off `--qualify-g57-fp16` PanVK build option
limits experimental availability to exact GPU ID `0x90930010` on arch 9;
other targets retain the upstream policy. Native numerical FP16 and actual
frame output still require candidate validation, not a profile edit or name alias.

The 17:39:02 trial captured the enabled native FP16 feature. It then aborted on
an internal draw object's absent `device` method. That object is reached only
after native pipeline preparation succeeds; this is not evidence of command
execution. Draw ownership now exposes the real retained device and checks exact
pipeline/program identity, preserving command-buffer resource validation.

The 18:14:02 retry then reached SpringBoard's MPS GaussianBlur initialization.
The stock MPS factory returned NULL because all queried Apple families were
unsupported; the failure preceded kernel-library loading. The optional bundle
flag `--mps-common` now prepares an experimental native Common backend using the
exact MPSCore UUID, exports, native constructor/vtable, real device limits and
guarded acquire/release cache publication. Apple-family answers stay unchanged.
It defaults off; enabled bundles strongly link matching MPSCore and libc++.
Both build modes pass strict linking. Actual native construction, dispatch,
blur output and wrapper compatibility remain runtime qualification gates.

The compute descriptor path accepts the caller's threadgroup-width promise
without using that optimization; unsupported private controls still return errors.
The pipeline's native execution width comes from the queried subgroup width,
not the translator's fixed logical preference. Existing Gaussian asset requests
compile offline, which does not prove native execution or numerical output.

The declared compiler series now includes patch 0003 for sRGB tile-read metadata,
with actual color-table data forwarding and a triangle-list/triangle-strip fill PointSize
qualification. In the unchanged 1304-request corpus, 511 requests compile and
form structural model draws; existing successful stage bytes remain unchanged.
These models are not captured runtime bindings or GPU results. XR semantics
and remaining unsupported format/texture paths stay guarded.

## Execution layers

| Layer | Implementation |
| --- | --- |
| Metal frontend | Objective-C device, resources, queues, command buffers and encoders. |
| Shader translation | C library parsing/binding interfaces and a pinned Rust AIR compiler adapter. |
| Vulkan backend | Real Vulkan resources, immutable draw/dispatch descriptor snapshots and pipelines. |
| PanVK ICD | Pinned Mesa plus v9 Job Manager work and an XNU transport overlay, built as an iOS dylib. |
| XNU | Versioned IOKit BO, GPU-VM, synchronization, submission, completion and recovery calls. |
| Hardware | Mali-G57 MC2, Valhall v9 Job Manager; GPU ID `0x90930010` observed from XNU. |

## Device discovery and capabilities

The kernel accelerator publishes `MetalPluginName` and `MetalPluginClassName`.
Stock Metal loads `/System/Library/Extensions/MaliMetal.bundle` and requires the
device class to inherit from its private `_MTLDevice` base. Registration requires
a usable native Vulkan device, enabled backend capabilities and an AIR compiler
profile. Failure prevents device creation and preserves the underlying error.

Private feature-query initialization follows the inspected 20H392 binary.
No complete Apple GPU family is advertised. Limits and supported operations
reflect the implemented frontend and enabled native device profile; upstream
Vulkan support alone is insufficient to advertise a Metal feature.

## Resources and commands

Buffers and typed images use real Vulkan allocations. Resource modules include
mip/array/cube/3D views and transfers, queried heaps/aliasing and conditional BDA.
Shared buffers
and IOSurface-backed textures preserve host ownership, locking and row stride;
Private resources use native storage and do not expose CPU contents. IOSurface
interoperability currently uses staging/copy/readback, not imported zero-copy GPU
memory. Allocations and resources remain owned through actual fence completion.

Per-queue serial submission preserves command order. Render commands include
supported triangle draws and indexed draws with native index buffers. Compute
programs retain the compiled module/layout and create exact specialization
variants from compiler-produced dispatch plans. Nonuniform grids are represented
by exact interior/edge regions, rather than rounded-up extra invocations.
Unsupported operations produce explicit errors.

Color attachment count is bounded to eight and the native device limit, using
the queried format paths. Independent blending is enabled only
when reported by the native Vulkan device. The backend consumes the compiler's
effective attachment states, which can differ from the input states when it
removes unwritten outputs. The render compiler/provider interface is ABI 4,
with compute ABI 2; older providers are rejected during the bundle build.
Native graphics helpers include depth/stencil/MSAA/resolve and dynamic rendering,
but existing frontend restrictions and runtime gates still apply.

## AIR and native shader compilation

The parser reads precompiled Metal libraries, function metadata and constants.
The native compiler emits SPIR-V and typed plans for argument ranges, vertex
layouts, constants and dispatch specialization. A genuine system MSL frontend
adapter is linked and validates returned MTLB/AIR; native execution and broader
source-option coverage remain validation gates. Rust is confined to the AIR
compiler adapter; the frontend,
Vulkan backend and kernel transport use Objective-C/C/C++.

Offline checks used actual CoreAnimation clear, upsample, a selected specialized
textured pipeline, and APL/dimming compute kernels. Those checks establish the
tested input/translation paths, not every shader variant in QuartzCore or every
future application pipeline. Compute resource support is currently bounded to
verified direct buffers, constants and read-only sampled-image forms.

## Kernel and PanVK boundary

The ICD uses XNU IOKit calls instead of Linux DRM or Android kbase ioctls. The
initial port does not advertise dma-buf/external-memory import or host-cached BO
memory. CPU allocations use Normal non-cacheable mappings; CPU pages are 16 KiB
and GPU pages are 4 KiB. Kernel-created GPU addresses are separate from CPU
virtual addresses.

One hardware job runs globally at a time. An independent workloop timer polls
JM/MMU/GPU status; a bounded job watchdog attempts reset and captures failures.
Failed recovery retains potentially DMA-visible pinned storage. Tiler heaps use
committed bounded allocations; grow-on-fault paging is not implemented. See
[GPU Power Sequence](MT6789-GPU-Power-Sequence.md) for rails, clocks, register
layout and remaining DVFS work.

Patch 0198 implements the target IOSurface shared-event kernel protocol:
kernel-owned timelines, Mach rights, read-only task mappings, waits and async
notifications. Local object compilation and patch composition pass. A private
readiness handshake separates this implementation from older unsupported
selectors. Runtime rights, mapping, notification and GPU-boundary visibility
validation are still required before shared-event support is considered working.

## Build and provenance

Build the pinned dependencies separately and provide matching iOS libraries;
proprietary inputs and generated binaries are not included in Git. The main
builder verifies iOS platform, source and dependency hashes and strict linking.
It packages dependency provenance and license notices. The offline installer
verifies the current source digest and copied files before replacing the bundle.
See [build instructions](https://github.com/squeeeezy/HelioDarwin/blob/x676c/MaliMetal/README.md).

Pinned source inputs:

- [Mesa 5a07217f](https://gitlab.freedesktop.org/mesa/mesa/-/tree/5a07217f034b3e50d8c7c7794f97a2df1742613b)
- [v9 PanVK work fd6ed91e](https://github.com/zenithblue-oss/panvk-kbase-android/tree/fd6ed91eff1c8d3a83307a2348b959bb93797011)
- [AIR compiler a2c440b8](https://github.com/steelbrain/metal2vulkan/tree/a2c440b8bcdcf20d34aa1694b61ae22c2a7ea68b)

The native build preserves upstream license notices. Prior related work includes
[Darling](https://github.com/darlinghq/darling) and
[Indium](https://github.com/darlinghq/indium); their existence does not establish
runtime compatibility of this implementation.

The 21:10:55 run reached native MPS Common registration in SpringBoard and
the real `D8F18H` compute pipeline request. The native compute descriptor lazily
creates a nonnil, empty stage-input descriptor; rejecting its mere presence
aborted pipeline construction. The frontend now accepts the verified empty
public defaults and rejects configured stage input. Other unsupported controls
report their individual reason. Both observed D8F18H specializations pass offline
compilation and SPIR-V validation; GPU dispatch, pixels and performance still
require a new device run.

The 23:17:49 run did not repeat the D8F18H rejection. Native memory
creation and a missing texture-layout API are now observed blockers.
Successful hardware completion was not separately logged; buffer swaps
alone do not establish that a particular frame was GPU-rendered.

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

## Remaining coverage and validation

The 2026-11-06 source candidate adds bounded sampled/shared LA08 IOSurface
import and canonical nonindexed triangle strips, including the filled-triangle
PointSize check. Strict arm64 iOS linking and two existing captured-request
structural shader checks pass; device pixels remain unverified.

Kernel patch 0206 preserves panel-memory source aliases by composing into one
private 10,368,000-byte buffer before publishing a complete frame to the panel
mapping. Existing source bounds, cache cleaning and DSI push ordering remain.
This adds storage and a full-frame CPU copy; it is a correctness correction,
not a performance result. Patch 0207 publishes the two native VideoToolbox
linear-alignment dictionary schemas with the existing 64-byte requirement.
Their hardware and service behavior remains to be checked on the phone.

Patched coherent color tile reads, native depth/stencil/MSAA helpers, views and
compute write/indirect paths are now integrated source, with device validation
pending. Full frontend coverage is not implied: constant-blend and pipeline
restrictions remain, resource argument/root consumers are disabled, and XR
render/shadow semantics and broader formats/advanced APIs need further work.
Shared-event runtime transport and the system MSL frontend are unverified.

Offline source/patch composition and compilation are separate from the next
device checks: stock Metal loading, GPU power/VM access, native pipeline creation,
real JM completion, correct image/readback and UI presentation. The clock/interface
observed after the DSI fix used system software rendering and does not prove this
driver's GPU execution.
