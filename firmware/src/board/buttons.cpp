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

}  // namespace buttons
}  // namespace board
