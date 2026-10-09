#include "guardian_x/admin_credential_storage.h"

#include <LittleFS.h>

namespace guardian_x {
namespace {

bool removeIfPresent(const char *filePath) {
  if (!LittleFS.exists(filePath)) {
    return true;
  }
  return LittleFS.remove(filePath) && !LittleFS.exists(filePath);
}

}  // namespace

const char *AdminCredentialStorage::path() {
  return "/guardian-x-admin.bin";
}

const char *AdminCredentialStorage::temporaryPath() {
  return "/guardian-x-admin.tmp";
}

bool AdminCredentialStorage::begin() {
  // Mirrors LittleFsStorage::begin(): never format implicitly.  A mount
  // failure is reported truthfully; the caller decides on recovery.
  mounted_ = LittleFS.begin(false);
  return mounted_;
}

bool AdminCredentialStorage::exists() {
  return mounted_ && LittleFS.exists(path());
}

bool AdminCredentialStorage::load(AdminCredentialRecord *record) {
  if (!mounted_ || record == nullptr) {
    return false;
  }
  File file = LittleFS.open(path(), FILE_READ);
  if (!file) {
    return false;
  }
  if (file.size() != sizeof(AdminCredentialRecord)) {
    file.close();
    return false;
  }
  const size_t read = file.read(reinterpret_cast<uint8_t *>(record),
                                sizeof(AdminCredentialRecord));
  file.close();
  return read == sizeof(AdminCredentialRecord);
}

bool AdminCredentialStorage::save(const AdminCredentialRecord &record) {
  if (!mounted_ || !removeIfPresent(temporaryPath())) {
    return false;
  }

  File file = LittleFS.open(temporaryPath(), FILE_WRITE);
  if (!file) {
    return false;
  }
  const size_t written = file.write(
      reinterpret_cast<const uint8_t *>(&record), sizeof(AdminCredentialRecord));
  file.flush();
  file.close();

  if (written != sizeof(AdminCredentialRecord)) {
    removeIfPresent(temporaryPath());
    return false;
  }

  // LittleFS replaces an existing file atomically as part of rename().  Keep
  // the old credential in place until the new record is fully closed and the
  // rename commits, so a failed update leaves the previous credential usable.
  if (!LittleFS.rename(temporaryPath(), path())) {
    removeIfPresent(temporaryPath());
    return false;
  }
  return true;
}

bool AdminCredentialStorage::erase() {
  if (!mounted_) {
    return false;
  }
  // Idempotent: absent files are already erased.  Propagate IO failures so
  // recovery and factory-reset commands cannot report success prematurely.
  return removeIfPresent(path()) && removeIfPresent(temporaryPath());
}

}  // namespace guardian_x
