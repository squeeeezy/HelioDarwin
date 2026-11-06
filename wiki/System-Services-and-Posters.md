# System Services and Posters

HelioDarwin includes reversible launchd policy and opt-in PosterBoard preparation for **iOS 16.7.16, build 20H392**. The native helpers compile to arm64 iOS artifacts with strict compiler warnings and source/binary hash checks. **Native loading and publisher activity have been observed; complete wallpaper delivery remains unvalidated.** A present launchd job does not establish that its service works.

## Service policy

The inspected root snapshot has **ten removed jobs**: `com.apple.CommCenter`, `com.apple.locationd`, `com.apple.wifid`, `com.apple.bluetoothd`, `com.apple.nfcd`, `com.apple.awdd`, `com.apple.analyticsd`, `com.apple.passd`, `com.apple.biometrickitd` and `com.apple.ScreenTimeAgent`.

Four other jobs are **present in that snapshot**: `com.apple.nanotimekitcompaniond`, `com.apple.fairplayd.H2`, `com.apple.ptpd` and `com.apple.searchd`. The current policy preserves these four instead of adding further disables. Presence is not runtime validation.

For the remaining disable set, the root-tweaks script now archives original job definitions before setting `Disabled`. It retains program arguments, Mach services and device-matching conditions. Previously deleted jobs are not recreated automatically: restoration needs a preserved definition or an explicit original launchd cache from the same iOS build.

From the repository root, inspect or restore an offline root with:

```sh
python3 tools/sweet_daemons.py MOUNTED_ROOT
sudo python3 tools/sweet_daemons.py MOUNTED_ROOT --restore LABEL
sudo python3 tools/sweet_daemons.py MOUNTED_ROOT --restore LABEL --source-cache ORIGINAL_LAUNCHD_CACHE
```

The first command is read-only. Restoration restores a definition; it does not supply missing hardware or software contracts. Other bring-up changes, including crash-panic policy, watchdog preferences, Setup/migration markers and audio workarounds, remain separate.

Activation preparation now merges missing build/migration metadata while preserving existing daemon fields and activation state. It does not force `ActivationState` to `Activated`. Audio preparation also rejects conflicting active and disabled VirtualAudio plug-in copies.

## PosterBoard preparation

An early active-wallpaper read can lazily create a switcher configuration before the first extension initialization. A later prewarm then reaches an assertion requiring that configuration to be absent. The readiness guard leaves initialization and assertions intact: it returns unavailable before initialization and calls the original getter afterward. Returning unavailable alone does not ensure eventual wallpaper delivery.

The custom poster helper now checks the stock query-authorization method
before discovery interpretation or any registration/prewarm action. Its installed
binary has no poster-host entitlement; denial is reported as failure, never as
proof that providers are missing. No entitlement is added. Its managed job is
diagnostic-only: helper-only arguments, RunAtLoad=false and KeepAlive=false.
Native PosterBoard discovery, readiness and SpringBoard prewarm remain intact.

### Scoped metadata recovery candidate

The optional `SweetRepairPosterMetadata` dependency repairs the two retained
provider records through the matching mobile `lsd` process and its existing
MobileInstallation permissions. It fetches complete native metadata for the
PaperBoard and WallpaperKit hosts, checks the two original plugin identities
and authoritative provider entitlements, and compares fresh LaunchServices
entitlement dictionaries before and after registration. Healthy records are
left alone. It uses the native identifier-keyed registration dictionary and
arms the native save timer; it does not erase the database or unregister first.

Build and install into an offline matching root:

```sh
python3 tools/posterboard/build_recovery.py --sysroot IOS_SYSROOT --out RECOVERY_BUILD
sudo python3 tools/sweet_daemons.py MOUNTED_ROOT --repair-poster-metadata RECOVERY_BUILD
```

The builder also accepts `--wsl --linker /usr/bin/ld64.lld-18` on Windows.
Installation pins the existing ad-hoc-signed `lsd` preimage and preserves its
entitlement blobs and rollback metadata. Runtime guards check the OS build,
loaded images and inspected native methods. One worker pass starts after the
main runloop is ready; it does not block the main queue. Its owned diagnostic
file is `/private/var/mobile/Library/Logs/SweetRepairPosterMetadata.log`.

The candidate builds locally. Native registration acceptance, persistence,
renewed provider discovery and wallpaper pixels still require a device run.

The 2026-11-06 run subsequently accepted both scoped registrations, read back
the original entitlement dictionaries, rediscovered all nine providers and moved
the retained stores back under `Extensions`. The native four-path update reached
three SpringBoard observers. The previous PosterBoard assertion loop is absent
from the retained console. Home-configuration/sandbox path errors, Aegir snapshot
failures and corrupt displayed pixels remain open; correct wallpaper and durable
state across another iOS boot are not established.
To remove the dependency, use
`sudo python3 tools/sweet_daemons.py MOUNTED_ROOT --remove-poster-metadata-repair`.
This restores a recorded executable preimage; it retains provider data and logs.

The captured legacy registration path first unregisters a provider, then passes
a flat installation dictionary where a plugin-identifier map is expected. The
replacement can report success while losing genuine signing/capability metadata.
Stopping that custom boot action preserves the stock registration path. Removed
provider stores remain in DeletedExtensions; stock discovery has a verified
restore path for them. The next run must establish valid provider identities,
restored coordinators, nonnil publisher paths and visible wallpaper. No cache
wipe or custom store is used; user data and original capability metadata are preserved.


Private API use is restricted by loaded framework UUIDs and runtime contract checks:

| Framework | Expected UUID for 20H392 |
| --- | --- |
| PosterBoard | `fbd32eac-fbbf-3f56-a0ba-5bd44d2e2725` |
| CoreServices | `8d42103d-bd59-308d-9d2c-2082b39738e7` |
| ExtensionFoundation | `f1100ce5-49b1-34bd-aafd-30cfb2466b47` |
| PosterBoardServices | `4fc96a43-1b54-348b-9b78-13485197e086` |

Build both artifacts into the same directory using [the builder](https://github.com/squeeeezy/HelioDarwin/blob/x676c/tools/posterboard/build.py), once normally and once with `--registration-helper`. It requires Zig's Clang and a matching iOS library sysroot. The Windows/WSL linker option is `--wsl --linker /usr/bin/ld64.lld-18`.

Installation into an offline root is explicit:

```sh
sudo python3 tools/sweet_daemons.py MOUNTED_ROOT --prepare-posters POSTER_BUILD
sudo python3 tools/sweet_daemons.py MOUNTED_ROOT --remove-poster-preparation
```

Preparation verifies the target build, current sources and arm64/iOS artifacts, preserves the original native PosterBoard job and scoped readiness environment, and installs the custom helper only for explicit diagnostics. Its managed job has no boot registration/prewarm flags and does not run automatically. The supported PosterBoard executable retains its normal dependency on the guard library and the matching framework. Provider metadata, coordinator restoration, native prewarm and visible wallpaper require the next device readback; complete wallpaper-gallery support is not established.

Dependency installation is restricted to pinned executable hashes and UUIDs for this root. Both originals are already ad-hoc signed. The installer appends a load command in verified unused header space, preserving existing dependency ordinals, code/data sections, XML/DER entitlements and signature metadata, then recomputes the existing CodeDirectory page hashes. It retains original bytes and filesystem metadata for rollback and refuses unrelated executable changes. Rollback restores the executable before removing its required component or managed job settings. Current SWEET signature/platform policy remains unchanged; full Apple AMFI validation is not claimed.

## Red status area and geometry

Binary analysis confirms that UIKit's fallback status-bar provider creates a system-red background when native provider selection fails. The split-provider matcher considers native width **and** notch/exclusion size adjusted by native/reference scale; 1080-pixel variants already exist. Width or `360×800` points alone does not establish the cause.

Native `1080×2400` remains the boot default (`sweetfb_native=1`), giving `360×800` points at scale 3. The new opt-in [native metadata adapter](https://github.com/squeeeezy/HelioDarwin/tree/x676c/tools/native-display) keeps that framebuffer configuration and the existing iPhone X artwork identity. The scanout and touch paths continue to use the full physical panel without crop offsets. The older `1125×2436` cropped canvas is not the resolution fix. See [Display Pipeline](Display-Pipeline.md) and [Input and HID](Input-and-HID.md).

SpringBoard publishes display and device contexts to UIKit applications. When loaded and activated, the adapter changes its shared factories to supply the native panel geometry:

| Metadata | Native profile |
| --- | --- |
| Physical pixels / UIKit points | `1080×2400` / `360×800`, scale 3 |
| Exclusion rectangle in portrait points | `(156, 0, 48, 36)` |
| Corner radius | 39 points, corresponding to 117 physical pixels |
| Portrait safe area, top/left/bottom/right | `47 / 0 / 34 / 0` points |
| Preserved phone metrics | Periphery top 32; home-affordance allowance 21 |

Cutout and corner dimensions come from the device's read-only Android display contract. The portrait top inset matches UIKit's stock generic `FixedSplit` height. Other orientation wrappers retain stock values and nil semantics; UIKit's existing orientation fallback remains responsible for their interpretation.

The adapter uses the ordinary non-Jindo `FixedSplit` renderer, which reads the actual native width, scale and exclusion rectangle. The installer selects that stock feature policy with the valid `Jindo = { Enabled = false }` definition; it preserves the original definition for rollback. This is appropriate to the existing iPhone X identity, whose SpringBoard Jindo device path requires different artwork subtypes. It does not replace status-bar rendering methods, paint over the diagnostic, change assertions, or advertise Dynamic Island support.

Installation checks build `20H392` and matching source/binary hashes. Runtime activation additionally checks exact framework UUIDs, method signatures, non-Jindo policy and the native display/edge contracts. Mismatches retain original behavior and produce diagnostics. The library logs measured display-context pixels, scale, bounds, exclusion rectangle and safe insets, plus the device-context corner-radius readback.

| Native adapter image | Required UUID |
| --- | --- |
| FrontBoard | `0abe70d4-6cc0-37f9-af9e-ca2fd42442de` |
| FrontBoardServices | `9df90f7d-2d79-316c-92e4-13ead6cf214d` |
| SpringBoardFoundation | `7949279b-dbc1-3949-b8ef-90e7ef34dd46` |
| UIKitCore | `1242978a-2c2c-3781-8d6c-9777edce2804` |
| UIKitServices | `b9aded5a-37c0-30d4-ac8a-28ddc2f5654c` |
| SpringBoardHome | `ee83267f-132c-35c6-aa2d-aea8eee035a3` |
| SpringBoard | `22034aba-9a9a-3b94-93a5-a0de02befd2e` |
| libsystem_featureflags | `3a160c9e-3488-3ffd-a194-e2a166fe1169` |

Build and explicitly prepare an offline root with:

```sh
python tools/native-display/build.py --sysroot IOS_SYSROOT --out NATIVE_BUILD
python3 tools/sweet_native_display.py MOUNTED_ROOT
sudo python3 tools/sweet_native_display.py MOUNTED_ROOT --install NATIVE_BUILD
sudo python3 tools/sweet_native_display.py MOUNTED_ROOT --remove
```

For Windows/WSL linking, add `--wsl --linker /usr/bin/ld64.lld-18` to the build command. Root mutations require a POSIX root shell. Inspection is read-only; installation adds a guarded dependency to the pinned SpringBoard executable, retains the ordinary adapter opt-in and feature definition, and removes only its own DYLD injection token. Rollback restores the exact executable before clearing managed settings and restoring the original Jindo definition. Unrelated job fields and environment entries, the dylib, logs and recovery data are preserved.

The artifact compiles for arm64 iOS with strict warnings. Earlier installation through DYLD environment injection did not establish activation: launchd removed the variable and dyld's AMFI policy query was unavailable. The guarded dependency installers pass offline installation/removal checks; runtime activation and the visible result remain unvalidated. Captured diagnostics also include SpringBoard and service failures while reading shared-cache Objective-C metadata; this common failure is under investigation. Startup loading, feature-table regeneration, context propagation to applications, actual provider selection, orientation handling and visible wallpaper still need assembled-boot evidence. Successful compilation is not a claim that the red status area or every UIKit layout issue is fixed.

The 17:39:02 trial confirmed native-display constructor installation. Its context
readback still had the stock 38/29-point donor insets and cutout, so the adapter
correctly rejected that result. The guard now recognizes the exact screen-type-4
donor selected by the native 360×800 bounds; this does not imply that the user
enabled Display Zoom. Required native outputs remain unchanged and still need
successful readback.

The poster helper's real query returned zero providers and entered the first
LaunchServices registration call. `lsd` was running. The manual capture ended
less than 125 seconds after helper creation, before even the earliest phase
timeout; no registration hang or missing entitlement was established. A longer
capture must show its actual return, timeout or sampled blocking stack before
changing that path. PosterBoard had not spawned in the retained launchd segment.

The 18:14:02 retry confirmed the requested context and corner radius with
`match=1`. SpringBoard then failed MPS GaussianBlur initialization, before a
complete visible UI was validated. Android reported Watchdog on the early
return. The separate hardware-watchdog service in patch 0200 addresses the
proven display-off service coupling; its runtime liveness is still pending.
The registration log remained at LaunchServices, and an actual timeout or
successful provider delivery has not been established.

The later full diagnostic reached the intended 600-second reset. Native MPS
Common construction/cache readback succeeded in backboardd, and no prior
SpringBoard abort appeared in that capture; Gaussian output remains unverified.
The helper timed out on its real ExtensionKit query. Sampled `lsd` was restoring
installation journals: InstalledContentLibrary requested UniqueDeviceID, then
MobileGestalt waited for Wi-Fi identity through an IONetworkController match.
The direct `/chosen` factory-address property was absent. Boot preparation now
supplies verified private Wi-Fi and Bluetooth metadata without creating a network
driver or changing identity-version policy. The resulting seeding/query/provider
delivery is a new runtime check, not yet a completed wallpaper milestone.

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

Separate failures remain outside this texture candidate: the inspected
VideoToolbox alignment reader expects a dictionary for
`DisplayPipePlaneBaseAlignment`. Patch 0207 replaces the scalar property with
the inspected linear-alignment dictionary and supplies the native stride schema;
device service recovery remains unverified. Other captured extension/daemon aborts still need their own
contract analysis. These services are not considered repaired by AMFI
membership or the texture-layout implementation.

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

## Remaining contracts

Persistent AppleKeyStore key wrapping/unwrapping, bag lifecycle and access control remain missing prerequisites for real keychain persistence. SEP-backed protection, biometric/NFC/secure-element interfaces, modem transport, Wi-Fi/Bluetooth controllers and the USB PTP function require separate implementations. Search, ScreenTime and pass services also retain storage, account and keybag dependencies. These changes do not establish blanket daemon or hardware functionality.

Source: [root tweaks](https://github.com/squeeeezy/HelioDarwin/blob/x676c/tools/sweet-root-tweaks.py), [service tools](https://github.com/squeeeezy/HelioDarwin/blob/x676c/tools/sweet_daemons.py), [PosterBoard helpers](https://github.com/squeeeezy/HelioDarwin/tree/x676c/tools/posterboard), [native display preparation](https://github.com/squeeeezy/HelioDarwin/blob/x676c/tools/sweet_native_display.py).
