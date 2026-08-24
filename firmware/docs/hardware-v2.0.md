# Watchy v2.0 hardware reference

**Single source of truth for pins is [`src/board/board_v20.h`](../src/board/board_v20.h).**
This document explains *why* those values are what they are and records the traps.

Values below were taken from upstream `sqfmi/Watchy` `src/config.h` and
`src/Watchy.cpp` (the `ARDUINO_WATCHY_V20` branch) plus the SQFMI hardware page.
Do not "correct" them from memory — v2.0 differs from v1.5 in exactly the places
people misremember.

## Silicon

| | |
|---|---|
| MCU | ESP32-PICO-D4 (dual-core Xtensa LX6, 240 MHz max) |
| Flash | 4 MB, in-package |
| SRAM | 320 KB usable, **no PSRAM** |
| PlatformIO board | `watchy` ("SQFMI Watchy v2.0") |
| Battery | LiPo 3.7 V, **200 mAh** (402030) |
| Regulator | ME6211C33M5G-N LDO — its quiescent current sets our sleep-current floor |

No double-precision FPU. `float` is hardware-accelerated; `double` is emulated in
software and slow. `-Wdouble-promotion` is enabled to catch accidental widening.

## Pin map

| Function | GPIO | Electrical |
|---|---|---|
| I2C SDA | 21 | shared: PCF8563 + BMA423 |
| I2C SCL | 22 | |
| Button — Menu | 26 | **active HIGH** |
| Button — Back | 25 | **active HIGH** |
| Button — Up | **35** | **active HIGH**, input-only pin |
| Button — Down | 4 | **active HIGH** |
| Battery ADC | **34** | input-only; ×2.0 divider |
| RTC interrupt (PCF8563) | 27 | **active LOW** |
| Accelerometer INT1 (BMA423) | 14 | |
| Accelerometer INT2 (BMA423) | 12 | |
| Vibration motor | 13 | |
| Display CS | 5 | |
| Display DC | 10 | |
| Display RES | 9 | |
| Display BUSY | 19 | |

Display: 1.54" 200×200 e-paper (GDEH0154D67 / GDEY0154D67 family), driven through
GxEPD2.

## Traps

### GPIO 34–39 are input-only with no internal pull resistors

Both the **Up button (35)** and the **battery ADC (34)** land in this range.

- `pinMode(35, INPUT_PULLUP)` compiles, runs, and does **nothing**. The Up button
  works only because the board provides an external resistor.
- These pins cannot be outputs. They cannot be `gpio_hold_en()`'d as outputs.

### Buttons are active HIGH

Pressed reads `HIGH`. Consequences:

- Deep-sleep button wake is
  `esp_sleep_enable_ext1_wakeup(BTN_MASK, ESP_EXT1_WAKEUP_ANY_HIGH)`.
- Any code written against a typical active-low button board is inverted here.
- Watchy v3.0 flipped these to active low. Do not copy v3.0 code either.

### The RTC interrupt is active LOW

`esp_sleep_enable_ext0_wakeup(GPIO_NUM_27, 0)` — trigger level 0.

`ext0` and `ext1` are separate wake units, which is why the RTC gets `ext0` and
the four buttons share `ext1`. `ext1` cannot mix trigger polarities, so the
active-low RTC line could not join the active-high button mask even if we wanted.

### Battery voltage needs the ×2.0 divider

```cpp
float volts = analogReadMilliVolts(BATT_ADC_PIN) / 1000.0f * 2.0f;
```

Use `analogReadMilliVolts()`, not `analogRead()` — it applies the per-chip eFuse
ADC calibration. Raw `analogRead()` on the ESP32 is materially non-linear.

### v1.5 values that leak into v2.0 code

| | v1.5 | **v2.0** |
|---|---|---|
| Up button | 32 | **35** |
| Battery ADC | 35 | **34** |

Both revisions use the PCF8563, so RTC code ports cleanly and lends false
confidence that the rest does too. It does not.

### The board manifest does not identify the revision

PlatformIO's `watchy.json` sets `-DARDUINO_WATCHY` only. `ARDUINO_WATCHY_V20` is
defined explicitly in `platformio.ini`, and `board_v20.h` `#error`s if it is
missing — otherwise a wrong pin map would compile silently.

## Sleep and wake

Wake sources in normal operation:

- **`ext0`** — PCF8563 alarm on GPIO 27, active low. The once-a-minute tick.
- **`ext1`** — any of the four buttons going high.
- **accelerometer INT** — optional, off unless a gesture feature is enabled.
  Costs sleep current, so it is opt-in.

Before sleeping, unused GPIOs are set to `INPUT` so nothing floats or drives a
powered-down peripheral. Upstream Watchy does this with a bitmask of pins to
leave alone; we do the same in `board::power::deepSleep()`, and the mask is
derived from the pin map rather than hard-coded.

`display.hibernate()` must be called before sleep, or the panel keeps its charge
pump running.

## Things worth measuring on real hardware

The power budget uses estimates until these are measured. Numbers to take with a
µA-capable meter (see `docs/power-budget.md`):

1. Deep-sleep current, accelerometer interrupt disabled.
2. Deep-sleep current with the accelerometer interrupt enabled — decides whether
   gesture features are affordable.
3. Wake duration and average current for a minute tick with a partial refresh.
4. Same for a full refresh.
5. Whether QIO / 80 MHz flash boots reliably on this module (`board_build.f_flash`,
   `flash_mode`) — currently left at the manifest default of DIO / 40 MHz on
   purpose. A shorter wake is worth real battery life if it is stable.

## Sources

- [sqfmi/Watchy `src/config.h`](https://github.com/sqfmi/Watchy/blob/master/src/config.h)
- [sqfmi/Watchy `src/Watchy.cpp`](https://github.com/sqfmi/Watchy/blob/master/src/Watchy.cpp)
- [Watchy hardware page](https://watchy.sqfmi.com/docs/hardware/)
- [PlatformIO — SQFMI Watchy v2.0](https://docs.platformio.org/en/stable/boards/espressif32/watchy.html)
