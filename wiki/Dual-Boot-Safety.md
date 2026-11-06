# Dual-Boot Safety

The working setup keeps Android on slot B and uses slot A for a HelioDarwin boot attempt. The boot helper writes `boot_a` and A/B metadata in `misc`; the separate root-image flasher writes `iosroot`. These operations have different scopes and should not be confused.

## One attempt selects the next boot

`boot-xnu.ps1` reads the 32-byte boot-control block at offset 2048 in `misc`, prepares slot A with `tries=1` and `successful=0`, writes the image to `boot_a`, verifies both writes and requests a reboot. LK consumes the attempt when it selects slot A. On the next reset, exhausted, unsuccessful slot A permits fallback to the bootable Android slot B.

**A/B metadata is not a watchdog.** A hang does not make LK run again. If XNU stops progressing, a real reset is still required before slot fallback can occur. The current shim does not run the old timed display-pump recovery path, and the boot helper's ADB polling cannot reset a processor that no longer responds.

The fallback also depends on valid slot metadata and a bootable slot B. It does not restore overwritten partitions or replace backups.

## Opt-in diagnostic reset

With `sweetreboot=N`, where `1 <= N <= 1800`, the X676C kernel starts a diagnostic thread after UFS bring-up. It waits for that interval, calls filesystem sync, then writes `0x1209` to TOPRGU `SWRST` at `0x10007014`. The timer runs independently of process sampling, so `sweetsample=-` does not disable it.

`sweetreboot=420` returned the phone automatically to Android slot B during [run 39945002194](https://github.com/squeeeezy/HelioDarwin/actions/runs/39945002194) on 2026-10-23. An earlier diagnostic attempt, [run 39941252453](https://github.com/squeeeezy/HelioDarwin/actions/runs/39941252453), reached its sync/reset path but `PSCI SYSTEM_RESET` returned without resetting the device.

This is a tested software-reset path, not an unconditional hardware watchdog: it needs UFS startup, kernel scheduling and completion of sync. It cannot guarantee recovery from an earlier boot failure or a kernel-wide hang. Without the boot argument, no diagnostic reset deadline is armed.

## Inherited hardware watchdog

The platform can also enter XNU with TOPRGU already armed. Earlier source fed it
only during display pushes; display-off skipped that path. A subsequent early
return reported `Watchdog` in Android, with display-off in the preceding console
and no diagnostic-reset or panic marker. The service coupling is proven; the
precise inherited timeout and reset subtype were not captured in that boot.

Patch 0200 moves the existing restart write to a dedicated timer and workloop,
independent of display power and scanout. It preserves inherited MODE/LENGTH,
derives its cadence from that timeout and logs the initial registers and first
service gaps. It does not disable the watchdog or change the 600-second
diagnostic reset. Source/object/patch checks pass. The 19:51:55 run reached its intended reset
and returned automatically. The 23:17:49 capture records display-off and later
reaches the reset thread, but the user returned manually; physical recovery
is not established for that run.

Patch 0205 follows the inspected MT6789 vendor restart loop: repeat the
SWRST command every 5 ms without returning. It preserves MODE, LENGTH and the
independent watchdog heartbeat. Compilation, patch composition and instruction
checks pass. The 00:26:36 run subsequently reached this retry and returned to
Android B around 00:39. Recovery from other hangs remains unvalidated.

## Deployment helpers

```powershell
pwsh x676c/tools/boot-xnu.ps1 -Run ACTIONS_RUN_ID -Adb ADB_EXECUTABLE -Serial DEVICE_SERIAL
```

This helper obtains the kernel artifact, builds the ADT/payload/shim, writes `boot_a` and `misc`, reboots, waits for Android and saves the previous console record. It does **not** flash a new root filesystem. Its current default command line does not include `sweetreboot`. `-BootArgs` replaces the whole command line; it does not append arguments.

If Android's root executable is outside `PATH`, supply its path with `-Su` to
either PowerShell helper. The root-image flasher accepts the same setting as
`--su`. The default remains `su`; the helpers do not install or relocate it.

`x676c/tools/auto-deploy.ps1` requires an explicit `-Run`, waits for rooted Android and preserves the previous raw console. It replaces `iosroot` only when `-RootImage IMAGE` is supplied. The root flasher also requires an explicit image path, checks the HFS+ geometry, Android slot B, an unmounted destination and partition capacity, then verifies SHA-256 over the complete written image. Neither helper chooses an old build or local root image implicitly. Source and destination must still be reviewed before writing; verification cannot undo interruption during a write.

The host return window defaults to 20 minutes and can be changed with `-ReturnWaitMinutes`. This is an ADB wait, not a reset timer. A longer diagnostic can select `sweetreboot=600` to allow ten minutes after UFS startup; earlier 420- and 600-second diagnostics returned automatically, while the
23:17:49 run ended at manual return. With patch 0205, the later 00:26:36 and
01:29:49 runs returned automatically; recovery from other hangs remains unverified.

Capture [pstore](Pstore-Debugging-on-MediaTek.md) after Android returns and before starting another shim boot, which initializes a fresh console header.

## Source

- [Boot helper](https://github.com/squeeeezy/HelioDarwin/blob/x676c/x676c/tools/boot-xnu.ps1) and [boot-control editor](https://github.com/squeeeezy/HelioDarwin/blob/x676c/x676c/tools/bootctl_block.py)
- [Auto-deploy watcher](https://github.com/squeeeezy/HelioDarwin/blob/x676c/x676c/tools/auto-deploy.ps1) and [root-image flasher](https://github.com/squeeeezy/HelioDarwin/blob/x676c/x676c/tools/flash_rootfs.py)
- [Diagnostic reset patch](https://github.com/squeeeezy/HelioDarwin/blob/x676c/xnu/patches/0190-x676c-serialize-dsi-and-use-toprgu-reset.patch)
