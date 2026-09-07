// =============================================================================
// TEC-1G Matrix Keyboard Interface — Raspberry Pi Pico (RP2040)
// =============================================================================
//
// Hardware:
//   - Raspberry Pi Pico running at 120MHz (required for PIO-USB)
//   - USB keyboard connected via GP0 (D+) / GP1 (D-)
//   - 74HCT245 level shifters on all 16 I/O lines (3.3V <-> 5V)
//
// platformio.ini:
//   platform = https://github.com/maxgerhardt/platform-raspberrypi.git
//   board = pico
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
//   GP16-GP22  Outputs D0-D6   (to Mon3 data bus,    via level shifter)
//   GP28       Output  D7      (to Mon3 data bus,    via level shifter)
//
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
//   So the hot path (loop(), running continuously on Core 0) does the
//   absolute minimum possible:
//     1. ONE 32-bit read of all GPIO pins at once (SIO register, ~1 cycle)
//     2. ONE compare per slot against a PRECOMPUTED expected-address value
//     3. ~3 SIO register writes to drive/release the data pins
//   It also runs from RAM (__not_in_flash_func) so USB-host activity on
//   Core 1 cannot stall it via shared XIP flash-cache evictions, and every
//   aggregate mask it touches is a compile-time immediate, not a RAM load.
//
//   All the "thinking" -- which key is pressed, which row/col that maps to,
//   converting that to GPIO bitmasks -- happens in processReport(), which
//   only runs when a USB HID report arrives (i.e. on key press/release).
//   That code has microseconds to spare and is never on the critical path.
// =============================================================================

#include "Adafruit_TinyUSB.h"
#include "pico/stdlib.h"
#include "hardware/structs/sio.h"

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

// ─── 74AHCT245 Output Enable control pin ─────────────────────────────────────
// OE is active LOW on the 74AHCT245.
// GP5 HIGH = OE high = chip outputs disabled (high-Z on B-side, Mon3 pull-ups
//            hold data bus at 0xFF = no key pressed)
// GP5 LOW  = OE low  = chip outputs enabled  (Pico drives the data bus)
//
// A 10kΩ pull-up resistor from OE to 5V is recommended on the PCB/breadboard
// so that if the Pico resets or is unpowered, the chip stays safely disabled
// and the TEC-1G data bus is never left floating or driven unexpectedly.
#define OE_PIN 5
#define OE_MASK (1u << OE_PIN)
// Raw SIO writes -- identical registers to gpio_put(), spelled out so the hot
// path never depends on wrapper-call codegen. GP5 is configured as OUTPUT in
// setup() before these are ever executed.
#define OE_DISABLE() (sio_hw->gpio_set = OE_MASK)   // HIGH = disable outputs
#define OE_ENABLE()  (sio_hw->gpio_clr = OE_MASK)   // LOW  = enable outputs

// ─── Precomputed GPIO bitmasks ────────────────────────────────────────────────
// Per-pin masks are built once in setup() from ADDR_PINS[] / DATA_PINS[] for
// the (non-hot) slot-management code. The two aggregate masks are COMPILE-TIME
// constants: the hot loop ANDs/compares them on every iteration, and as RAM
// globals they would cost an extra load each pass instead of folding into
// immediate operands.
uint32_t ADDR_PIN_MASK[8];   // bit mask for each address line, e.g. 1<<ADDR_PINS[i]
uint32_t DATA_PIN_MASK[8];   // bit mask for each data line

// All 8 data pins OR'd -- used to drive all latches HIGH before pulling one
// LOW, so the 74AHCT245 sees 0xFF minus exactly one bit rather than all-zero.
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
#define HID_KEY_RIGHT_ARROW  0x4F
#define HID_KEY_LEFT_ARROW   0x50
#define HID_KEY_DOWN_ARROW   0x51
#define HID_KEY_UP_ARROW     0x52

#define HID_MOD_LEFT_CTRL    0x01
#define HID_MOD_LEFT_SHIFT   0x02
#define HID_MOD_RIGHT_SHIFT  0x20
#define HID_MOD_RIGHT_CTRL   0x10

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

// Release one slot and clear it. Data pins stay as OUTPUT -- the latch
// value doesn't matter since OE on the chip controls bus access.
void clearSlot(uint8_t s) {
  activeMatchPattern[s] = EMPTY_MATCH;  // hot loop's "slot live" flag -- reset FIRST
  activeAddrMask[s] = 0;
  activeDataMask[s] = 0;
  activeSlotKey[s]  = 0;
}

void clearAllSlots() {
  clearSlot(0);
  clearSlot(1);
  OE_DISABLE();  // both slots empty -- fully release the data bus
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
    Serial.println("3rd simultaneous key ignored (matrix scan limit is 2)");
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

  Serial.print("Slot ");
  Serial.print(slot);
  Serial.print(" -> addr=A");
  Serial.print(8 + addr);
  Serial.print(" data=D");
  Serial.println(data);
}

// Release any slot whose tracked key is NOT present in the keepKeys list.
// keepCount tells us how many of keepKeys[] are valid (0, 1, or 2).
void releaseSlotsNotIn(uint16_t const *keepKeys, uint8_t keepCount) {
  for (uint8_t s = 0; s < 2; s++) {
    if (activeSlotKey[s] == 0) continue;  // already empty

    bool stillHeld = false;
    for (uint8_t k = 0; k < keepCount; k++) {
      if (keepKeys[k] == activeSlotKey[s]) { stillHeld = true; break; }
    }

    if (!stillHeld) {
      Serial.print("Slot ");
      Serial.print(s);
      Serial.println(" released (key no longer held)");
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

void processReport(hid_keyboard_report_t const *report) {
  // ── Step 1: build the list of currently-held key "tags" from this report ──
  // Each tag is either a HID keycode (0x00-0xFF) for a regular key, or a
  // modifier tag (SLOT_TAG_BIT | SK_SHIFT / SK_CTRL) for Shift/Ctrl.
  // Mon3's matrix scan can resolve at most 2 simultaneous keys, so we only
  // keep the first 2 distinct tags we find here -- same hardware limit as
  // occupySlot() enforces, kept consistent in both places.
  uint16_t heldKeys[2] = {0, 0};
  uint8_t heldCount = 0;

  bool shiftHeld = (report->modifier & (HID_MOD_LEFT_SHIFT | HID_MOD_RIGHT_SHIFT));
  bool ctrlHeld  = (report->modifier & (HID_MOD_LEFT_CTRL  | HID_MOD_RIGHT_CTRL));

  // Modifiers are added first, since in a Shift+3 combo Mon3 expects the
  // modifier's row to be one of the two it picks up -- order they're added
  // here doesn't actually matter for correctness (Mon3 scans all 8 rows
  // regardless), but doing modifiers first keeps behaviour predictable.
  if (shiftHeld && heldCount < 2) heldKeys[heldCount++] = modifierTag(SK_SHIFT);
  if (ctrlHeld  && heldCount < 2) heldKeys[heldCount++] = modifierTag(SK_CTRL);

  for (uint8_t i = 0; i < 6 && heldCount < 2; i++) {
    uint8_t kc = report->keycode[i];
    if (kc == 0x00) continue;
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

  if (heldCount == 0) {
    OE_DISABLE();
    Serial.println("All keys released");
  }
}

// ─── TinyUSB Callbacks ────────────────────────────────────────────────────────

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                       uint8_t const *desc_report, uint16_t desc_len) {
  Serial.print("USB keyboard connected. dev_addr=");
  Serial.print(dev_addr);
  Serial.print(" instance=");
  Serial.println(instance);
  tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
  tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
  Serial.println("USB keyboard disconnected.");
  clearAllSlots();
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                  uint8_t const *report, uint16_t len) {
  if (len < sizeof(hid_keyboard_report_t)) {
    tuh_hid_receive_report(dev_addr, instance);
    return;
  }
  processReport(reinterpret_cast<hid_keyboard_report_t const *>(report));
  tuh_hid_receive_report(dev_addr, instance);
}

// ─── Core 1 — USB Host Task ───────────────────────────────────────────────────
// NOTE: USBHost.begin() is called from setup() on Core 0 (not setup1()) --
// the earlephilhower core does not reliably call setup1() in all versions,
// see project history. loop1() is still used to service the USB task so it
// doesn't compete with the time-critical loop() on Core 0.

void setup1() {}

void loop1() {
  USBHost.task();
}

// ─── Core 0 ───────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 10000);
  Serial.println("TEC-1G USB Keyboard Interface starting...");

  // OE control for 74AHCT245 -- start disabled (HIGH) so the data bus
  // is fully released until we actually have a key to assert.
  gpio_init(OE_PIN);
  gpio_set_dir(OE_PIN, GPIO_OUT);
  OE_DISABLE();

  // Address input pins -- pull-ups, Mon3 pulls one LOW at a time.
  // (ALL_ADDR_MASK / ALL_DATA_MASK are compile-time #defines now -- only the
  // per-pin arrays used by the slot-management code are built here.)
  for (uint8_t i = 0; i < 8; i++) {
    gpio_init(ADDR_PINS[i]);
    gpio_set_dir(ADDR_PINS[i], GPIO_IN);
    gpio_pull_up(ADDR_PINS[i]);
    ADDR_PIN_MASK[i] = 1u << ADDR_PINS[i];
  }

  // Data pins -- always OUTPUT mode. The 74AHCT245's OE pin controls whether
  // these values actually reach the TEC-1G bus (OE HIGH = chip disabled,
  // OE LOW = chip enabled). Default latch value is HIGH (1) on all pins so
  // when OE is enabled the bus reads 0xFF (no key). Only the active key's
  // bit is pulled LOW just before OE is enabled in the hot loop.
  for (uint8_t i = 0; i < 8; i++) {
    gpio_init(DATA_PINS[i]);
    gpio_set_dir(DATA_PINS[i], GPIO_OUT);
    gpio_put(DATA_PINS[i], 1);              // latch HIGH = 0xFF default
    DATA_PIN_MASK[i] = 1u << DATA_PINS[i];
  }

  // Initialise USB host on Core 0 (setup1 not reliable on earlephilhower)
  USBHost.begin(1);  // rhport 1 = PIO-USB on GP0/GP1

  Serial.println("Ready -- plug in USB keyboard.");
}

// =============================================================================
// loop() -- THE HOT PATH. Runs continuously from RAM (__not_in_flash_func),
// and must complete its useful work well within Mon3's data-setup budget:
// Mon3 samples each matrix row ONCE per scan pass (single IN A,(C) per row,
// no retry), latching the data bus ~600-750ns after the address lines change
// (IN A,(C) = 12 cycles @ 4MHz = 3us total, but the address is only valid
// ~2.5-3 cycles before the data sample point).
//
// ADDRESS QUALIFICATION:
//   A8-A15 are wired directly off the Z80 address bus via transistors, so
//   they are NOT exclusively driven during the deliberate IN A,(C) keyboard
//   scan -- they fluctuate any time the Z80's address bus happens to match
//   that bit pattern for an unrelated reason (other instruction fetches,
//   memory access, etc). IORQ and the port-number byte (which would let us
//   qualify "this is really an IO read of port 0xFE") are not broken out
//   on this board, so we can't gate on those directly.
//
//   Instead we exploit how Mon3's real scan behaves: while it's actively
//   scanning the keyboard, EXACTLY ONE of A8-A15 is low and the other
//   seven are high, and this repeats continuously in a tight loop. Random
//   bus glitching from unrelated activity is very unlikely to produce that
//   exact one-bit-low / seven-bits-high pattern on this specific group of
//   eight lines at the moment we sample them.
//
// TWO-KEY SUPPORT:
//   Mon3's matrix scan can pick up 2 simultaneously-held keys across one
//   full A8-A15 scan cycle (e.g. Shift+3), since it scans each row in turn
//   and records whichever rows show a low data bit. The Pico doesn't need
//   to know which slot is "the modifier" -- it only needs to correctly
//   present BOTH held keys' matrix connections, each on its own row, as
//   Mon3's scan visits that row. Mon3 decides on its own, after the full
//   scan, how to interpret having seen two keys (e.g. Shift+3 -> '#').
//
//   Both slots are tested against the SAME address-bus read every loop
//   iteration. Since Mon3 can only have one row active at a time, at most
//   one slot will match on any given iteration in normal operation -- but
//   each slot is checked independently and safely regardless.
//
// Per iteration, this does:
//   2 loads (activeMatchPattern[0..1])  -- the only cross-core state read
//   1 read  (sio_hw->gpio_in)           -- all GPIO pins, one cycle
//   1 AND   (extract watched address bits, compile-time mask)
//   2 compares (one per slot) + gate compare
//   ~3 SIO writes                       -- only on an actual bus state change
// ~15-25 instructions total (~150-250ns worst case), roughly 3x inside the
// budget -- the previous version recomputed the match patterns and reloaded
// the aggregate masks every iteration, which sat within ~100ns of the limit
// and dropped rows under USB/cache jitter. Still no function calls, no
// array loops, no Serial in the hot path.
// =============================================================================

void __not_in_flash_func(loop)() {
  // Persisted across iterations -- the bus state we are CURRENTLY presenting.
  //   drivenMask = data bits currently held LOW (0 = nothing driven)
  //   oeOn       = whether the 74AHCT245 is currently enabled
  // We only ever TOUCH the bus when the desired state differs from this, so a
  // steady held row is left completely alone (no per-iteration set/clr glitch,
  // no OE chatter). Every write to the data latches is done while OE is
  // DISABLED, so a data bit can never be driven HIGH onto an enabled bus.
  static uint32_t drivenMask = 0;
  static bool     oeOn       = false;

  // EMPTY_MATCH in a slot means "slot empty" -- and because the address read
  // below is masked to ALL_ADDR_MASK, an empty slot can never match it.
  const uint32_t match0 = activeMatchPattern[0];
  const uint32_t match1 = activeMatchPattern[1];

  if (match0 == EMPTY_MATCH && match1 == EMPTY_MATCH) {
    if (oeOn) { OE_DISABLE(); oeOn = false; drivenMask = 0; }
    return;
  }

  const uint32_t addrBits = sio_hw->gpio_in & ALL_ADDR_MASK;

  // ── Which slot(s) match the current address scan row ──────────────────
  // Strict per-row matching stays: each key's column is only pulled low on
  // ITS OWN row, exactly like the real matrix switch (row<->col). At most
  // one slot matches per iteration since Mon3 drives one row low at a time
  // (two keys sharing one row legitimately match together and OR their bits).
  uint32_t wantMask = 0;
  if (addrBits == match0) wantMask  = activeDataMask[0];
  if (addrBits == match1) wantMask |= activeDataMask[1];

  // ── STATE-CHANGE GATE: nothing to do if the bus already shows this ─────
  if (wantMask == drivenMask && oeOn == (wantMask != 0)) return;

  if (wantMask == 0) {
    // Leaving a key row -> release the bus FIRST, then restore latches HIGH
    // (safe now: bus is high-Z, Mon3's pull-ups hold 0xFF). Releasing fast
    // matters: the Z80 is already fetching the next instruction, and every
    // cycle we keep driving the bus is a cycle of potential contention.
    OE_DISABLE();
    oeOn = false;
    sio_hw->gpio_set = ALL_DATA_MASK;
    drivenMask = 0;
    return;
  }

  // Entering / changing a key row. Build the latch pattern while OE is
  // DISABLED so no bit is ever driven HIGH onto a live bus, then enable OE
  // LAST -- the bus makes exactly one clean 0xFF -> key-pattern transition.
  if (oeOn) { OE_DISABLE(); oeOn = false; }   // only if it was live
  sio_hw->gpio_set = ALL_DATA_MASK;           // all latches high
  sio_hw->gpio_clr = wantMask;                // pull the active bit(s) low
  OE_ENABLE();                                // present the pattern
  oeOn = true;
  drivenMask = wantMask;
}
