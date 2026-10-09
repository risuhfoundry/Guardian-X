#pragma once

// Device-only adapters over ESP-IDF facilities.  This header is included only
// by the firmware (main.cpp) and is guarded so host-side code never sees it.

#if defined(ARDUINO) && defined(ESP32)

#include <Arduino.h>
#include <esp_random.h>

#include <stddef.h>
#include <stdint.h>

#include "guardian_x/security.h"

namespace guardian_x {

// ESP32-S3 hardware true random number generator.  Session tokens and
// password salts are always drawn from the RNG peripheral; there is no
// software pseudo-random fallback.
class EspSecureRandom final : public SecureRandom {
 public:
  bool fill(uint8_t *buffer, size_t length) override {
    if (buffer == nullptr) {
      return false;
    }
    esp_fill_random(buffer, length);
    return true;
  }
};

// Session lifetimes are measured in monotonically increasing milliseconds
// since boot, so sessions survive neither reboot nor clock wrap assumptions.
class MillisTimeSource final : public TimeSource {
 public:
  uint32_t nowMs() override { return static_cast<uint32_t>(millis()); }
};

}  // namespace guardian_x

#endif  // ARDUINO && ESP32
