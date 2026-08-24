#include "board/power.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/soc.h>

#include "board/board_v20.h"
#include "board/diag.h"

namespace board {
namespace power {
namespace {

// Pins that must keep their configuration through the sleep transition: the two
// wake groups, the RTC alarm line, the display control lines (the panel is
// hibernating, not unpowered), the accelerometer interrupts, and the input-only
// ADC pin. Everything else is driven to INPUT so nothing floats or keeps driving a
// powered-down peripheral.
// Pins that must never be reconfigured, for a different reason from kLeaveAlone's:
// on the ESP32-PICO-D4 these carry the module's *embedded* SPI flash. GPIO 6..11
// are the usual SD_* bus; GPIO 16 and 17 are the PICO-D4's internal flash CS/CLK
// and are not led out of the module at all.
//
// GPIO_IS_VALID_GPIO() reports every one of them as an ordinary pin and the IDF
// will happily set them to INPUT. The failure that follows is deceptive: the code
// keeps running out of the instruction cache and only dies at the next cache miss,
// so the crash lands on a *different, later* pin each boot and reads as a flaky
// sleep bug rather than a pin bug. Observed as ESP_RST_INT_WDT ~300 ms into every
// sleep, on every wake, with the reported pin varying between boots.
//
// kLeaveAlone means "this pin has a job we configured". kFlashBus means "touching
// this pin ends the program". Kept apart so neither reason can be deleted by
// someone reasoning about the other.
constexpr uint64_t kFlashBus = (0x3FULL << 6) | (1ULL << 16) | (1ULL << 17);

// UART0 TX/RX, excluded only when diagnostics are compiled in. Quiescing GPIO 1
// mid-sweep kills the channel the diagnostics travel on, so every later log line
// vanishes and any fault after that point reads as "hung at pin 1". In a shipping
// build (WORKADAY_DIAG=0) the UART is dead weight and is quiesced like the rest.
#if WORKADAY_DIAG
constexpr uint64_t kDiagUart = (1ULL << 1) | (1ULL << 3);
#else
constexpr uint64_t kDiagUart = 0ULL;
#endif

// The ESP32's boot strapping pins that this board does not otherwise use. A third
// reason to never touch a pin, and it is the one that cost a watch.
//
// These are sampled by the ROM at every reset to decide how the chip comes up:
// GPIO 0 selects download mode versus SPI boot, GPIO 2 must not be high for
// download mode to be entered, and GPIO 15 (MTDO) gates the ROM boot log on
// U0TXD. At reset each has a defined internal pull (0 up, 2 down, 15 up).
// `pinMode(pin, INPUT)` **disables both pulls**, so the sweep replaced three
// defined strap levels with three floating ones — and then deepSleep() latched
// whatever they floated to into a hold that survives every reset but a power-on.
// A watch that latches GPIO 0 high can no longer be put into download mode, and
// the only recovery is disconnecting the battery, which on v2.0 means opening the
// case.
//
// Excluding them costs nothing. Their reset pulls are *more* defined than the
// floating input the sweep was giving them, so this is better for leakage as well
// as for recovery — there is no Law 1 trade-off to weigh here.
//
// The other two ESP32 straps have jobs on this board and are already in
// kLeaveAlone: GPIO 5 is the display CS and GPIO 12 (MTDI) is the accelerometer's
// INT2. GPIO 12 is the dangerous one of that pair — it selects VDD_SDIO, and high
// at reset means 1.8 V into the PICO-D4's 3.3 V embedded flash — so note that
// kLeaveAlone protects a pin from *this sweep* and not from the pad hold. See
// releaseSleepHold().
constexpr uint64_t kStrappingPins = (1ULL << 0) | (1ULL << 2) | (1ULL << 15);

constexpr uint64_t kLeaveAlone =
    kButtonWakeMask | kAccelWakeMask | (1ULL << kPinRtcInterrupt) |
    (1ULL << kPinAccelInt2) | (1ULL << kPinBatteryAdc) | (1ULL << kPinDisplayCs) |
    (1ULL << kPinDisplayDc) | (1ULL << kPinDisplayReset) | (1ULL << kPinDisplayBusy) |
    (1ULL << kPinSda) | (1ULL << kPinScl);

// Margin on the timer backstop. Long enough that it never pre-empts a healthy RTC
// alarm (which would double the wake rate and the battery drain), short enough
// that a dead alarm is noticed within one tick.
constexpr uint16_t kBackstopMarginSeconds = 30;

// Every pin the sweep must not reconfigure, for the three separate reasons above.
constexpr uint64_t kNeverTouch = kLeaveAlone | kFlashBus | kDiagUart | kStrappingPins;

// The nearest thing to a unit test this file can have. Nothing here is a decision,
// so there is nothing to lift into core/ and nothing a host test could drive — but
// the membership of the never-touch set is mechanical, and a mechanical claim can
// be checked at build time. These are what stops a future edit narrowing one of the
// masks and silently putting a strap or the flash bus back into the sweep.
static_assert((kNeverTouch & (1ULL << 0)) != 0, "GPIO 0 is the boot-mode strap");
static_assert((kNeverTouch & (1ULL << 2)) != 0, "GPIO 2 is a boot-mode strap");
static_assert((kNeverTouch & (1ULL << 15)) != 0, "GPIO 15 (MTDO) is a boot strap");
static_assert((kNeverTouch & (1ULL << 5)) != 0, "GPIO 5 is a strap and the display CS");
static_assert((kNeverTouch & (1ULL << 12)) != 0, "GPIO 12 (MTDI) selects VDD_SDIO");
constexpr uint64_t kEmbeddedFlashPins = (1ULL << 6) | (1ULL << 7) | (1ULL << 8) |
                                       (1ULL << 9) | (1ULL << 10) | (1ULL << 11) |
                                       (1ULL << 16) | (1ULL << 17);
static_assert((kNeverTouch & kEmbeddedFlashPins) == kEmbeddedFlashPins,
              "GPIO 6-11, 16 and 17 are the PICO-D4's embedded flash bus");

void quiesceGpios() {
  for (int pin = 0; pin < GPIO_NUM_MAX; ++pin) {
    if ((kNeverTouch >> pin) & 1ULL) {
      continue;
    }
    if (!GPIO_IS_VALID_GPIO(static_cast<gpio_num_t>(pin))) {
      continue;
    }
    pinMode(pin, INPUT);
  }
}

void driveMotorLow() {
  // Released from any hold first: a held pad ignores pinMode() and digitalWrite()
  // silently, so configuring before releasing would look like it worked and do
  // nothing. The pad floats for the few microseconds in between, which is far too
  // short to move a motor.
  const gpio_num_t motor = static_cast<gpio_num_t>(kPinVibrationMotor);
  gpio_hold_dis(motor);
  pinMode(kPinVibrationMotor, OUTPUT);
  digitalWrite(kPinVibrationMotor, LOW);
}

void silenceMotor() {
  // A motor left driven would run for the whole sleep. Drive it low and hold that
  // level through deep sleep, since the pin is otherwise tri-stated the moment the
  // chip sleeps and the driver input would float.
  driveMotorLow();
  gpio_hold_en(static_cast<gpio_num_t>(kPinVibrationMotor));
}

}  // namespace

void releaseSleepHold() {
  // **The counterpart to gpio_deep_sleep_hold_en() in deepSleep(), and the reason
  // this function exists as a named thing rather than a line inside another one.**
  //
  // The hold is not scoped to the sleep. The IDF header says so outright: the pad
  // state "will not change no matter how the internal core reset and system reset
  // triggered by watchdog time-out or Deep-sleep events". The enable lives in
  // RTC_CNTL, which only a power-on reset clears. So a firmware that enables the
  // hold and never releases it latches its pads permanently after the first sleep,
  // and every reset short of disconnecting the battery leaves them latched.
  //
  // On this board that is a bricking defect and not an inconvenience. The pads the
  // sweep leaves configured include U0TXD and U0RXD, so once they are held there is
  // no serial output and the ROM download loader cannot answer esptool — the
  // symptom is "No serial data received" through every reset strategy, including
  // holding EN low with IO0 forced. v2.0 has no power switch, so the recovery is
  // opening the case.
  //
  // Called first in setup(), ahead of even WD_DIAG_BEGIN(). That ordering is the
  // point rather than tidiness: Serial.begin() on a held pad succeeds and emits
  // nothing, so releasing after it would leave the diagnostics that report the
  // problem as the one thing still broken.

  // Two hold domains, and the supported API only half-covers one of them.
  //
  // gpio_deep_sleep_hold_dis() clears RTC_CNTL_DG_PAD_AUTOHOLD_EN and stops there
  // (hal/esp32/include/hal/gpio_ll.h). That disables the hold for the *next* sleep
  // and does not touch the latch already applied: RTC_CNTL_DG_PAD_AUTOHOLD is a
  // read-only status bit released by writing RTC_CNTL_CLR_DG_PAD_AUTOHOLD, and
  // RTC_CNTL_DG_PAD_FORCE_UNHOLD — whose reset default is 1 — is cleared by
  // gpio_deep_sleep_hold_en() and never restored. Calling the API alone would look
  // like a fix and leave the pads exactly as stuck. Both bits are written here, in
  // the register the enable path itself uses, which returns RTC_CNTL_DIG_ISO_REG to
  // its power-on state.
  gpio_deep_sleep_hold_dis();
  SET_PERI_REG_MASK(RTC_CNTL_DIG_ISO_REG,
                    RTC_CNTL_DG_PAD_FORCE_UNHOLD | RTC_CNTL_CLR_DG_PAD_AUTOHOLD);

  // The RTC pads hold separately, and silenceMotor() is not the release for them.
  // Its gpio_hold_dis() runs on the way *into* sleep, immediately before re-driving
  // the pin — so between a wake and the next sleep GPIO 13 is still latched, and
  // anything that tried to buzz the motor during a wake would be silently ignored.
  // Nothing drives it today; a vibrate-on-alarm feature would have found this the
  // hard way.
  //
  // Driven low rather than merely unheld, because the alternative to a held-low pad
  // is a floating one, and the motor driver's input floating for a whole wake is
  // worse than either. Explicit low is what the pin should read at rest anyway.
  driveMotorLow();
  // And every other RTC pad, in case one is ever held: GPIO 12 (MTDI) is the
  // accelerometer's INT2 *and* the VDD_SDIO strap, so a hold latched while the
  // BMA423 happened to be asserting it would put 1.8 V on the PICO-D4's 3.3 V
  // embedded flash at the next reset. Being in kLeaveAlone does not protect a pin
  // from a hold; only this does.
  rtc_gpio_force_hold_dis_all();
}

void startWatchdog() {
  // The Arduino core may already have initialised the TWDT; re-initialising simply
  // reconfigures it, so the return value carries no news worth acting on.
  esp_task_wdt_init(kWatchdogTimeoutSeconds, /*panic=*/true);
  esp_task_wdt_add(nullptr);
}

void feedWatchdog() { esp_task_wdt_reset(); }

core::ResetReason resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      return core::ResetReason::PowerOn;
    case ESP_RST_DEEPSLEEP:
      return core::ResetReason::DeepSleepWake;
    case ESP_RST_SW:
      return core::ResetReason::SoftwareRestart;
    case ESP_RST_PANIC:
      return core::ResetReason::Panic;
    case ESP_RST_TASK_WDT:
      return core::ResetReason::TaskWatchdog;
    case ESP_RST_INT_WDT:
      return core::ResetReason::IntWatchdog;
    case ESP_RST_WDT:
      return core::ResetReason::TaskWatchdog;
    case ESP_RST_BROWNOUT:
      return core::ResetReason::Brownout;
    case ESP_RST_EXT:
      return core::ResetReason::ExternalPin;
    case ESP_RST_UNKNOWN:
    case ESP_RST_SDIO:
    default:
      return core::ResetReason::Unknown;
  }
}

core::WakeSource wakeSource() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT0:
      return core::WakeSource::RtcAlarm;

    case ESP_SLEEP_WAKEUP_EXT1: {
      // Buttons and the accelerometer can share ext1, so ask which line fired
      // rather than assuming it was a button.
      const uint64_t status = esp_sleep_get_ext1_wakeup_status();
      if ((status & kAccelWakeMask) != 0 && (status & kButtonWakeMask) == 0) {
        return core::WakeSource::Accelerometer;
      }
      return core::WakeSource::Button;
    }

    case ESP_SLEEP_WAKEUP_TIMER:
      return core::WakeSource::Timer;

    case ESP_SLEEP_WAKEUP_UNDEFINED:
      // Not a wake at all — this was a reset. Whether that is benign depends on
      // the reset reason, which the health module judges.
      return resetReason() == core::ResetReason::PowerOn ? core::WakeSource::PowerOn
                                                         : core::WakeSource::Unknown;

    default:
      return core::WakeSource::Unknown;
  }
}

core::ButtonId wakeButton() {
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) {
    return core::ButtonId::None;
  }
  const uint64_t status = esp_sleep_get_ext1_wakeup_status();
  if (status & (1ULL << kPinButtonMenu)) {
    return core::ButtonId::Menu;
  }
  if (status & (1ULL << kPinButtonBack)) {
    return core::ButtonId::Back;
  }
  if (status & (1ULL << kPinButtonUp)) {
    return core::ButtonId::Up;
  }
  if (status & (1ULL << kPinButtonDown)) {
    return core::ButtonId::Down;
  }
  return core::ButtonId::None;
}

[[noreturn]] void deepSleep(uint16_t tick_seconds, bool accel_wake) {
  WD_LOG("sleep: tick=%us accel_wake=%d", static_cast<unsigned>(tick_seconds),
         accel_wake ? 1 : 0);
  WD_DIAG_FLUSH();

  silenceMotor();
  quiesceGpios();

  // ext0: the PCF8563 alarm, active LOW. The RTC's timer flag must already have
  // been cleared, or INT is still asserted and this wake fires the instant we
  // sleep — a spin that never rests.
  esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(kPinRtcInterrupt), 0);

  // ext1: buttons, active HIGH. v2.0 specific — do not copy an active-low board.
  uint64_t ext1_mask = kButtonWakeMask;
  if (accel_wake) {
    ext1_mask |= kAccelWakeMask;
  }
  esp_sleep_enable_ext1_wakeup(ext1_mask, ESP_EXT1_WAKEUP_ANY_HIGH);

  // Timer backstop. Turns "the RTC alarm never arrived" from a permanent freeze
  // into one late tick.
  const uint64_t backstop_us =
      (static_cast<uint64_t>(tick_seconds) + kBackstopMarginSeconds) * 1000000ULL;
  esp_sleep_enable_timer_wakeup(backstop_us);

  // Latches every pad at its current configuration so nothing floats or stops
  // driving a powered-down peripheral while the chip sleeps — the display control
  // lines in particular are outputs into a hibernating panel, and a floating CMOS
  // input there costs more than the hold saves.
  //
  // **This has a counterpart and the pair is not optional: board::power::
  // releaseSleepHold(), called first in setup().** The latch outlives the sleep and
  // every reset short of a power cycle, so an enable with no release is a one-way
  // door. Do not add another hold anywhere without adding its release there.
  gpio_deep_sleep_hold_en();
  esp_deep_sleep_start();

  // esp_deep_sleep_start() does not return. Unreachable, but the compiler cannot
  // prove that through the IDF declaration.
  for (;;) {
  }
}

}  // namespace power
}  // namespace board
