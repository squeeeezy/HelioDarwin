# Input and HID

X676C input support has been implemented and compiled locally to ARM64 objects.
Run `40083985422` recorded physical touch press/release events in the kernel HID
path. The subsequent retry also recorded power, volume-up and volume-down
press/release events. The user confirmed touch interaction; display sleep/wake
still behaves incorrectly. **Multi-touch, volume behavior and complete gesture
handling remain unvalidated.**
Hardware identity and wiring below are established by Android's active driver
binding, input metadata, device tree and vendor driver behavior.

The first integrated boot reached the hardware startup thread but stopped
with `error=5` during clock setup. The shipped gate descriptor orders its fields
as status/clear/set: IMPW uses `0xe00`/`0xe04`/`0xe08`. Patch 0193 changes the
I2C0 enable write to CLR at `0xe04` and reports gate/reset failures separately.
Run `39984534077` subsequently read the expected `54:52` chip ID and entered
polling. Report parsing still returned `error=5` with completed I2C status `0x1`.
Patch 0194 records the first three report prefixes and DMA buffer address to
distinguish returned data from transport errors. Physical contact delivery
was subsequently observed at the kernel HID layer in run `40083985422`.
That boot also returned all-FF report prefixes and continued to reject them;
it does not establish complete controller recovery or gesture handling.

## Touchscreen

The actual controller is **FocalTech FT5452J**, chip ID `0x5452`, using Android module `ft3519_common`. The module filename does not identify an FT3519 controller. Android binds `mtk-tpd` at I2C device `0-0038`; merely loading a Goodix module does not make Goodix the active controller.

| Connection / limit | X676C value |
| --- | --- |
| Controller bus | I2C0, base `0x11e00000`, address `0x38` |
| APDMA channel | `0x10217080` |
| IRQ | GPIO9 / EINT9, falling edge in Android |
| Reset | GPIO152 |
| Power enable | GPIO62 |
| I2C pin functions | SCL0 on GPIO130; SDA0 on GPIO131 |
| Contact limit | Five slots |
| Native coordinates | X `0..1079`, Y `0..2399` |

The MT6789 I2C controller uses slave-address register `0x94` and a 16-byte FIFO. The driver reads each 32-byte touch report in one combined pointer-write/read APDMA transaction, with a contiguous bounce buffer, cache synchronization and bounded error recovery. FT5452J reports start at register `1`, with six-byte contact records; they do not use the earlier Topaz coordinate scaling. Controller selection requires the expected chip ID. No touch firmware upload is performed by XNU.

Patch 0211 reads a report every 10 ms period and no longer samples GPIO9 to decide whether to read: Android arms that line as a falling-edge interrupt, so a level sample can miss a short pulse and with it a brief tap. The transfer now sleeps between status polls instead of spinning. This does not claim that the Android IRQ route has been installed as a native XNU interrupt handler.

Report decoding follows the shipped `fts_irq_handler` and `fts_input_report_b`: event flags 0 and 2 hold a slot down, 1 and 3 release it, a repeated id takes its later record, and out-of-range coordinates are clamped. A frame is still rejected for an unknown format, a count above five, an id from 5 to 9, or a down record with a zero count. `0xff` in bytes 1 to 4 is the controller's reset state: contacts are released and it is not counted as an error. The earlier decoder dropped the whole frame for flag 3, a repeated id or one out-of-range coordinate, which would freeze every other finger. No capture with two simultaneous contacts exists yet, so multi-touch remains unvalidated.

## Physical buttons

| Button | Hardware source | iOS event |
| --- | --- | --- |
| Power / side | MT6366 PMIC `TOPSTATUS` register `0x28`, active-low bit 1 | Consumer Power keyboard event |
| Volume up | Keypad `MEM1` bit 0, active low | Consumer Volume Increment keyboard event |
| Volume down | Keypad `MEM1` bit 1, active low | Consumer Volume Decrement keyboard event |

Android exposes power through `mtk-pmic-keys`, with PMIC interrupt 48 for press and 50 for release. It exposes the volume buttons separately through `mtk-kpd`. XNU currently polls those hardware states every 20 ms. Matrix bit 8 is not used as a substitute power button.

`sweet_pwrap.c` provides serialized, bounded MT6789 SWINF reads/writes shared with GPU power setup. Input reads the power debounce register; it does not reprogram PMIC regulators, interrupt routing or long-press reset configuration. See [GPU Power Sequence](MT6789-GPU-Power-Sequence.md) for the shared bus and regulator details.

## Startup and iOS delivery

The active `rd=ufs` path now starts touch and keys from `bsd/dev/ufs/ufs_disk.c` after UFS bring-up. Previously those startup calls existed only on the older `rd=ufsroot0` path, so publishing HID services did not start the hardware readers. Each input initializer has a once guard; touch hardware access is delayed by five seconds by default.

`SweetHIDTouch` publishes a built-in, display-integrated digitizer service. Each hardware scan becomes one collection containing all active and lifted finger paths, with normalized coordinates and aggregate event state. `SweetHIDButtons` publishes the consumer-control service. Events use `IOHIDEventServiceUserClient` and its serialized event queue, following the [Apple IOHIDFamily collection contract](https://github.com/apple-oss-distributions/IOHIDFamily/blob/IOHIDFamily-1915.140.3/IOHIDFamily/IOHIDEventDriver.cpp).

State is retained until a HID client is ready, and incomplete event allocation is reported as an error. There is no synthetic unlock swipe or periodic Home-button generator. Coordinates must remain consistent with the [logical display profile](Display-Pipeline.md); a framebuffer or UI geometry change requires checking that mapping too.

## Controls and validation boundary

- `sweettouch=0` disables touch; `sweetkeys=0` disables button polling.
- `sweettouchdelay=N` changes the touch startup delay in seconds.
- `sweetfb_stayon=1` deliberately keeps the physical display on, so lack of panel blanking alone does not prove a missing power event.

The five affected translation units, including startup and shared PWRAP access,
compile to ARM64 objects. Chip identification and kernel HID press/release
observations establish part of the physical path. Multi-touch, controller
recovery, volume behavior and display sleep/wake still need validation beyond
the confirmed touch interaction and button-event delivery.

Source integration: [shared PWRAP patch 0191](https://github.com/squeeeezy/HelioDarwin/blob/x676c/xnu/patches/0191-x676c-shared-pwrap-swinf.patch) and [input patch 0192](https://github.com/squeeeezy/HelioDarwin/blob/x676c/xnu/patches/0192-x676c-real-touch-and-keys.patch).
