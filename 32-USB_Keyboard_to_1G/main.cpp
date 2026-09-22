// =============================================================================
// TEC-1G Matrix Keyboard Interface — Raspberry Pi Pico (RP2040) — Version 6
// =============================================================================
//
// Hardware:
//   - Raspberry Pi Pico / Pico W (RP2040) running at 120MHz (PIO-USB)
//   - USB keyboard connected via GP0 (D+) / GP1 (D-)
//   - TEC A8-A15 -> 74LVC245A @ 3.3V -> Pico address inputs
//     (LVC inputs are 5V-tolerant; outputs are 3.3V logic)
//   - Pico data outputs -> 74HCT245 @ 5V -> TEC keyboard D0-D7 inputs
//     (HCT TTL thresholds accept the Pico's 3.3V HIGH level)
//   - 74HCT245 /OE on GP5, with external 10k pull-up to Pico 3V3.
//     /OE is disabled during startup, then enabled once and left enabled.
//   - Optional TEC Caps status (+5V when active) -> 10k/20k divider -> GP13
//   - GP27 -> open-drain/open-collector stage -> TEC/Z80 /RESET
//
// platformio.ini:
//   platform = https://github.com/maxgerhardt/platform-raspberrypi.git
//   board = rpipicow
//   framework = arduino
//   board_build.core = earlephilhower
//   board_build.f_cpu = 120000000L
//   build_flags =
//       -D USE_TINYUSB
//       -D CFG_TUH_ENABLED=1
//       -D CFG_TUH_RPI_PIO_USB=1
//       -D PIN_USB_HOST_DP=0
//   monitor_speed = 115200
//
// Pin Allocation:
//   GP0        USB D+  (PIO-USB host)
//   GP1        USB D-  (PIO-USB host, must be D+ + 1)
//   GP6-GP11   Inputs  A8-A13  (from Mon3 address bus, via level shifter)
//   GP14-GP15  Inputs  A14-A15 (from Mon3 address bus, via level shifter)
//   GP13       Input   TEC Caps status (5V via 10k/20k resistor divider)
//   GP16-GP22  Outputs D0-D6   (to TEC matrix data inputs via 74HCT245)
//   GP27       Output  Z80 reset control (HIGH asserts via MOSFET/transistor)
//   GP28       Output  D7      (to TEC matrix data input via 74HCT245)
//
// =============================================================================
// VERSION 6 FEATURE SUMMARY
// =============================================================================
//   - Ordinary USB keys behave as held physical switches in the TEC matrix.
//   - Caps Lock is edge-triggered: each USB press produces one 20 ms virtual
//     matrix closure, preventing MON3 from repeatedly toggling while held.
//   - GP13 is enabled as an OPTIONAL TEC Caps-status input. A weak internal
//     pull-down keeps the input defined on boards where the status bodge is not
//     fitted. If the 10k/20k divider is added, the TEC state drives GP13 and
//     the USB keyboard Caps LED mirrors the real TEC Caps state.
//   - Every 30 s, a HID SET_REPORT is used as a harmless USB-host health probe.
//     If it cannot complete within 2 s, Core 1 stops feeding the RP2040 watchdog.
//   - The watchdog also recovers if USBHost.task() itself becomes stuck.
//   - Ctrl+Alt+Delete (VNP) resets the TEC, releases /RESET after 100 ms, then
//     reboots the Pico 50 ms later for a clean restart of the complete interface.
//   - The timing-critical Core 0 matrix loop remains the proven continuous
//     RAM-resident implementation and is not modified by the auxiliary logic.
// =============================================================================

// =============================================================================
// TIMING DESIGN NOTE
// =============================================================================
//   Mon3 executes "IN A,(C)" once per matrix row (8 rows, one rotating low
//   bit across A8-A15), with NO per-row retry: each row is sampled exactly
//   once per scan pass (verified from the MON3-1G ROM disassembly). Within
//   that instruction the address bus is valid only ~2.5-3 clock cycles
//   before the Z80 latches the data bus, so the REAL budget from "address
//   matches" to "data driven" is ~600-750ns -- roughly 75-90 Pico cycles
//   at 120MHz, NOT the ~1.5us originally assumed.
//
//   That is not enough room for: a per-pin gpio_get() loop (8 function
//   calls), array indexing, branching on row/col indices, and then more
//   function calls to set pin direction/state. Even the straightforward
//   mask-test version of this loop sits within ~100ns of the limit when
//   two key slots are active, and Mon3's single-sample-per-row scan turns
//   any late response into a dropped keypress for the ENTIRE pass. That
//   is exactly why two-key combos (Shift+M etc.) were unreliable: both
//   rows must be caught in the SAME pass, and marginal per-row timing
//   makes that rare for some row pairs (Shift+1 ~50%, Shift+M ~never).
//
//   The revised hot path (loop(), continuously on Core 0) freezes D0-D7 for
//   as long as the UPPER address byte (A8-A15) remains unchanged. When that
//   byte changes, it immediately snapshots the two published key slots and
//   computes the required matrix return value. Any upper-address value that
//   does not match an active key row produces 0xFF. This is deliberately
//   faster than first qualifying an exact one-low pattern, and importantly
//   prevents a late response from leaving the PREVIOUS ROW's key pattern on
//   the TEC keyboard-return lines. On an address transition it performs one
//   aggregate GPIO read, two slot compares, and at most ONE SIO gpio_togl
//   write to change the data byte. The code runs from RAM
//   (__not_in_flash_func) so USB-host activity on Core 1 cannot stall it via
//   shared XIP flash-cache evictions. The function contains its own infinite
//   polling loop and never returns to the Arduino framework, avoiding the
//   USE_TINYUSB yield() that would otherwise run between loop() calls.
//
//   All the "thinking" -- which key is pressed, which row/col that maps to,
//   converting that to GPIO bitmasks -- happens in processReport(), which
//   only runs when a USB HID report arrives (i.e. on key press/release).
//   That code has microseconds to spare and is never on the critical path.
// =============================================================================

#include "Adafruit_TinyUSB.h"
#include "pico/stdlib.h"
#include "hardware/structs/sio.h"
#include "hardware/watchdog.h"

// Run the hot loop from RAM: Core 1's USB-host stack continuously thrashes the
// RP2040's shared XIP flash cache, and a cache miss inside loop() can cost more
// than Mon3's entire ~600-750ns data-setup budget. __not_in_flash_func places
// the function in the .time_critical section (RAM-resident) under the pico-sdk;
// the fallback keeps this file buildable in environments that lack the macro.
#ifndef __not_in_flash_func
  #define __not_in_flash_func(funcname) funcname
#endif

// ─── USB Host ─────────────────────────────────────────────────────────────────
#ifndef PIN_USB_HOST_DP
  #define PIN_USB_HOST_DP 0   // D+ on GP0; D- will be GP1 automatically
#endif

Adafruit_USBH_Host USBHost;

// ─── Address Input Pins (A8-A15 from Mon3) ───────────────────────────────────
const uint8_t ADDR_PINS[8] = {6, 7, 8, 9, 10, 11, 14, 15};
//                             A8 A9 A10 A11 A12 A13 A14 A15

// ─── Data Output Pins (D0-D7 to Mon3) ────────────────────────────────────────
const uint8_t DATA_PINS[8] = {16, 17, 18, 19, 20, 21, 22, 28};
//                             D0  D1  D2  D3  D4  D5  D6  D7

// ─── 74HCT245 Output Enable ──────────────────────────────────────────────────
// /OE is active LOW. GP5 is used ONLY for safe startup sequencing:
//   - external 10k pull-up to Pico 3V3 keeps the HCT245 disabled during reset
//   - firmware initializes D0-D7 to HIGH (0xFF)
//   - firmware then drives /OE LOW once and leaves it LOW permanently
//
// The TEC has its own buffer which decides when these keyboard-return lines
// are connected to the Z80 data bus, so /OE does NOT belong in the hot path.
#define OE_PIN 5
#define OE_MASK (1u << OE_PIN)
#define OE_ENABLE()  (sio_hw->gpio_clr = OE_MASK)   // LOW  = enabled

// ─── TEC status / control pins ───────────────────────────────────────────────
#define CAPS_STATUS_PIN 13   // TEC +5V Caps status through 10k/20k divider
#define RESET_PIN       27   // HIGH turns on external pull-down transistor/MOSFET
#define RESET_PULSE_MS  100u

// GP13 is always enabled as an optional TEC Caps-status input. The firmware
// keeps a weak pull-down active, so an unmodified board reads a stable LOW.
// If the 10k/20k divider bodge is fitted, the external TEC signal overrides
// the weak pull-down and the USB keyboard Caps LED follows the real TEC state.

// A USB Caps press is converted to one short virtual matrix closure.
// This is intentionally adjustable without touching any other key handling.
#define CAPS_MATRIX_PULSE_MS 20u

#define RESET_ASSERT()  gpio_put(RESET_PIN, 1)
#define RESET_RELEASE() gpio_put(RESET_PIN, 0)

// ─── Precomputed GPIO bitmasks ────────────────────────────────────────────────
// Per-pin masks are built once in setup() from ADDR_PINS[] / DATA_PINS[] for
// the (non-hot) slot-management code. The two aggregate masks are COMPILE-TIME
// constants: the hot loop ANDs/compares them on every iteration, and as RAM
// globals they would cost an extra load each pass instead of folding into
// immediate operands.
uint32_t ADDR_PIN_MASK[8];   // bit mask for each address line, e.g. 1<<ADDR_PINS[i]
uint32_t DATA_PIN_MASK[8];   // bit mask for each data line

// All 8 data pins OR'd -- used to drive all latches HIGH before pulling one
// LOW, so the 74HCT245 sees 0xFF minus exactly one bit rather than all-zero.
// GP16,17,18,19,20,21,22,28 -- must match DATA_PINS[] above.
#define ALL_DATA_MASK 0x107F0000u

// All 8 address lines OR'd together (used to qualify a real scan -- see
// ADDRESS QUALIFICATION note below).
// GP6,7,8,9,10,11,14,15 -- must match ADDR_PINS[] above.
#define ALL_ADDR_MASK 0x0000CFC0u

static_assert(ALL_ADDR_MASK == ((1u<<6)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<10)|(1u<<11)|(1u<<14)|(1u<<15)),
              "ALL_ADDR_MASK does not match ADDR_PINS[]");
static_assert(ALL_DATA_MASK == ((1u<<16)|(1u<<17)|(1u<<18)|(1u<<19)|(1u<<20)|(1u<<21)|(1u<<22)|(1u<<28)),
              "ALL_DATA_MASK does not match DATA_PINS[]");

// ─── Key Map ──────────────────────────────────────────────────────────────────
// Row = address index 0=A8..7=A15 / Col = data index 0=D0..7=D7
// 0 in charMap means the cell is a special/modifier key (see specialMap).

const char charMap[8][8] = {
//   D0    D1    D2    D3    D4    D5    D6    D7
  {  0,    0,    0,    0,    0,    0,    0,    0   }, // A8  all special
  {  0,    0,    0,    0,    0,    ' ',  '\'', ',' }, // A9
  {  '-',  '.',  '/',  '0',  '1',  '2',  '3',  '4' }, // A10
  {  '5',  '6',  '7',  '8',  '9',  0,    ';',  0   }, // A11
  {  '=',  0,    0,    0,    'a',  'b',  'c',  'd' }, // A12
  {  'e',  'f',  'g',  'h',  'i',  'j',  'k',  'l' }, // A13
  {  'm',  'n',  'o',  'p',  'q',  'r',  's',  't' }, // A14
  {  'u',  'v',  'w',  'x',  'y',  'z',  0,    '\\'}  // A15
};

#define SK_NONE   0
#define SK_SHIFT  1
#define SK_CTRL   2
#define SK_FUNC   3
#define SK_UP     4
#define SK_DOWN   5
#define SK_LEFT   6
#define SK_RIGHT  7
#define SK_CAPS   8
#define SK_DEL    9
#define SK_TAB    10
#define SK_ENTER  11
#define SK_ESC    12

const uint8_t specialMap[8][8] = {
//   D0        D1        D2        D3        D4        D5        D6        D7
  { SK_SHIFT, SK_CTRL,  SK_FUNC,  SK_UP,    SK_DOWN,  SK_LEFT,  SK_RIGHT, SK_CAPS }, // A8
  { SK_DEL,   SK_TAB,   SK_ENTER, SK_NONE,  SK_ESC,   SK_NONE,  SK_NONE,  SK_NONE }, // A9
  { SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE }, // A10
  { SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE }, // A11
  { SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE }, // A12
  { SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE }, // A13
  { SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE }, // A14
  { SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE,  SK_NONE }  // A15
};

// USB HID keycodes for special keys (USB HID Usage Tables v1.12, Page 0x07)
#define HID_KEY_ENTER        0x28
#define HID_KEY_ESCAPE       0x29
#define HID_KEY_BACKSPACE    0x2A
#define HID_KEY_TAB          0x2B
#define HID_KEY_CAPS_LOCK    0x39
#define HID_KEY_F1           0x3A  // mapped to SK_FUNC
#define HID_KEY_DELETE       0x4C  // PC Delete key, used by Ctrl+Alt+Del VNP
#define HID_KEY_RIGHT_ARROW  0x4F
#define HID_KEY_LEFT_ARROW   0x50
#define HID_KEY_DOWN_ARROW   0x51
#define HID_KEY_UP_ARROW     0x52

#define HID_MOD_LEFT_CTRL    0x01
#define HID_MOD_LEFT_SHIFT   0x02
#define HID_MOD_LEFT_ALT     0x04
#define HID_MOD_RIGHT_CTRL   0x10
#define HID_MOD_RIGHT_SHIFT  0x20
#define HID_MOD_RIGHT_ALT    0x40

// ─── USB keyboard / auxiliary state (Core 1 only) ───────────────────────────
static uint8_t keyboardDevAddr  = 0;
static uint8_t keyboardInstance = 0;
static bool    keyboardMounted  = false;

// HID interrupt-IN receive state is derived from TinyUSB itself via
// tuh_hid_receive_ready(); do not maintain a parallel pending flag that can go stale.

// TinyUSB HID control transfers (SET_PROTOCOL / SET_REPORT) are asynchronous.
// In particular, do NOT repeatedly call tuh_hid_set_report() while EP0 is busy:
// some RP2040/TinyUSB versions can wedge the host control transfer state.
static bool    keyboardProtocolReady  = false;
static bool    capsReportBusy         = false;
static bool    capsLedSynced          = false;
static bool    lastCapsLedState       = false;
static bool    capsStateInFlight      = false;
static uint32_t nextCapsReportAttempt = 0;

// Must remain alive after tuh_hid_set_report() returns; TinyUSB may still be
// using this byte while the control transfer completes.
static uint8_t keyboardLedReport = 0;

static bool     vnpWasHeld        = false;
static bool     resetActive       = false;
static uint32_t resetReleaseAt    = 0;
static bool     picoRebootPending = false;
static uint32_t picoRebootAt      = 0;

// Caps is edge-triggered rather than held. These variables live only on Core 1.
static bool     capsUsbWasHeld      = false;
static bool     capsPulseActive     = false;
static uint32_t capsPulseReleaseAt  = 0;

// VNP reset sequencing: hold the TEC /RESET for 100 ms, release it, then give
// the TEC another 50 ms before rebooting the Pico itself.  Releasing GP27 first
// avoids relying on its state while the RP2040 is going through reset.
#define PICO_REBOOT_AFTER_TEC_MS  50u

// USB-host health monitoring. A harmless HID SET_REPORT (the current Caps LED
// state) is sent periodically even when Caps has not changed. Completion of that
// control transfer proves that TinyUSB/PIO-USB is still servicing the device.
#define USB_HEALTH_PROBE_MS    30000u
#define USB_HEALTH_TIMEOUT_MS   2000u
#define USB_WATCHDOG_MS         5000u

static uint32_t nextUsbHealthProbe   = 0;
static uint32_t usbHealthDeadline    = 0;
static bool     usbHealthProbeActive = false;
static bool     watchdogStarted      = false;

// =============================================================================
// HOT-PATH STATE — touched by loop() on every iteration.
// =============================================================================
// Mon3's matrix scan can pick up TWO simultaneously-held keys within one
// full A8-A15 scan cycle (e.g. Shift+3) -- it saves whichever row/col pairs
// it sees a low data bit on, then on completion decides itself whether to
// treat the first as a modifier for the second (Shift, Ctrl, Function).
// The Pico's job is only to faithfully present BOTH pressed-key matrix
// connections during the scan, in whichever order Mon3 happens to scan
// their rows -- it does NOT need to know or care which one is "the
// modifier". That decision is entirely Mon3/Monitor3's, matching the
// raw-passthrough design already used for Shift/Ctrl/Caps elsewhere in
// this sketch.
//
// So we track up to 2 independent (address-row-mask, data-line-mask)
// slots. mask == 0 in a slot means "that slot is empty / not in use".
//
// activeAddrMask[i] / activeDataMask[i] are written only from
// processReport() (the USB HID callback) and read only from loop().
volatile uint32_t activeAddrMask[2] = {0, 0};
volatile uint32_t activeDataMask[2] = {0, 0};

// Precomputed "expected address-bus value" per slot: ALL_ADDR_MASK with the
// slot's own row bit cleared. loop() compares its masked gpio_in read against
// this value DIRECTLY -- no per-iteration invert/AND against the row mask.
//
// EMPTY_MATCH (all 32 bits set) marks an empty slot. It is chosen because the
// hot loop's address read is masked to ALL_ADDR_MASK, so it can NEVER equal
// 0xFFFFFFFF -- an empty slot is structurally incapable of matching. (A 0
// sentinel would NOT be safe: A8-A15 all-low is a real bus state -- the Z80
// fetches Mon3 ROM code from addresses 0x00xx -- and during the few-ns
// publish/clear window a 0-sentinel slot could spuriously match it.)
//
// This is the hot loop's "slot live" flag: it is written LAST when a slot is
// populated and reset to EMPTY_MATCH FIRST when a slot is released.
#define EMPTY_MATCH 0xFFFFFFFFu
volatile uint32_t activeMatchPattern[2] = {EMPTY_MATCH, EMPTY_MATCH};

// Which HID keycode (or modifier tag) currently occupies each slot, so we
// can tell when a held key has actually been released vs. just not being
// the first keycode in this particular report anymore. 0 = empty slot.
// Stored as: bit 0-7 = HID keycode, or SLOT_TAG_BIT (0x8000) | SK_* tag
// for modifier-only slots.
volatile uint16_t activeSlotKey[2] = {0, 0};
#define SLOT_TAG_BIT 0x8000  // marks a slot as holding a modifier tag, not a HID keycode

// ─── Key Map Lookup (NOT hot path — runs only on HID report arrival) ────────

bool findCharKey(char c, int8_t &addr, int8_t &data) {
  for (uint8_t r = 0; r < 8; r++)
    for (uint8_t d = 0; d < 8; d++)
      if (charMap[r][d] == c) { addr = r; data = d; return true; }
  return false;
}

bool findSpecialKey(uint8_t tag, int8_t &addr, int8_t &data) {
  if (tag == SK_NONE) return false;
  for (uint8_t r = 0; r < 8; r++)
    for (uint8_t d = 0; d < 8; d++)
      if (specialMap[r][d] == tag) { addr = r; data = d; return true; }
  return false;
}

char hidToAscii(uint8_t keycode) {
  if (keycode >= 0x04 && keycode <= 0x1D) return 'a' + (keycode - 0x04);
  if (keycode >= 0x1E && keycode <= 0x26) return '1' + (keycode - 0x1E);
  if (keycode == 0x27) return '0';
  switch (keycode) {
    case 0x2C: return ' ';
    case 0x2D: return '-';
    case 0x2E: return '=';
    case 0x31: return '\\';
    case 0x33: return ';';
    case 0x34: return '\'';
    case 0x36: return ',';
    case 0x37: return '.';
    case 0x38: return '/';
    default:   return 0;
  }
}

uint8_t hidToSpecial(uint8_t keycode) {
  switch (keycode) {
    case HID_KEY_UP_ARROW:    return SK_UP;
    case HID_KEY_DOWN_ARROW:  return SK_DOWN;
    case HID_KEY_LEFT_ARROW:  return SK_LEFT;
    case HID_KEY_RIGHT_ARROW: return SK_RIGHT;
    case HID_KEY_BACKSPACE:   return SK_DEL;
    case HID_KEY_TAB:         return SK_TAB;
    case HID_KEY_ENTER:       return SK_ENTER;
    case HID_KEY_ESCAPE:      return SK_ESC;
    case HID_KEY_CAPS_LOCK:   return SK_CAPS;
    case HID_KEY_F1:          return SK_FUNC;
    default:                  return SK_NONE;
  }
}

// Set the active key by (row, col) index -- converts to bitmasks ONCE here,
// so loop() never has to.
// ─── Slot Management (NOT hot path — runs only on HID report arrival) ───────

// Find which slot (0 or 1) currently holds the given keycode/tag, or -1.
int8_t findSlotByKey(uint16_t key) {
  for (uint8_t s = 0; s < 2; s++) {
    if (activeSlotKey[s] == key) return s;
  }
  return -1;
}

// Find an empty slot (mask == 0), or -1 if both are full.
int8_t findEmptySlot() {
  for (uint8_t s = 0; s < 2; s++) {
    if (activeAddrMask[s] == 0) return s;
  }
  return -1;
}

// Release one slot and clear it. Data pins remain outputs and the HCT245
// remains enabled; Core 0 updates the presented byte on the next new row.
void clearSlot(uint8_t s) {
  activeMatchPattern[s] = EMPTY_MATCH;  // hot loop's "slot live" flag -- reset FIRST
  activeAddrMask[s] = 0;
  activeDataMask[s] = 0;
  activeSlotKey[s]  = 0;
}

void clearAllSlots() {
  clearSlot(0);
  clearSlot(1);
}

// Occupy a slot with a given (row, col) key. If the same `key` tag is
// already in a slot, this just refreshes it (no-op on the masks, which
// is fine since they'd be identical). If both slots are full and this is
// a NEW key, the oldest design choice here is to ignore the 3rd+ key --
// Mon3's own matrix scan can only resolve 2 keys per scan cycle anyway,
// so a 3rd simultaneous key isn't representable on the real hardware
// either, and silently ignoring it is the correct behaviour here.
void occupySlot(uint16_t key, int8_t addr, int8_t data) {
  int8_t existing = findSlotByKey(key);
  if (existing >= 0) {
    // Already tracked in this slot -- nothing to change.
    return;
  }

  int8_t slot = findEmptySlot();
  if (slot < 0) {
    // Both slots full and this is a 3rd distinct key -- matches Mon3's own
    // 2-key-per-scan hardware limit, so we drop it rather than displace
    // an existing held key.
    return;
  }

  // Data masks FIRST, match pattern LAST: loop() on Core 0 treats a slot as
  // live only via its match pattern, so it must be the final write --
  // otherwise one hot-loop iteration could see a live slot whose data bit
  // isn't set yet (missed scan at the press-edge).
  activeDataMask[slot]     = DATA_PIN_MASK[data];
  activeAddrMask[slot]     = ADDR_PIN_MASK[addr];
  activeSlotKey[slot]      = key;
  activeMatchPattern[slot] = ALL_ADDR_MASK & ~ADDR_PIN_MASK[addr];

}

// Release any slot whose tracked key is NOT present in the keepKeys list.
// keepCount tells us how many of keepKeys[] are valid (0, 1, or 2).
void releaseSlotsNotIn(uint16_t const *keepKeys, uint8_t keepCount) {
  for (uint8_t s = 0; s < 2; s++) {
    if (activeSlotKey[s] == 0) continue;  // already empty

    // Caps is a timed one-shot, not a member of the normal held-key list.
    // Keep its slot alive until serviceCapsPulse() releases it.
    if (capsPulseActive && activeSlotKey[s] == HID_KEY_CAPS_LOCK) continue;

    bool stillHeld = false;
    for (uint8_t k = 0; k < keepCount; k++) {
      if (keepKeys[k] == activeSlotKey[s]) { stillHeld = true; break; }
    }

    if (!stillHeld) {
      clearSlot(s);
    }
  }
}

// ─── USB HID Report Processing (NOT hot path) ────────────────────────────────

// Build a unique key-tag for a modifier, distinct from any HID keycode
// range (HID keycodes are 0x00-0xFF, so we set bit 15 to disambiguate).
uint16_t modifierTag(uint8_t skTag) {
  return SLOT_TAG_BIT | skTag;
}

// ─── VNP reset and Caps LED helpers (Core 1, NOT hot path) ──────────────────

bool reportContainsKey(hid_keyboard_report_t const *report, uint8_t keycode) {
  for (uint8_t i = 0; i < 6; i++) {
    if (report->keycode[i] == keycode) return true;
  }
  return false;
}

void startCapsPulse() {
  // Do not retrigger while a previous one-shot is still active.
  if (capsPulseActive) return;

  int8_t addr = -1, data = -1;
  if (!findSpecialKey(SK_CAPS, addr, data)) return;

  // Use the normal slot publisher so the timing-critical Core 0 loop remains
  // completely unchanged. If both matrix slots are already occupied, Caps
  // cannot be represented at that instant and is ignored like any 3rd key.
  occupySlot(HID_KEY_CAPS_LOCK, addr, data);

  if (findSlotByKey(HID_KEY_CAPS_LOCK) >= 0) {
    capsPulseActive = true;
    capsPulseReleaseAt = millis() + CAPS_MATRIX_PULSE_MS;
  }
}

void serviceCapsPulse() {
  if (!capsPulseActive) return;

  if ((int32_t)(millis() - capsPulseReleaseAt) >= 0) {
    int8_t slot = findSlotByKey(HID_KEY_CAPS_LOCK);
    if (slot >= 0) clearSlot((uint8_t)slot);
    capsPulseActive = false;
  }
}

void triggerReset() {
  const uint32_t now = millis();

  clearAllSlots();
  capsPulseActive = false;
  capsUsbWasHeld = false;
  RESET_ASSERT();
  resetActive = true;
  resetReleaseAt = now + RESET_PULSE_MS;

  // Reboot the Pico only AFTER the TEC reset pulse has been released.
  picoRebootPending = true;
  picoRebootAt = now + RESET_PULSE_MS + PICO_REBOOT_AFTER_TEC_MS;
}

void serviceResetPulse() {
  const uint32_t now = millis();

  if (resetActive && (int32_t)(now - resetReleaseAt) >= 0) {
    RESET_RELEASE();
    resetActive = false;
  }

  if (picoRebootPending && (int32_t)(now - picoRebootAt) >= 0) {
    // The TEC has already had its full reset pulse and GP27 is released.
    // watchdog_reboot(0, 0, 0) performs a normal RP2040 reboot from flash.
    picoRebootPending = false;
    watchdog_reboot(0, 0, 0);

    // Normally the reset occurs immediately. Do not resume USB/matrix-side
    // housekeeping in the tiny interval before the watchdog reset takes effect.
    while (true) { tight_loop_contents(); }
  }
}

void armKeyboardReceive() {
  if (!keyboardMounted) return;

  // Ask TinyUSB for the endpoint's REAL state instead of trusting a software
  // pending flag. During a normal long idle, the interrupt-IN endpoint remains
  // busy waiting for the keyboard and we leave it alone. If PIO-USB/TinyUSB
  // silently drops or aborts that transfer, the endpoint becomes ready and this
  // code automatically queues a fresh receive request.
  if (!tuh_hid_receive_ready(keyboardDevAddr, keyboardInstance)) return;

  // If queuing transiently fails, loop1() will simply try again later.
  tuh_hid_receive_report(keyboardDevAddr, keyboardInstance);
}

void serviceCapsLED() {
  if (!keyboardMounted || !keyboardProtocolReady || capsReportBusy) return;

  const uint32_t now = millis();
  const bool healthProbeDue = ((int32_t)(now - nextUsbHealthProbe) >= 0);

  // GP13 is optional. With no status bodge fitted its weak pull-down makes
  // capsNow false; once the 10k/20k divider is fitted this becomes the real
  // TEC Caps state and is mirrored to the USB keyboard LED.
  const bool capsNow = gpio_get(CAPS_STATUS_PIN);
  const bool capsNeedsSync = (!capsLedSynced || capsNow != lastCapsLedState);

  if (!capsNeedsSync && !healthProbeDue) return;
  if ((int32_t)(now - nextCapsReportAttempt) < 0) return;

  // Start (or continue) a health window. If the control transfer cannot be
  // queued/completed before this deadline, loop1() stops feeding the watchdog.
  if (!usbHealthProbeActive) {
    usbHealthProbeActive = true;
    usbHealthDeadline = now + USB_HEALTH_TIMEOUT_MS;
  }

  keyboardLedReport = capsNow ? KEYBOARD_LED_CAPSLOCK : 0;

  if (tuh_hid_set_report(keyboardDevAddr, keyboardInstance, 0,
                         HID_REPORT_TYPE_OUTPUT,
                         &keyboardLedReport, sizeof(keyboardLedReport))) {
    capsReportBusy    = true;
    capsStateInFlight = capsNow;
  } else {
    nextCapsReportAttempt = now + 10;
  }
}

void processReport(hid_keyboard_report_t const *report) {
  // ── VULCAN NERVE PINCH: Ctrl+Alt+Delete ────────────────────────────────
  // This bypasses the matrix scan entirely. The reset output is independent
  // of A8-A15 and is edge-triggered so holding the chord gives one reset.
  // Version 6 retains the Pico reboot after the 100 ms TEC reset pulse has finished.
  const bool ctrlHeld =
      (report->modifier & (HID_MOD_LEFT_CTRL | HID_MOD_RIGHT_CTRL));
  const bool altHeld =
      (report->modifier & (HID_MOD_LEFT_ALT | HID_MOD_RIGHT_ALT));
  const bool deleteHeld = reportContainsKey(report, HID_KEY_DELETE);
  const bool vnpHeld = ctrlHeld && altHeld && deleteHeld;

  if (vnpHeld) {
    if (!vnpWasHeld) triggerReset();
    vnpWasHeld = true;
    return;  // suppress the Ctrl/Alt/Delete chord from the TEC matrix
  }
  vnpWasHeld = false;

  // ── CAPS LOCK: one software-generated matrix pulse per USB press ─────────
  // Do not expose Caps as a continuously-held TEC matrix switch. A rising
  // USB key edge starts one short pulse; repeated HID reports while held are
  // ignored until an actual USB release has been seen.
  const bool capsHeld = reportContainsKey(report, HID_KEY_CAPS_LOCK);
  if (capsHeld && !capsUsbWasHeld) {
    startCapsPulse();
  }
  capsUsbWasHeld = capsHeld;

  // ── Step 1: build the list of currently-held key "tags" from this report ──
  // Each tag is either a HID keycode (0x00-0xFF) for a regular key, or a
  // modifier tag (SLOT_TAG_BIT | SK_SHIFT / SK_CTRL) for Shift/Ctrl.
  // Mon3's matrix scan can resolve at most 2 simultaneous keys, so we only
  // keep the first 2 distinct tags we find here -- same hardware limit as
  // occupySlot() enforces, kept consistent in both places.
  uint16_t heldKeys[2] = {0, 0};
  uint8_t heldCount = 0;

  const bool shiftHeld =
      (report->modifier & (HID_MOD_LEFT_SHIFT | HID_MOD_RIGHT_SHIFT));

  // Modifiers are added first, since in a Shift+3 combo Mon3 expects the
  // modifier's row to be one of the two it picks up -- order they're added
  // here doesn't actually matter for correctness (Mon3 scans all 8 rows
  // regardless), but doing modifiers first keeps behaviour predictable.
  if (shiftHeld && heldCount < 2) heldKeys[heldCount++] = modifierTag(SK_SHIFT);
  if (ctrlHeld  && heldCount < 2) heldKeys[heldCount++] = modifierTag(SK_CTRL);

  for (uint8_t i = 0; i < 6 && heldCount < 2; i++) {
    uint8_t kc = report->keycode[i];
    if (kc == 0x00) continue;
    if (kc == HID_KEY_CAPS_LOCK) continue;  // handled by one-shot logic above
    heldKeys[heldCount++] = kc;  // plain HID keycode as the tag
  }

  // ── Step 2: release any tracked slot whose key is no longer held ─────────
  releaseSlotsNotIn(heldKeys, heldCount);

  // ── Step 3: for each currently-held key not already tracked, resolve it
  //            to a (row, col) matrix cell and occupy a free slot ─────────
  for (uint8_t i = 0; i < heldCount; i++) {
    uint16_t key = heldKeys[i];
    int8_t addr = -1, data = -1;

    if (key & SLOT_TAG_BIT) {
      // Modifier tag -- look up directly by SK_* value
      uint8_t skTag = key & 0x7FFF;
      findSpecialKey(skTag, addr, data);
    } else {
      // Regular HID keycode -- check special keys first, then printable
      uint8_t tag = hidToSpecial((uint8_t)key);
      if (tag != SK_NONE) {
        findSpecialKey(tag, addr, data);
      } else {
        char c = hidToAscii((uint8_t)key);
        if (c != 0) findCharKey(c, addr, data);
      }
    }

    if (addr >= 0 && data >= 0) {
      occupySlot(key, addr, data);
    }
  }

}

// ─── TinyUSB Callbacks ────────────────────────────────────────────────────────

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                       uint8_t const *desc_report, uint16_t desc_len) {
  (void)desc_report;
  (void)desc_len;

  const uint8_t protocol = tuh_hid_interface_protocol(dev_addr, instance);

  if (protocol == HID_ITF_PROTOCOL_KEYBOARD) {
    keyboardDevAddr         = dev_addr;
    keyboardInstance        = instance;
    keyboardMounted         = true;
    keyboardProtocolReady   = false;
    capsReportBusy          = false;
    capsLedSynced           = false;
    nextCapsReportAttempt   = 0;
    usbHealthProbeActive    = false;
    usbHealthDeadline       = 0;
    nextUsbHealthProbe      = millis() + USB_HEALTH_PROBE_MS;

    // Use boot protocol so reports have the standard 8-byte keyboard format.
    // Do not send the Caps LED SET_REPORT until the completion callback below
    // confirms that this asynchronous control transfer has finished.
    if (!tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT)) {
      // If SET_PROTOCOL cannot even be queued, most boot keyboards are already
      // usable in the required format.  Allow LED service later rather than
      // hammering EP0 here; keyboard input reception is independent.
      keyboardProtocolReady = true;
    }

    armKeyboardReceive();
    return;
  }

  // Keep receiving on non-keyboard HID interfaces of composite devices.
  tuh_hid_receive_report(dev_addr, instance);
}

// TinyUSB host control-transfer completion callbacks.  These are deliberately
// used to serialize keyboard LED SET_REPORT operations on endpoint zero.
void tuh_hid_set_protocol_complete_cb(uint8_t dev_addr, uint8_t instance,
                                      uint8_t protocol) {
  (void)protocol;
  if (keyboardMounted && dev_addr == keyboardDevAddr &&
      instance == keyboardInstance) {
    keyboardProtocolReady = true;
    capsLedSynced = false;          // sync optional GP13 Caps state next
    nextCapsReportAttempt = millis();
  }
}

void tuh_hid_set_report_complete_cb(uint8_t dev_addr, uint8_t instance,
                                    uint8_t report_id, uint8_t report_type,
                                    uint16_t len) {
  (void)report_id;
  (void)report_type;

  if (keyboardMounted && dev_addr == keyboardDevAddr &&
      instance == keyboardInstance) {
    capsReportBusy = false;

    // Any completed SET_REPORT proves the control endpoint and host task are
    // still making forward progress, even if the device reports a short/failed
    // transfer. Schedule the next periodic health probe from this completion.
    usbHealthProbeActive = false;
    usbHealthDeadline = 0;
    nextUsbHealthProbe = millis() + USB_HEALTH_PROBE_MS;

    if (len == sizeof(keyboardLedReport)) {
      lastCapsLedState = capsStateInFlight;
      capsLedSynced = true;
    } else {
      // Failed/stalled transfer: retry later rather than immediately.
      capsLedSynced = false;
      nextCapsReportAttempt = millis() + 20;
    }
  }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
  if (keyboardMounted && dev_addr == keyboardDevAddr &&
      instance == keyboardInstance) {
    keyboardMounted         = false;
    keyboardDevAddr         = 0;
    keyboardInstance        = 0;
    keyboardProtocolReady   = false;
    capsReportBusy          = false;
    capsLedSynced           = false;
    usbHealthProbeActive    = false;
    usbHealthDeadline       = 0;
    nextUsbHealthProbe      = 0;
    vnpWasHeld              = false;
    capsUsbWasHeld          = false;
    capsPulseActive         = false;
    clearAllSlots();
  }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                  uint8_t const *report, uint16_t len) {
  // Only decode the boot-keyboard interface selected at mount time.
  if (!keyboardMounted || dev_addr != keyboardDevAddr ||
      instance != keyboardInstance) {
    tuh_hid_receive_report(dev_addr, instance);
    return;
  }

  // This interrupt-IN transfer has completed. Process the report, then ask
  // TinyUSB whether the endpoint is ready for the next receive request.
  if (len >= sizeof(hid_keyboard_report_t)) {
    processReport(reinterpret_cast<hid_keyboard_report_t const *>(report));
  }

  armKeyboardReceive();
}

// ─── Core 1 — USB Host / slow auxiliary tasks ─────────────────────────────────
// Core 1 owns USB, the non-blocking reset timer, and the TEC Caps status input.
// None of this code participates in the sub-microsecond matrix response.

void setup1() {}

void loop1() {
  // The watchdog is deliberately owned/fed by Core 1. Core 0's matrix hot loop
  // can continue forever even if USB wedges, so feeding it from Core 0 would
  // hide exactly the failure we are trying to recover from.
  if (!watchdogStarted) {
    watchdog_enable(USB_WATCHDOG_MS, true);
    watchdogStarted = true;
  }

  // If USBHost.task() itself ever blocks permanently, execution never reaches
  // watchdog_update() below and the RP2040 automatically reboots.
  USBHost.task();

  armKeyboardReceive();       // re-arm whenever TinyUSB says endpoint is free
  serviceResetPulse();
  serviceCapsPulse();
  serviceCapsLED();

  bool usbHealthy = true;
  if (keyboardMounted && usbHealthProbeActive &&
      (int32_t)(millis() - usbHealthDeadline) >= 0) {
    usbHealthy = false;
  }

  // No keyboard mounted is a valid state and must not cause reboot loops. When
  // a keyboard IS mounted, an overdue control-transfer health probe intentionally
  // stops the feed so the hardware watchdog gives us a clean Pico restart.
  if (usbHealthy) {
    watchdog_update();
  }
}

// ─── Core 0 ───────────────────────────────────────────────────────────────────

void setup() {
  // USB CDC serial is optional debug only. Do NOT wait for a PC terminal:
  // when powered from the TEC the keyboard interface must start immediately.
  Serial.begin(115200);

  // HCT245 /OE startup sequencing. External 10k pull-up to Pico 3V3 holds
  // this HIGH before firmware runs. Preload the GPIO output latch HIGH before
  // changing GP5 to an output so there is no brief enable glitch.
  gpio_init(OE_PIN);
  gpio_put(OE_PIN, 1);
  gpio_set_dir(OE_PIN, GPIO_OUT);

  // Reset control via external open-drain/open-collector stage. Keep the gate/
  // base drive LOW so reset is released.
  gpio_init(RESET_PIN);
  RESET_RELEASE();
  gpio_set_dir(RESET_PIN, GPIO_OUT);

  // Optional TEC Caps-status input. Keep a weak pull-down enabled so GP13 is
  // defined even when the status bodge is absent. If the external 10k/20k
  // divider is fitted, the TEC's HIGH level comfortably overrides this pull.
  gpio_init(CAPS_STATUS_PIN);
  gpio_set_dir(CAPS_STATUS_PIN, GPIO_IN);
  gpio_pull_down(CAPS_STATUS_PIN);

  // Address inputs -- pull-ups, Mon3 pulls one LOW at a time.
  // (ALL_ADDR_MASK / ALL_DATA_MASK are compile-time #defines now -- only the
  // per-pin arrays used by the slot-management code are built here.)
  for (uint8_t i = 0; i < 8; i++) {
    gpio_init(ADDR_PINS[i]);
    gpio_set_dir(ADDR_PINS[i], GPIO_IN);
    gpio_pull_up(ADDR_PINS[i]);
    ADDR_PIN_MASK[i] = 1u << ADDR_PINS[i];
  }

  // Data pins are always outputs. /OE is still HIGH at this point, so the TEC
  // sees nothing while the Pico initializes every latch to HIGH (0xFF).
  for (uint8_t i = 0; i < 8; i++) {
    gpio_init(DATA_PINS[i]);
    gpio_set_dir(DATA_PINS[i], GPIO_OUT);
    gpio_put(DATA_PINS[i], 1);
    DATA_PIN_MASK[i] = 1u << DATA_PINS[i];
  }

  // Initialise USB host on Core 0 (setup1 not reliable on earlephilhower).
  USBHost.begin(1);  // rhport 1 = PIO-USB on GP0/GP1

  // All data latches are now known-good. Enable the HCT245 ONCE and leave it
  // enabled forever; the TEC's own 245 controls connection to the Z80 bus.
  OE_ENABLE();

}

// =============================================================================
// loop() -- THE HOT PATH. Runs continuously from RAM (__not_in_flash_func).
//
// IMPORTANT ARDUINO-CORE DETAIL:
//   The Earle Philhower Arduino core normally executes:
//
//       loop();
//       __loop();
//
//   and with USE_TINYUSB enabled __loop() calls yield(). Returning from loop()
//   after each GPIO sample therefore inserts non-deterministic housekeeping gaps
//   into the matrix polling. A missed matrix row looks like a key release to
//   MON3; the next successful row then looks like a new press. That produces
//   very fast apparent repeat and can make Caps Lock toggle repeatedly.
//
//   Therefore this loop deliberately NEVER RETURNS. It contains its own tight
//   infinite polling loop on Core 0. USB HOST work, Caps LED mirroring and the
//   VNP reset timer all continue independently on Core 1.
//
// PHYSICAL-MATRIX RULE:
//   USB HID reports are treated only as CURRENT KEY STATE. No typematic/repeat
//   pulses are generated for ordinary keys. A normal USB key held down remains a
//   continuously closed virtual matrix switch until physically released.
//   CAPS LOCK is the deliberate exception in Version 6: one short matrix pulse is
//   generated on the USB press edge, preventing MON3 from repeatedly toggling
//   Caps while the PC key is held.
//
// FREEZE / FAIL-SAFE RULE:
//   - D0-D7 are reconsidered ONLY when the observed A8-A15 value changes.
//   - While A8-A15 remains unchanged, D0-D7 are frozen completely. A USB HID
//     report arriving on Core 1 therefore cannot alter the byte mid-read.
//   - On every A8-A15 transition, compare the new value with the two active
//     matrix-row match patterns. If neither matches, present 0xFF immediately.
//   - If one or both slots match, present their active-low data bit(s).
//
// The HCT245 stays enabled; the TEC's own buffer determines when these
// keyboard-return signals actually reach the Z80 data bus.
// =============================================================================

void __not_in_flash_func(loop)() {
  // Last observed upper-address pattern. As long as A8-A15 are unchanged,
  // the presented D0-D7 byte cannot change.
  uint32_t lastAddrBits = 0xFFFFFFFFu;

  // DATA GPIO bits currently held LOW. setup() starts all eight HIGH.
  uint32_t drivenMask = 0;

  // Never return to the Arduino core. In particular, do not allow the core's
  // post-loop yield() to insert timing holes into matrix polling.
  for (;;) {
    const uint32_t addrBits = sio_hw->gpio_in & ALL_ADDR_MASK;

    // Same upper address as previous sample: freeze D0-D7 completely.
    if (addrBits == lastAddrBits) continue;
    lastAddrBits = addrBits;

    // Snapshot Core 1's published held-key state only on an address change.
    const uint32_t match0 = activeMatchPattern[0];
    const uint32_t match1 = activeMatchPattern[1];

    uint32_t wantMask = 0;
    if (addrBits == match0) wantMask  = activeDataMask[0];
    if (addrBits == match1) wantMask |= activeDataMask[1];

    // A non-key row is 0xFF (no columns low). gpio_togl changes all required
    // data bits atomically in one SIO write, with no intermediate 0xFF state.
    const uint32_t changeMask = drivenMask ^ wantMask;
    if (changeMask) sio_hw->gpio_togl = changeMask;
    drivenMask = wantMask;
  }
}
