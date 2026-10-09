#pragma once

#include "guardian_x/storage.h"

namespace guardian_x {

class LittleFsStorage final : public StorageBackend {
 public:
  bool begin() override;
  bool slotExists(uint8_t slot) const override;
  bool readSlot(uint8_t slot, StorageHeader *header, void *payload,
                size_t payloadSize) override;
  bool writeSlot(uint8_t slot, const StorageHeader &header,
                 const void *payload, size_t payloadSize) override;

  // Phase 3 recovery helpers, invoked only when no readable snapshot exists.
  // recoverWithFormat() remounts the filesystem and formats it when it cannot
  // be mounted at all (a blank factory-fresh partition, or one too damaged to
  // mount, holds no readable data).  eraseAllSlots() removes both snapshot
  // slots and any temporary files so DeviceCore can recreate a fresh
  // unprovisioned snapshot.
  bool recoverWithFormat();
  void eraseAllSlots();

 private:
  static const char *slotPath(uint8_t slot);
  static const char *temporaryPath(uint8_t slot);
};

}  // namespace guardian_x
