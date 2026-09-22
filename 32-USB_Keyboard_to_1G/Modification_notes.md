# TEC-1G USB Keyboard Interface
## Raspberry Pi Pico — Version 6 Modification Notes

This document describes the hardware modifications and firmware behaviour used by the Version 6 Raspberry Pi Pico USB keyboard interface for the TEC-1G.

The interface converts a standard USB HID keyboard into the TEC-1G keyboard matrix while preserving the behaviour expected by MON3.

## PlatformIO board selection

The same source can be built for a Raspberry Pi Pico, Pico W, or Pico 2 W by selecting the appropriate board in `platformio.ini`.

With the maxgerhardt Raspberry Pi platform and the Earle Philhower Arduino-Pico core, use:

| Board                 | `board =` setting |
|-----------------------|-------------------|
| Raspberry Pi Pico     | `pico`            |
| Raspberry Pi Pico W   | `rpipicow`        |
| Raspberry Pi Pico 2 W | `rpipico2w`       |

The common PlatformIO settings remain:

```ini
[env:tec1g]
platform = https://github.com/maxgerhardt/platform-raspberrypi.git
framework = arduino
board_build.core = earlephilhower

; Select ONE of the following board lines:
board = pico
; board = rpipicow
; board = rpipico2w

board_build.f_cpu = 120000000L

lib_deps =
    https://github.com/sekigon-gonnoc/Pico-PIO-USB.git

build_flags =
    -D USE_TINYUSB
    -D CFG_TUH_ENABLED=1
    -D CFG_TUH_RPI_PIO_USB=1
    -D PIN_USB_HOST_DP=0
```

### Important PIO-USB pin setting

The TEC-1G interface uses:

- GP0 = USB D+
- GP1 = USB D-

Therefore the build flags **must** contain:

```ini
-D PIN_USB_HOST_DP=0
```

Earlier development versions used `PIN_USB_HOST_DP=2`; that setting is not correct for the final hardware wiring.

Only the `board =` line needs to be changed when selecting between Pico, Pico W and Pico 2 W. The PIO-USB D+/D- wiring remains GP0/GP1.

> **Pico 2 W note:** Pico 2 W uses the RP2350 rather than the RP2040. Use a current maxgerhardt PlatformIO platform and current Pico-PIO-USB library when building for it.

Version 6 includes:

- Fast direct matrix emulation.
- Two simultaneous matrix keys.
- Correct held-key behaviour.
- Special one-shot handling for Caps Lock.
- USB keyboard Caps LED feedback from the TEC.
- Ctrl+Alt+Delete “Vulcan Nerve Pinch” reset.
- Automatic Pico reboot as part of the VNP.
- Protection against USB-host lockups caused by some wireless keyboard receivers.
- Hardware watchdog recovery.

---

# 1. 74HCT245 Output Enable Modification

The Pico drives the TEC keyboard return lines through a 74HCT245 running from the TEC's +5 V supply.

The Pico's 3.3 V output levels are suitable for the TTL-level inputs of the 74HCT245.

## Data connections

Pico to 74HCT245:

| TEC Data | Pico GPIO |
|----------|----------:|
|    D0    |   GP16    |
|    D1    |   GP17    |
|    D2    |   GP18    |
|    D3    |   GP19    |
|    D4    |   GP20    |
|    D5    |   GP21    |
|    D6    |   GP22    |
|    D7    |   GP28    |

The HCT245 outputs connect to the TEC keyboard-return lines.

The TEC already contains its own bus buffer which connects these keyboard lines to the Z80 data bus only when required. Therefore the Pico-side 74HCT245 does **not** need to be switched on and off for each keyboard scan.

## /OE modification

Connect:

- 74HCT245 `/OE` → Pico GP5
- 10 kΩ pull-up from `/OE` to **Pico 3.3 V**

Do **not** pull `/OE` up to +5 V because GP5 is a 3.3 V Pico GPIO.

The external resistor ensures that the 74HCT245 remains disabled while the Pico is resetting or before its GPIOs have been configured.

Firmware startup sequence:

1. `/OE` is initially held HIGH by the 10 kΩ resistor.
2. D0-D7 are configured as outputs.
3. All D0-D7 outputs are driven HIGH, representing no key pressed.
4. GP5 is configured as an output.
5. GP5 is driven LOW.
6. The 74HCT245 remains enabled continuously during normal operation.

This avoids startup glitches and removes `/OE` switching from the timing-critical matrix loop.

---

# 2. TEC Address Bus and Matrix Handling

The TEC keyboard scanner exposes A8-A15 to the interface.

These are 5 V signals and must not be connected directly to the Pico.

A 74LVC245A powered from 3.3 V is used as the level translator.

## Address connections

| TEC Address | Pico GPIO |
|-------------|----------:|
|     A8      |    GP6    |
|     A9      |    GP7    |
|     A10     |    GP8    |
|     A11     |    GP9    |
|     A12     |    GP10   |
|     A13     |    GP11   |
|     A14     |    GP14   |
|     A15     |    GP15   |

The 74LVC245A accepts the TEC's 5 V logic levels and outputs safe 3.3 V logic to the Pico.

## Important MON3 timing discovery

MON3 performs one keyboard read for each matrix row.

During an `IN A,(C)` instruction the relevant A8-A15 address pattern is only available for approximately:

- 2.5-3 Z80 clock cycles
- approximately 600-750 ns at 4 MHz
- roughly 75-90 Pico CPU cycles at 120 MHz

There is no second attempt at the same row during that scan pass.

Therefore the Pico cannot afford GPIO helper functions, loops, row decoding and multiple output operations inside the time-critical section.

## Version 6 matrix method

Core 0 is dedicated entirely to the keyboard matrix.

The matrix loop:

- runs from RAM rather than XIP flash;
- contains its own permanent `for (;;)` loop;
- never returns to the Arduino framework;
- performs one aggregate GPIO read of A8-A15;
- reacts only when the upper address byte changes;
- compares the address directly against two precomputed active-key patterns;
- performs at most one SIO GPIO toggle operation.

The loop deliberately avoids returning to the Arduino framework because the Earle Philhower core normally performs housekeeping and `yield()` after each `loop()` call.

Those delays were found to cause missed matrix rows.

A missed row could look to MON3 like:

```text
key pressed
key missing for one scan
key pressed again
```

which produced apparent key bounce and extremely fast repeat.

Keeping Core 0 permanently inside the matrix polling loop eliminated this behaviour.

## Physical-switch rule

For ordinary keys:

> A USB key that is physically held down is represented as a continuously closed TEC matrix switch.

The Pico does **not** generate its own typematic repeat.

MON3 remains responsible for:

- debounce;
- normal key repeat;
- modifier handling;
- deciding how two simultaneously pressed matrix keys should be interpreted.

The Pico tracks up to two simultaneous matrix positions.

This allows combinations such as:

- Shift + letter
- Ctrl + key
- Function combinations

to appear to MON3 just as they would on the original physical keyboard.

---

# 3. Ctrl+Alt+Delete — “Vulcan Nerve Pinch” Reset

Ctrl+Alt+Delete is handled specially.

It does not pass through the TEC keyboard matrix.

Both left and right Ctrl and Alt keys are accepted.

The USB Delete key is HID code `0x4C`.

When:

```text
Ctrl + Alt + Delete
```

is detected, the Pico performs a hardware reset of the TEC followed by a reboot of itself.

## Reset output

Pico:

- GP27 → external N-channel MOSFET or NPN open-collector stage
- external stage → TEC/Z80 `/RESET`

The TEC reset input is active LOW.

The transistor stage is used because the Pico should **pull the reset line LOW**, but should not actively drive the TEC reset line HIGH.

## Recommended N-channel MOSFET arrangement

A small logic-level N-channel MOSFET can be used.

Typical connection:

```text
Pico GP27
    |
    +---- Gate

MOSFET Drain ---- TEC /RESET
MOSFET Source --- GND
```

Pico ground and TEC ground must be common.

A gate pull-down resistor is recommended so the transistor remains OFF while the Pico is starting.

For example:

```text
Gate ---- 10 kΩ ---- GND
```

A small series resistor between GP27 and the MOSFET gate may also be fitted if desired.

The exact MOSFET is not critical because the reset signal carries essentially no current. A small logic-level device suitable for operation from a 3.3 V gate is sufficient.

## Logic

GP27 LOW:

```text
MOSFET OFF
TEC /RESET released
```

GP27 HIGH:

```text
MOSFET ON
TEC /RESET pulled LOW
```

## Version 6 VNP sequence

When Ctrl+Alt+Delete is first detected:

1. Clear any currently active virtual matrix keys.
2. Drive GP27 HIGH.
3. The MOSFET pulls TEC `/RESET` LOW.
4. Hold TEC reset for 100 ms.
5. Drive GP27 LOW.
6. TEC `/RESET` is released.
7. Wait approximately another 50 ms.
8. Reboot the Pico using the Pico hardware watchdog reboot mechanism.

Therefore the complete sequence is:

```text
Ctrl+Alt+Delete
        |
        v
TEC /RESET asserted
        |
      100 ms
        |
        v
TEC /RESET released
        |
       50 ms
        |
        v
   Pico reboots
```

The VNP is edge-triggered.

Holding Ctrl+Alt+Delete down does not continuously reset the machine.

---

# 4. Caps Lock Matrix Behaviour

Caps Lock required special treatment.

Originally Caps was handled like an ordinary matrix key:

> USB Caps held = TEC Caps switch continuously closed.

This caused MON3 to see multiple Caps operations during one physical press.

The symptom was:

- apparent key bounce;
- multiple Caps toggles;
- Caps repeating when held.

## Version 6 Caps behaviour

Caps is now converted into a short one-shot matrix pulse.

On the transition:

```text
USB Caps UP
      to
USB Caps DOWN
```

the Pico closes the TEC Caps matrix position for approximately:

```text
20 ms
```

The matrix switch is then opened again even if the USB Caps key remains physically held.

Further HID reports containing Caps are ignored until the USB key has been released.

Therefore:

```text
Caps pressed
     |
     v
TEC Caps closed
     |
    20 ms
     |
     v
TEC Caps opened
     |
     v
wait for USB key release
```

Holding Caps for several seconds still produces only one TEC Caps event.

This eliminated the Caps bounce/repeat problem.

The pulse length is defined in firmware by:

```cpp
CAPS_MATRIX_PULSE_MS
```

Version 6 currently uses:

```text
20 ms
```

Ordinary keys are **not** treated this way. They still behave as continuously held physical matrix switches.

---

# 5. TEC Caps Status Feedback — GP13 Modification

Version 6 supports reading the actual Caps state from the TEC.

This is optional.

The Caps key itself works correctly without this modification.

The feedback is used to make the USB keyboard's Caps Lock LED reflect the actual state of the TEC.

This is preferable to having the Pico maintain its own guessed Caps state because MON3 remains the authority.

## TEC Caps signal

The TEC provides a Caps status signal which is approximately:

```text
0 V   = Caps off
+5 V  = Caps on
```

The Pico GPIO must not receive +5 V directly.

A resistor divider is therefore required.

## Voltage divider

Use:

```text
TEC Caps status
      |
     10 kΩ
      |
      +---------- GP13
      |
     20 kΩ
      |
     GND
```

This divides 5 V to approximately:

```text
5 × 20 / (10 + 20)
```

approximately:

```text
3.33 V
```

which is suitable for the Pico input.

## Connection summary

- TEC Caps status → 10 kΩ
- junction of 10 kΩ and 20 kΩ → GP13
- 20 kΩ → GND

Ensure Pico and TEC grounds are common.

## Firmware behaviour

GP13 is configured as an input.

Version 6 also enables a weak internal pull-down.

Therefore, if the Caps feedback bodge is **not fitted**, GP13 remains at a defined LOW level instead of floating.

If the divider is fitted later, the external TEC status signal overrides the weak internal pull-down.

When feedback is present:

```text
TEC Caps OFF
      |
      v
GP13 LOW
      |
      v
USB keyboard Caps LED OFF
```

and:

```text
TEC Caps ON
      |
      v
GP13 HIGH
      |
      v
USB keyboard Caps LED ON
```

The USB keyboard LED therefore displays the TEC's actual state rather than a locally generated state.

Importantly:

> The Caps matrix one-shot does not depend on GP13 feedback.

The keyboard remains usable whether or not the feedback modification has been fitted.

---

# 6. USB Keyboard Connection

PIO-USB host connections are:

| USB Signal | Pico |
|------------|-----:|
|     D+     | GP0  |
|     D-     | GP1  |

The firmware build uses:

```text
PIN_USB_HOST_DP=0
```

so GP1 is automatically used as D-.

The USB keyboard requires a suitable +5 V VBUS supply and a common ground.

Small series resistors in D+ and D- may be used for signal integrity if required.

---

# 7. Wireless Keyboard Receiver Findings

During testing, wired and wireless keyboards behaved differently.

## Wired keyboard

A normal wired USB keyboard proved very reliable.

It could:

- sit idle indefinitely;
- be unplugged;
- be reconnected;
- enumerate again normally.

No USB host lockup was observed from ordinary wired keyboard inactivity.

## Wireless receiver failure

The wireless keyboard receiver showed a repeatable failure.

If the keyboard:

- was switched off;
- went out of RF range; or
- otherwise lost contact with its receiver

for approximately two minutes or more, the Pico USB host could become permanently wedged.

Once this occurred:

- turning the keyboard back on did not recover it;
- unplugging and reconnecting the wireless receiver did not recover it;
- replacing the receiver with a wired keyboard did not recover USB operation;
- the wired keyboard still received power and performed its LED power-on sequence;
- however USB enumeration and HID traffic no longer occurred.

A Pico power cycle restored operation.

This demonstrated that the failure was not:

- the TEC matrix loop;
- the keyboard mapping;
- a stale held-key state;
- merely a disconnected receiver.

Instead, the wireless receiver was placing the Pico PIO-USB/TinyUSB host into a global non-responsive state.

---

# 8. Wireless USB Host Protection

Several attempted recovery methods were tested.

Simply retrying:

```text
tuh_hid_receive_report()
```

was insufficient.

Periodically aborting and recreating the HID interrupt-IN transfer was also insufficient.

The successful solution was to create periodic real USB control-endpoint traffic.

## USB health probe

Version 6 sends the keyboard LED state using a HID `SET_REPORT` approximately every:

```text
30 seconds
```

even when the Caps state has not changed.

This serves two purposes:

1. It prevents the wireless receiver/USB host connection from remaining completely dormant.
2. Completion of the control request proves that the USB host stack is still processing transactions.

The probe uses the same keyboard LED report used for Caps feedback.

With no feedback modification fitted, the LED report simply represents Caps OFF.

## Health timing

Current Version 6 values are approximately:

```text
USB health probe interval: 30 seconds
health transaction timeout: 2 seconds
hardware watchdog timeout: 5 seconds
```

If the USB control transaction does not complete within the expected period, Core 1 deliberately stops feeding the hardware watchdog.

---

# 9. Pico Hardware Watchdog

The Pico hardware watchdog is enabled as an additional recovery mechanism.

It is deliberately serviced from **Core 1**, where the USB host stack runs.

This is important.

Core 0 contains the permanent matrix polling loop and could continue running even if the USB system were completely dead.

If Core 0 fed the watchdog, a USB failure could therefore remain undetected forever.

Instead:

```text
Core 1
  |
  +---- USBHost.task()
  |
  +---- USB health handling
  |
  +---- watchdog_update()
```

If `USBHost.task()` hangs completely, Core 1 can no longer reach `watchdog_update()`.

The hardware watchdog then resets the Pico automatically.

Likewise, if a USB health probe begins but fails to complete, watchdog feeding is stopped.

This means a future USB-host failure should recover automatically instead of requiring a power cycle.

## Wireless keyboard test result

With the health probe and watchdog in place:

- the wireless keyboard was left inactive/off for approximately 10 minutes;
- it successfully resumed when switched back on;
- disconnecting and reconnecting the wireless receiver also worked normally.

This configuration became the Version 6 baseline.

---

# 10. Core Allocation

## Core 0

Dedicated exclusively to the timing-critical TEC matrix.

It performs:

```text
read A8-A15
    |
    v
detect address change
    |
    v
compare active matrix keys
    |
    v
update D0-D7
```

The loop runs continuously from RAM and never returns to the Arduino framework.

## Core 1

Handles slower asynchronous work:

- TinyUSB / PIO-USB host processing;
- HID reports;
- keyboard mapping;
- Caps one-shot timing;
- Caps LED feedback;
- Ctrl+Alt+Delete handling;
- TEC reset timing;
- USB health probes;
- hardware watchdog servicing.

This separation prevents USB activity from interfering with the sub-microsecond TEC matrix response.

---

# 11. Version 6 Pin Summary

| GPIO | Function                   |
|-----:|----------------------------|
| GP0  | PIO USB D+                 |
| GP1  | PIO USB D-                 |
| GP5  | 74HCT245 `/OE`             |
| GP6  | A8                         |
| GP7  | A9                         |
| GP8  | A10                        |
| GP9  | A11                        |
| GP10 | A12                        |
| GP11 | A13                        |
| GP13 | Optional TEC Caps feedback |
| GP14 | A14                        |
| GP15 | A15                        |
| GP16 | D0                         |
| GP17 | D1                         |
| GP18 | D2                         |
| GP19 | D3                         |
| GP20 | D4                         |
| GP21 | D5                         |
| GP22 | D6                         |
| GP27 | TEC reset MOSFET control   |
| GP28 | D7                         |

---

# 12. Current Version 6 Behaviour Summary

Version 6 should now behave as follows:

## Ordinary key

```text
key down
   |
   v
TEC matrix switch closed
   |
   v
remains closed while key held
   |
   v
key released
   |
   v
matrix switch opened
```

## Caps Lock

```text
Caps down
   |
   v
TEC Caps matrix switch closed for 20 ms
   |
   v
switch opened
   |
   v
ignore until physical Caps release
```

## Caps feedback

If the GP13 divider is fitted:

```text
TEC Caps state
    |
    v
   GP13
    |
    v
USB Caps LED
```

## Ctrl+Alt+Delete

```text
Ctrl + Alt + Delete
        |
        v
TEC reset for 100 ms
        |
        v
reset released
        |
      50 ms
        |
        v
Pico reboot
```

## Wireless receiver protection

```text
every 30 seconds
      |
      v
HID LED SET_REPORT
      |
      +---- success ---> continue
      |
      +---- failure ---> watchdog not fed
                          |
                          v
                     Pico resets
```

---

# 13. Recommended Final Hardware Additions

For the completed board:

1. Fit the 10 kΩ `/OE` pull-up from GP5 to Pico 3.3 V.
2. Use the 74LVC245A for safe A8-A15 level conversion.
3. Use the 74HCT245 for Pico-to-TEC D0-D7 conversion.
4. Fit the GP27 open-drain MOSFET reset stage.
5. Fit the optional GP13 Caps feedback divider:
   - 10 kΩ from TEC Caps status to GP13.
   - 20 kΩ from GP13 to ground.
6. Maintain a common Pico/TEC ground.
7. Keep Version 6 as the known-good firmware baseline.

The GP13 feedback modification may be omitted initially without affecting keyboard operation. It can be added later as a board bodge or incorporated into a future PCB revision.
