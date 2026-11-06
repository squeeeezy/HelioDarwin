# MT6789 Mali-G57 power, clocks, and kernel transport

The Infinix X676C has a Mali-G57 MC2, Valhall v9 **Job Manager (JM)** GPU. It does not use the newer CSF submission interface.

## Current status — 2026-10-26

**GPU power-on is confirmed on the phone.** XNU has verified the VGPU and
VSRAM_GPU enable bits, observed the MFG power-domain status and read Mali
identification registers. Power-on is separate from rendering validation.

| Stage | Observed status |
| --- | --- |
| GPU power-on and register access | Confirmed on physical X676C boots. |
| Native Vulkan device creation | Succeeded after the IOKit and cache-address fixes. |
| GPU job completion | Ten successful JM terminal records, including fragment work, in run `40224078640`. |
| Correct rendered frames and performance | Remain unvalidated. |
| Full DVFS and dynamic power-off | Remain unfinished. |

XNU diagnostic boots have read `GPU_ID = 0x90930010`, `AS_PRESENT = 0xff`, and `JS_PRESENT = 0x7`. Android identifies the device as Mali-G57 r0p1 and reports shader/core mask `0x5`: two cores at bits 0 and 2.

The GPU memory and submission transport compiles to arm64 XNU objects; **hardware JM completion is confirmed on the phone**, including fragment work. Display output is covered separately in [Display Pipeline](https://github.com/squeeeezy/HelioDarwin/wiki/Display-Pipeline); its working framebuffer path does not establish Metal or Vulkan rendering.

## Evidence and scope

The power/register definitions were checked against the shipped `mtk_gpufreq_mt6789.ko`, `mali_kbase_mt6789.ko`, `mtk-pmic-wrap.ko`, `mt6358-regulator.ko`, and MediaTek clock drivers. Descriptor layouts use the pinned Mesa v9 XML. Vendor implementation details and offline compilation are evidence about the source; they do not validate the new XNU path under GPU load.

The current implementation preserves the existing SPM/MFG sequence. It uses split-SWINF PWRAP transactions, checks regulator-enable readback in the source, and implements GPU address spaces and JM submission. Run `39972968304` verified both rail-enable readbacks, with inherited selector values `0x38`/`0x38`. The 2026-10-26 capture subsequently records successful hardware JM completion.

## Register bases

| Block | Physical base | Purpose |
| :-- | :-- | :-- |
| Mali-G57 | `0x13000000` | GPU control, Job Manager, and MMU registers; kernel maps 16 KiB |
| SPM | `0x10006000` | MFG power controls and power-status registers |
| infracfg_ao | `0x10001000` | MFG bus protection |
| topckgen | `0x10000000` | MFG clock mux and update registers |
| apmixedsys | `0x1000c000` | MFGPLL |
| MFG_TOP / mfgcfg | `0x13fbf000` | External clock controls and bg3d gate |
| PWRAP | `0x10026000` | MT6366 PMIC access through the MT6789 SWINF interface |

For the broader board layout, see [Hardware Architecture](https://github.com/squeeeezy/HelioDarwin/wiki/Hardware-Architecture).

## MT6366 PMIC access: split SWINF

The shipped MT6789 PWRAP descriptor selects the split command/data register interface through capability bit 5. The older packed WACS command, halfword-shifted PMIC addresses, and FSM bits `[18:16]` do not apply to this interface.

| PWRAP offset | Register | Use |
| :-- | :-- | :-- |
| `+0x880` | CMD | Read: raw PMIC register address. Write: raw address OR `BIT(29)` |
| `+0x884` | WDATA | 16-bit write value, written before CMD |
| `+0x894` | RDATA | 16-bit read result |
| `+0x8a4` | VLDCLR | Write `1` to acknowledge a read result |
| `+0x8a8` | STATUS | FSM is `(STATUS >> 1) & 7`; idle `0`, read-valid `6` |

The shared `sweet_pwrap_read` / `sweet_pwrap_write` helper serializes complete transactions with one mutex and uses bounded polling. It preserves firmware-owned bus initialization. Both GPU rail setup and PMIC key polling use this helper.

### Regulator registers

These MT6366 rail definitions were checked in the phone's shipped MT6358-family regulator driver (`mt6358-regulator.ko`):

| Rail | Enable register / mask | Voltage-selector register / mask |
| :-- | :-- | :-- |
| VGPU | `0x1508`, bit `0` | `0x1526`, bits `[6:0]` |
| VSRAM_GPU | `0x1bc8`, bit `0` | `0x1bf4`, bits `[6:0]` |

The GPU factory follows the vendor power-control order: enable VGPU, then VSRAM_GPU. It changes only the enable bit, checks transaction errors, verifies enable readback, and reports the inherited selector values. It **preserves firmware voltages**; it does not assume or force 0.800 V.

Successful rail readback is required before the driver advertises JM submission capability. The integrated boot passed those enable/readback transactions. Selector readback alone is not a voltage or DVFS measurement.

## SPM MTCMOS sequence

The vendor AP-controlled path enables the rails, brings up MFG0 and MFG1, releases MFG bus protection, enables the reference clock, then brings up MFG2/MFG3 and the remaining GPU clocks. GPUEB was reported off on the inspected Android configuration.

The XNU factory retains its existing sequence, including preparing the reference-clock mux before the MFG domain sequence. Register access has been observed with this setup; full vendor DVFS and dynamic power-off have not been implemented.

| Domain | SPM PWR_CON offset | Status mask |
| :-- | :-- | :-- |
| MFG0, async/bus | `+0x308` | `0x4` |
| MFG1, core | `+0x30c` | `0x8` |
| MFG2, shader stack 0 | `+0x310` | `0x10` |
| MFG3, shader stack 1 | `+0x314` | `0x20` |

Power status is read from both `SPM + 0x16c` and `SPM + 0x170`. PWR_CON fields are `RST_B` bit 0, `ISO` bit 1, `PWR_ON` bit 2, `PWR_ON_2ND` bit 3, `CLK_DIS` bit 4, `SRAM_PDN` bit 8, and `SRAM_PDN_ACK` bit 12.

For each domain, the source sets both power-on bits, polls both status registers, clears clock-disable/isolation, releases reset, clears SRAM power-down, and polls its acknowledgement. After MFG1, bus protection is released through infracfg_ao writes:

| Offset | Value |
| :-- | :-- |
| `+0x718` | `0x80` |
| `+0x2a4` | `0x600000` |
| `+0x718` | `0x60` |
| `+0x2ac` | `0x200000` |

## Clock controls and remaining DVFS work

The mfgcfg bg3d gate uses status `+0x0`, clear `+0x8`, set `+0x4`, bit 0. Writing bit 0 to clear ungates it. The retained external-clock sequence updates MFG_TOP offsets `0x20`, `0xb0`, `0xb4`, `0x24`, and `0xf60`.

The MFG mux uses topckgen CLK_CFG at `+0x50`, set/clear `+0x54/+0x58`, and CLK_CFG_UPDATE at `+0x04`. The source ungates the existing selection and polls its status; it does not establish a validated operating frequency. Exact parent selection and a complete frequency-transition policy remain open work.

MFGPLL control locations include apmixedsys `+0x268` and power control `+0x274`. A complete PLL enable/program/lock sequence and coordinated voltage/frequency transitions have not been validated for XNU. Android OPP tables are not evidence that XNU is running at those frequencies or voltages.

The experimental timestamp path validates its native counter separately from
MFG clock policy. It does not measure the MFG operating frequency or establish
DVFS support. Patch 0208 provides the native clock interface used by that path;
hardware calibration/query publication remains unqualified.

## GPU memory and JM transport

The versioned IOKit ABI keeps MMIO programming in the kernel. Its source implements:

- Owned BO handles, allocation, task-memory pinning for supported imports, and release. A CPU virtual address is never treated as a GPU virtual address.
- Per-client GPU virtual address spaces and four-level AArch64 page tables with **4 KiB GPU pages**. A present/free physical hardware AS slot is leased per validated job instead of reserved by an idle connection. XNU CPU VM pages remain **16 KiB**; allocations and task imports obey that CPU-page granularity.
- Kernel-assigned GPU addresses, reused free VA ranges, guard pages, and explicit CPU/GPU cache ownership.
- Normal non-cacheable CPU BO mappings through write-combine memory attributes. These are ordinary RAM mappings suitable for buffer accesses, rather than Device-memory mappings. The initial Mesa adapter does not advertise host-cached memory; cached task imports are a separate kernel ABI facility.
- One active hardware submission globally. The kernel programs address-space registers and `JS_HEAD_NEXT`, affinity/configuration, and `JS_COMMAND_NEXT = START`.
- A workloop timer that polls actual JM/MMU/GPU status independently of user waits. Caller wait expiry returns pending; a five-second job watchdog attempts recovery.
- Fault/status capture and GPU cache cleaning before completion becomes CPU-visible. If recovery fails, the device is marked unavailable and affected pinned BOs/page tables are retained so potentially active DMA cannot access freed memory.

The register layout is GPU_CONTROL at `0x0000`, JOB_CONTROL at `0x1000`, and MMU at `0x2000`. Job slot *n* starts at `0x1800 + n * 0x80`; address space *n* starts at `0x2400 + n * 0x40`. `AS_PRESENT = 0xff` and `JS_PRESENT = 0x7` are availability bitmaps, not address-space/job counts encoded as integers.

Patch 0209 preserves each client's task, BO handles, GPU virtual address space
and page tables while leasing a real present/free hardware AS slot only for a
validated job under the global GPU lock. Opening an idle connection no longer
reserves a hardware slot. Completion releases the lease only after cache handling
and confirmed MMU flush/detach; a confirmed reset can also release it. Failed
recovery retains the lease and potentially DMA-visible owners. Global one-job
execution and existing memory limits are unchanged. Runtime AS reuse and detach
cost remain unqualified; this source change does not establish the cause of
previous connection failures or corrupt pixels.

### v9 descriptor correction

A v9 hardware job header is **32 bytes**, with job allocations aligned to **128 bytes**. It contains exception status, first-incomplete-task status, a 64-bit fault pointer, packed type/flags/index, two 16-bit dependencies, and a 64-bit next pointer. The fragment payload starts at byte 32. A 64-byte-header assumption or older-generation job-type numbering produces invalid descriptors.

The exact layouts are recorded in [MaliValhallHw.h](https://github.com/squeeeezy/HelioDarwin/blob/x676c/MaliMetal/MaliValhallHw.h), cross-checked against [Mesa v9 XML at the pinned revision](https://gitlab.freedesktop.org/mesa/mesa/-/blob/5a07217f034b3e50d8c7c7794f97a2df1742613b/src/panfrost/genxml/v9.xml). The public transport is [MaliMetalKernelABI.h](https://github.com/squeeeezy/HelioDarwin/blob/x676c/MaliMetal/MaliMetalKernelABI.h); the kernel implementation is carried by [patch 0189](https://github.com/squeeeezy/HelioDarwin/blob/x676c/xnu/patches/0189-x676c-malimetal-jm-gpu-userclient.patch).

## Validation boundary

Observed on the device: readable GPU identification and availability registers, and the Android GPU/core-mask identity above. The preserved MFG bring-up reached those reads.

Checked offline: vendor register/protocol definitions, Mesa descriptor layouts, arm64 kernel compilation, and composition of the GPU, shared PWRAP, display, and input patches. Kernel CI `42175315331` passed. Hardware qualification of per-job AS reuse, MMU detach behavior, GPU timestamp calibration/query publication and timing uncertainty remains pending.

The integrated boot validated the revised SWINF enable/readback path and native PanVK loading. Later fixes passed the CPU cache-type query, IOKit connection discovery and GPU-buffer cache-mode setup; run `40090751567` created the native Vulkan device. Run `40224078640` then records ten hardware JM completions on native GPU mappings. Still unvalidated: fault recovery, isolation, numerical correctness, correct rendered pixels and sustained performance. No Metal or Vulkan conformance is claimed. See [MaliMetal Driver](https://github.com/squeeeezy/HelioDarwin/wiki/MaliMetal-Driver) for the userspace implementation and its supported-path limits.
