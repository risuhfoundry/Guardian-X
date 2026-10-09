#pragma once

#include <stddef.h>
#include <stdint.h>

#include "guardian_x/model.h"

namespace guardian_x {

constexpr uint16_t kStorageFormatVersion = 2;
constexpr size_t kStorageSlotCount = 2;

struct StorageHeader {
  char magic[4];
  uint16_t formatVersion;
  uint16_t reserved;
  uint32_t generation;
  uint32_t payloadSize;
  uint32_t payloadCrc32;
};

// This interface deliberately knows nothing about LittleFS, the SEN0677, or
// Arduino.  It allows the core to be tested without hardware and keeps all
// ESP32 filesystem behavior in a narrow adapter.
class StorageBackend {
 public:
  virtual ~StorageBackend() = default;

  virtual bool begin() = 0;
  virtual bool slotExists(uint8_t slot) const = 0;
  // The payload is byte-addressed so DeviceCore can migrate a prior,
  // versioned snapshot without giving the LittleFS adapter domain knowledge.
  virtual bool readSlot(uint8_t slot, StorageHeader *header, void *payload,
                        size_t payloadSize) = 0;
  virtual bool writeSlot(uint8_t slot, const StorageHeader &header,
                         const void *payload, size_t payloadSize) = 0;
};

uint32_t crc32(const uint8_t *data, size_t length);

}  // namespace guardian_x
