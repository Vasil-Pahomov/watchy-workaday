#include "core/ui_state.h"

namespace core {

uint8_t activatedMenuItem(const UiState& state, ButtonId button) {
  // The one transition that enters an app. Anything else — navigating the menu,
  // backing out, a press inside an app already open, no press at all — activates
  // nothing, which is what keeps a screen left open from re-asking for a radio
  // window on every subsequent wake.
  if (state.screen != Screen::Menu || button != ButtonId::Menu) {
    return kNoMenuItem;
  }
  // handleButton() keeps menu_index inside the menu, but this value has been
  // through deep sleep and a reflash since it was last written.
  return state.menu_index < kMenuItemCount ? state.menu_index : kNoMenuItem;
}

bool handleButton(UiState& state, ButtonId button) {
  if (button == ButtonId::None) {
    return false;
  }

  state.idle_minutes = 0;

  switch (state.screen) {
    case Screen::Watchface:
      if (button == ButtonId::Menu) {
        state.screen = Screen::Menu;
        state.menu_index = 0;
        return true;
      }
      // Up/Down/Back on the watchface are intentionally inert: a wake that
      // changes nothing should cost nothing beyond the wake itself.
      return false;

    case Screen::Menu:
      switch (button) {
        case ButtonId::Up:
          state.menu_index =
              static_cast<uint8_t>((state.menu_index + kMenuItemCount - 1) % kMenuItemCount);
          return true;
        case ButtonId::Down:
          state.menu_index = static_cast<uint8_t>((state.menu_index + 1) % kMenuItemCount);
          return true;
        case ButtonId::Menu:
          state.screen = Screen::App;
          state.app_index = state.menu_index;
          return true;
        case ButtonId::Back:
          state.screen = Screen::Watchface;
          state.menu_index = 0;
          return true;
        case ButtonId::None:
          return false;
      }
      return false;

    case Screen::App:
      if (button == ButtonId::Back) {
        state.screen = Screen::Menu;
        return true;
      }
      // Other presses belong to the running app, which handles them itself.
      return false;
  }

  return false;
}

bool tickIdle(UiState& state, uint16_t minutes) {
  if (state.screen == Screen::Watchface) {
    state.idle_minutes = 0;
    return false;
  }

  const uint32_t accumulated = static_cast<uint32_t>(state.idle_minutes) + minutes;
  state.idle_minutes =
      accumulated > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(accumulated);

  if (state.idle_minutes >= kIdleTimeoutMinutes) {
    state.screen = Screen::Watchface;
    state.menu_index = 0;
    state.idle_minutes = 0;
    return true;
  }
  return false;
}

}  // namespace core
