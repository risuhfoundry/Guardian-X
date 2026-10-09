#include "guardian_x/littlefs_storage.h"

#include <LittleFS.h>

namespace guardian_x {

const char *LittleFsStorage::slotPath(uint8_t slot) {
  return slot == 0 ? "/guardian-x-0.bin" : "/guardian-x-1.bin";
}

const char *LittleFsStorage::temporaryPath(uint8_t slot) {
  return slot == 0 ? "/guardian-x-0.tmp" : "/guardian-x-1.tmp";
}

bool LittleFsStorage::begin() {
  // Formatting automatically would turn a mount failure into silent data
  // loss.  A recovery-required result is safer and intentionally explicit;
  // recoverWithFormat() performs the deliberate format only after the boot
  // logic has concluded that no readable data can exist.
  return LittleFS.begin(false);
}

bool LittleFsStorage::recoverWithFormat() {
  return LittleFS.begin(true);
}

void LittleFsStorage::eraseAllSlots() {
  for (uint8_t slot = 0; slot < kStorageSlotCount; ++slot) {
    LittleFS.remove(slotPath(slot));
    LittleFS.remove(temporaryPath(slot));
  }
}

bool LittleFsStorage::slotExists(uint8_t slot) const {
  return slot < kStorageSlotCount && LittleFS.exists(slotPath(slot));
}

bool LittleFsStorage::readSlot(uint8_t slot, StorageHeader *header,
                               void *payload, size_t payloadSize) {
  if (slot >= kStorageSlotCount || header == nullptr || payload == nullptr ||
      payloadSize == 0) {
    return false;
  }

  File file = LittleFS.open(slotPath(slot), FILE_READ);
  if (!file) {
    return false;
  }

  const size_t headerRead =
      file.read(reinterpret_cast<uint8_t *>(header), sizeof(StorageHeader));
  if (headerRead != sizeof(StorageHeader) ||
      header->payloadSize != payloadSize ||
      file.size() != sizeof(StorageHeader) + payloadSize) {
    if (file) {
      file.close();
    }
    return false;
  }

  const size_t payloadRead =
      file.read(reinterpret_cast<uint8_t *>(payload), payloadSize);
  file.close();
  return payloadRead == payloadSize;
}

bool LittleFsStorage::writeSlot(uint8_t slot, const StorageHeader &header,
                                const void *payload, size_t payloadSize) {
  if (slot >= kStorageSlotCount || payload == nullptr || payloadSize == 0 ||
      header.payloadSize != payloadSize) {
    return false;
  }

  const char *temporary = temporaryPath(slot);
  const char *destination = slotPath(slot);
  LittleFS.remove(temporary);

  File file = LittleFS.open(temporary, FILE_WRITE);
  if (!file) {
    return false;
  }

  const size_t headerWritten = file.write(
      reinterpret_cast<const uint8_t *>(&header), sizeof(StorageHeader));
  const size_t payloadWritten = file.write(
      reinterpret_cast<const uint8_t *>(payload), payloadSize);
  file.flush();
  file.close();

  if (headerWritten != sizeof(StorageHeader) || payloadWritten != payloadSize) {
    LittleFS.remove(temporary);
    return false;
  }

  // The other slot remains valid throughout this replacement.  If power is
  // lost after removing this older/newer slot but before rename, boot selects
  // the remaining CRC-valid snapshot instead of accepting partial data.
  LittleFS.remove(destination);
  if (!LittleFS.rename(temporary, destination)) {
    LittleFS.remove(temporary);
    return false;
  }
  return true;
}

}  // namespace guardian_x
