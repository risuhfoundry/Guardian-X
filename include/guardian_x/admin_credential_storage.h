#pragma once

#include "guardian_x/security.h"

namespace guardian_x {

// LittleFS persistence for the single administrator credential record.
//
// The record is stored in its own file (/guardian-x-admin.bin), separate from
// the DeviceCore dual-slot snapshot, because it has an independent lifecycle:
// erasing the record must never touch application data, and snapshot schema
// migration must never touch security data.  It reuses the same LittleFS
// mount and the same atomic tmp+rename write pattern as the snapshot slots.
// No plaintext password and no session token ever reaches this file.
class AdminCredentialStorage final : public CredentialStore {
 public:
  AdminCredentialStorage() : mounted_(false) {}

  // Mounts LittleFS without formatting; false when the filesystem cannot be
  // mounted.  Safe to call again after DeviceCore already mounted it.
  bool begin();

  bool exists() override;
  bool load(AdminCredentialRecord *record) override;
  bool save(const AdminCredentialRecord &record) override;
  bool erase() override;

 private:
  static const char *path();
  static const char *temporaryPath();

  bool mounted_;
};

}  // namespace guardian_x
