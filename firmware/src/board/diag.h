// Diagnostics that compile to nothing in a release build.
//
// Serial output is not free: the UART clock stays up, the strings occupy flash,
// and printf formatting burns CPU cycles during the wake we are trying to keep as
// short as possible. WORKADAY_DIAG is 0 in platformio.ini; flip it to 1 for
// bring-up and debugging.
#pragma once

#include <Arduino.h>

#ifndef WORKADAY_DIAG
#define WORKADAY_DIAG 0
#endif

#if WORKADAY_DIAG
#define WD_DIAG_BEGIN()   \
  do {                    \
    Serial.begin(115200); \
  } while (0)
#define WD_DIAG_FLUSH() Serial.flush()
#define WD_LOG(fmt, ...) Serial.printf("[%8lu] " fmt "\r\n", millis(), ##__VA_ARGS__)
#else
#define WD_DIAG_BEGIN() \
  do {                  \
  } while (0)
#define WD_DIAG_FLUSH() \
  do {                  \
  } while (0)
#define WD_LOG(fmt, ...) \
  do {                   \
  } while (0)
#endif
