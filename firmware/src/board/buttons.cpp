#include "board/buttons.h"

#include <Arduino.h>

#include "board/board_v20.h"

namespace board {
namespace buttons {
namespace {

int pinFor(core::ButtonId button) {
  switch (button) {
    case core::ButtonId::Menu:
      return kPinButtonMenu;
    case core::ButtonId::Back:
      return kPinButtonBack;
    case core::ButtonId::Up:
      return kPinButtonUp;
    case core::ButtonId::Down:
      return kPinButtonDown;
    case core::ButtonId::None:
      break;
  }
  return -1;
}

}  // namespace

void configure() {
  // Plain INPUT on all four. The board pulls them; INPUT_PULLUP would be wrong on
  // 35 (no internal pull resistor exists there) and misleading on the rest.
  pinMode(kPinButtonMenu, INPUT);
  pinMode(kPinButtonBack, INPUT);
  pinMode(kPinButtonUp, INPUT);
  pinMode(kPinButtonDown, INPUT);
}

bool isPressed(core::ButtonId button) {
  const int pin = pinFor(button);
  if (pin < 0) {
    return false;
  }
  return digitalRead(pin) == HIGH;  // active high on v2.0
}

core::ButtonId pressed() {
  if (isPressed(core::ButtonId::Menu)) {
    return core::ButtonId::Menu;
  }
  if (isPressed(core::ButtonId::Back)) {
    return core::ButtonId::Back;
  }
  if (isPressed(core::ButtonId::Up)) {
    return core::ButtonId::Up;
  }
  if (isPressed(core::ButtonId::Down)) {
    return core::ButtonId::Down;
  }
  return core::ButtonId::None;
}

void attachPressInterrupt(core::ButtonId button, PressHandler handler) {
  const int pin = pinFor(button);
  if (pin < 0 || handler == nullptr) {
    return;
  }
  // pinMode() runs gpio_config(), which on an RTC-capable pin calls
  // rtc_gpio_deinit() and hands the pad back to the digital matrix. Without it
  // the pad is still where esp_sleep_enable_ext1_wakeup() left it — muxed to
  // the RTC domain — and gpio_intr_enable() below is armed on a line that never
  // changes. Plain INPUT: the board pulls these, and INPUT_PULLUP is wrong on 35
  // (no pull resistor exists there) and misleading on the rest.
  pinMode(pin, INPUT);
  // Active HIGH on v2.0, so a press is the rising edge. Bounce may deliver the
  // edge more than once; every consumer of this is idempotent by contract.
  attachInterrupt(digitalPinToInterrupt(pin), handler, RISING);
}

void detachPressInterrupt(core::ButtonId button) {
  const int pin = pinFor(button);
  if (pin < 0) {
    return;
  }
  detachInterrupt(digitalPinToInterrupt(pin));
}

}  // namespace buttons
}  // namespace board
