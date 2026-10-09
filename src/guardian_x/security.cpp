#include "guardian_x/security.h"

#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

#include <stddef.h>
#include <string.h>

#include "guardian_x/storage.h"

namespace guardian_x {

namespace {

bool isPrintable(uint8_t c) { return c >= 0x20 && c <= 0x7E; }
bool isPrintableNoSpace(uint8_t c) { return c >= 0x21 && c <= 0x7E; }

}  // namespace

const char *securityResultText(SecurityResult result) {
  switch (result) {
    case SecurityResult::kOk:
      return "ok";
    case SecurityResult::kNotReady:
      return "not ready";
    case SecurityResult::kAlreadyInitialized:
      return "already initialized";
    case SecurityResult::kNotInitialized:
      return "not initialized";
    case SecurityResult::kInvalidInput:
      return "invalid input";
    case SecurityResult::kInvalidCredentials:
      return "invalid credentials";
    case SecurityResult::kStoreFailure:
      return "storage failure";
    case SecurityResult::kRandomFailure:
      return "random source failure";
    case SecurityResult::kCryptoFailure:
      return "cryptographic failure";
    case SecurityResult::kCorruptRecord:
      return "corrupt credential record";
  }
  return "unknown";
}

bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t length) {
  if (a == nullptr || b == nullptr) {
    return false;
  }
  uint8_t difference = 0;
  for (size_t index = 0; index < length; ++index) {
    difference |= a[index] ^ b[index];
  }
  return difference == 0;
}

bool validateAdminUsername(const char *username) {
  if (username == nullptr) {
    return false;
  }
  const size_t length = strlen(username);
  if (length == 0 || length > kMaximumAdminUsernameLength) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    if (!isPrintableNoSpace(static_cast<uint8_t>(username[index]))) {
      return false;
    }
  }
  return true;
}

bool validateAdminPassword(const char *password) {
  if (password == nullptr) {
    return false;
  }
  const size_t length = strlen(password);
  if (length < kMinimumAdminPasswordLength ||
      length > kMaximumAdminPasswordLength) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    if (!isPrintable(static_cast<uint8_t>(password[index]))) {
      return false;
    }
  }
  return true;
}

SecurityCore::SecurityCore(CredentialStore &store, SecureRandom &random,
                           TimeSource &time)
    : store_(store),
      random_(random),
      time_(time),
      ready_(false),
      recordLoaded_(false),
      nextSessionId_(1) {
  memset(sessions_, 0, sizeof(sessions_));
  memset(&record_, 0, sizeof(record_));
}

SecurityResult SecurityCore::begin() {
  ready_ = false;
  recordLoaded_ = false;
  nextSessionId_ = 1;
  memset(sessions_, 0, sizeof(sessions_));
  memset(&record_, 0, sizeof(record_));

  if (!store_.exists()) {
    // No record: the device simply has no administrator yet.
    ready_ = true;
    return SecurityResult::kOk;
  }
  if (!store_.load(&record_)) {
    return SecurityResult::kCorruptRecord;
  }
  if (!validateRecord(record_)) {
    return SecurityResult::kCorruptRecord;
  }
  recordLoaded_ = true;
  ready_ = true;
  return SecurityResult::kOk;
}

bool SecurityCore::ready() const { return ready_; }

bool SecurityCore::isAdminInitialized() const {
  return ready_ && recordLoaded_;
}

SecurityResult SecurityCore::initializeAdmin(const char *username,
                                             const char *password) {
  if (!ready_) {
    return SecurityResult::kNotReady;
  }
  if (recordLoaded_) {
    return SecurityResult::kAlreadyInitialized;
  }
  if (!validateAdminUsername(username) || !validateAdminPassword(password)) {
    return SecurityResult::kInvalidInput;
  }

  AdminCredentialRecord record;
  memset(&record, 0, sizeof(record));
  memcpy(record.magic, "GXA1", 4);
  record.formatVersion = kCredentialRecordFormatVersion;
  memcpy(record.username, username, strlen(username));
  record.iterations = kDefaultPbkdf2Iterations;
  if (!random_.fill(record.salt, kPasswordSaltLength)) {
    return SecurityResult::kRandomFailure;
  }
  if (!deriveHash(password, record.salt, record.iterations,
                  record.passwordHash)) {
    return SecurityResult::kCryptoFailure;
  }
  record.payloadCrc32 = computeRecordCrc(record);

  if (!store_.save(record)) {
    return SecurityResult::kStoreFailure;
  }
  record_ = record;
  recordLoaded_ = true;
  return SecurityResult::kOk;
}

SecurityResult SecurityCore::authenticate(const char *username,
                                          const char *password,
                                          uint8_t *token) {
  if (!ready_) {
    return SecurityResult::kNotReady;
  }
  if (!recordLoaded_) {
    return SecurityResult::kNotInitialized;
  }
  if (username == nullptr || password == nullptr || token == nullptr) {
    return SecurityResult::kInvalidInput;
  }
  const size_t passwordLength = strlen(password);
  if (passwordLength == 0 || passwordLength > kMaximumAdminPasswordLength ||
      strlen(username) > kMaximumAdminUsernameLength) {
    return SecurityResult::kInvalidInput;
  }

  // Derive first so the time spent does not reveal which check failed.
  uint8_t candidate[kPasswordHashLength];
  if (!deriveHash(password, record_.salt, record_.iterations, candidate)) {
    return SecurityResult::kCryptoFailure;
  }
  const bool passwordMatches =
      constantTimeEqual(candidate, record_.passwordHash, kPasswordHashLength);
  const bool userMatches = usernameMatches(username, record_);
  if (!passwordMatches || !userMatches) {
    return SecurityResult::kInvalidCredentials;
  }
  return createSession(token);
}

AuthzDecision SecurityCore::authorize(const uint8_t *token,
                                      Principal *principal) {
  // Fail closed: any invalid state denies.
  if (!ready_ || !recordLoaded_ || token == nullptr) {
    return AuthzDecision::kDenied;
  }
  const int slot = findSession(token);
  if (slot < 0) {
    return AuthzDecision::kDenied;
  }
  if (principal != nullptr) {
    const size_t length = strnlen(record_.username, kMaximumAdminUsernameLength);
    memcpy(principal->username, record_.username, length);
    principal->username[length] = '\0';
    principal->sessionId = sessions_[slot].sessionId;
    principal->authenticatedAtMs = sessions_[slot].createdAtMs;
  }
  return AuthzDecision::kAllowed;
}

SecurityResult SecurityCore::logout(const uint8_t *token) {
  if (!ready_) {
    return SecurityResult::kNotReady;
  }
  if (token == nullptr) {
    return SecurityResult::kInvalidCredentials;
  }
  const int slot = findSession(token);
  if (slot < 0) {
    return SecurityResult::kInvalidCredentials;
  }
  sessions_[slot].active = false;
  return SecurityResult::kOk;
}

SecurityResult SecurityCore::logoutAll() {
  if (!ready_) {
    return SecurityResult::kNotReady;
  }
  for (uint8_t index = 0; index < kMaximumSessions; ++index) {
    sessions_[index].active = false;
  }
  return SecurityResult::kOk;
}

SecurityResult SecurityCore::changePassword(const char *currentPassword,
                                            const char *newPassword) {
  if (!ready_) {
    return SecurityResult::kNotReady;
  }
  if (!recordLoaded_) {
    return SecurityResult::kNotInitialized;
  }
  if (currentPassword == nullptr || newPassword == nullptr) {
    return SecurityResult::kInvalidInput;
  }
  if (!validateAdminPassword(newPassword)) {
    return SecurityResult::kInvalidInput;
  }
  const size_t currentLength = strlen(currentPassword);
  if (currentLength == 0 || currentLength > kMaximumAdminPasswordLength) {
    return SecurityResult::kInvalidInput;
  }

  uint8_t candidate[kPasswordHashLength];
  if (!deriveHash(currentPassword, record_.salt, record_.iterations,
                  candidate)) {
    return SecurityResult::kCryptoFailure;
  }
  if (!constantTimeEqual(candidate, record_.passwordHash,
                         kPasswordHashLength)) {
    return SecurityResult::kInvalidCredentials;
  }

  AdminCredentialRecord updated = record_;
  if (!random_.fill(updated.salt, kPasswordSaltLength)) {
    return SecurityResult::kRandomFailure;
  }
  if (!deriveHash(newPassword, updated.salt, updated.iterations,
                  updated.passwordHash)) {
    return SecurityResult::kCryptoFailure;
  }
  updated.payloadCrc32 = computeRecordCrc(updated);

  if (!store_.save(updated)) {
    return SecurityResult::kStoreFailure;
  }
  record_ = updated;
  for (uint8_t index = 0; index < kMaximumSessions; ++index) {
    sessions_[index].active = false;
  }
  return SecurityResult::kOk;
}

SecurityResult SecurityCore::resetAdminCredentials() {
  for (uint8_t index = 0; index < kMaximumSessions; ++index) {
    sessions_[index].active = false;
  }
  recordLoaded_ = false;
  memset(&record_, 0, sizeof(record_));
  // Revoke in-memory authority even when storage reports an erase failure.
  // The caller can retry or reboot to reload a still-present credential, but
  // no existing token remains usable after a recovery request.
  return store_.erase() ? SecurityResult::kOk : SecurityResult::kStoreFailure;
}

void SecurityCore::expireSessions() {
  if (!ready_) {
    return;
  }
  const uint32_t now = time_.nowMs();
  for (uint8_t index = 0; index < kMaximumSessions; ++index) {
    if (sessions_[index].active &&
        isExpired(sessions_[index].expiresAtMs, now)) {
      sessions_[index].active = false;
    }
  }
}

uint8_t SecurityCore::activeSessionCount() const {
  uint8_t count = 0;
  for (uint8_t index = 0; index < kMaximumSessions; ++index) {
    if (sessions_[index].active) {
      ++count;
    }
  }
  return count;
}

uint8_t SecurityCore::sessionCapacity() {
  return static_cast<uint8_t>(kMaximumSessions);
}

bool SecurityCore::validateRecord(const AdminCredentialRecord &record) const {
  if (record.magic[0] != 'G' || record.magic[1] != 'X' ||
      record.magic[2] != 'A' || record.magic[3] != '1') {
    return false;
  }
  if (record.formatVersion != kCredentialRecordFormatVersion) {
    return false;
  }
  if (record.iterations < kMinimumPbkdf2Iterations ||
      record.iterations > kMaximumPbkdf2Iterations) {
    return false;
  }
  // The fixed-width username field must hold printable characters up to its
  // first NUL, and only NULs from there on.
  bool terminated = false;
  for (size_t index = 0; index < kMaximumAdminUsernameLength; ++index) {
    const uint8_t c = static_cast<uint8_t>(record.username[index]);
    if (c == '\0') {
      terminated = true;
    } else if (terminated || !isPrintableNoSpace(c)) {
      return false;
    }
  }
  return record.payloadCrc32 == computeRecordCrc(record);
}

uint32_t SecurityCore::computeRecordCrc(const AdminCredentialRecord &record) {
  return crc32(reinterpret_cast<const uint8_t *>(&record),
               offsetof(AdminCredentialRecord, payloadCrc32));
}

bool SecurityCore::deriveHash(const char *password, const uint8_t *salt,
                              uint32_t iterations, uint8_t *out) {
  if (password == nullptr || salt == nullptr || out == nullptr) {
    return false;
  }
  // mbedTLS (ESP-IDF 2.28.x) PBKDF2-HMAC-SHA256.  This is the device
  // framework's maintained, hardware-accelerated implementation; no custom
  // cryptography is written here.
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr) {
    return false;
  }
  mbedtls_md_context_t context;
  mbedtls_md_init(&context);
  if (mbedtls_md_setup(&context, info, 1) != 0) {
    mbedtls_md_free(&context);
    return false;
  }
  const int result = mbedtls_pkcs5_pbkdf2_hmac(
      &context, reinterpret_cast<const unsigned char *>(password),
      strlen(password), salt, kPasswordSaltLength,
      static_cast<unsigned int>(iterations), kPasswordHashLength, out);
  mbedtls_md_free(&context);
  return result == 0;
}

bool SecurityCore::usernameMatches(const char *candidate,
                                   const AdminCredentialRecord &record) {
  if (candidate == nullptr) {
    return false;
  }
  const size_t length = strlen(candidate);
  if (length == 0 || length > kMaximumAdminUsernameLength) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    if (record.username[index] != candidate[index]) {
      return false;
    }
  }
  return length == kMaximumAdminUsernameLength ||
         record.username[length] == '\0';
}

bool SecurityCore::isExpired(uint32_t expiresAtMs, uint32_t nowMs) {
  // Wrap-safe: correct while the lifetime is far below 2^31 ms.
  return static_cast<int32_t>(expiresAtMs - nowMs) <= 0;
}

int SecurityCore::findSession(const uint8_t *token) {
  const uint32_t now = time_.nowMs();
  int match = -1;
  for (uint8_t index = 0; index < kMaximumSessions; ++index) {
    if (!sessions_[index].active) {
      continue;
    }
    if (isExpired(sessions_[index].expiresAtMs, now)) {
      sessions_[index].active = false;
      continue;
    }
    if (match < 0 &&
        constantTimeEqual(token, sessions_[index].token, kSessionTokenLength)) {
      match = index;
    }
  }
  return match;
}

SecurityResult SecurityCore::createSession(uint8_t *token) {
  const uint32_t now = time_.nowMs();

  // Reuse expired slots first, then evict the session with the earliest
  // expiry when every slot is live.
  int slot = -1;
  for (uint8_t index = 0; index < kMaximumSessions; ++index) {
    if (sessions_[index].active &&
        isExpired(sessions_[index].expiresAtMs, now)) {
      sessions_[index].active = false;
    }
    if (slot < 0 && !sessions_[index].active) {
      slot = index;
    }
  }
  if (slot < 0) {
    int oldest = 0;
    for (uint8_t index = 1; index < kMaximumSessions; ++index) {
      if (static_cast<int32_t>(sessions_[index].expiresAtMs -
                               sessions_[oldest].expiresAtMs) < 0) {
        oldest = index;
      }
    }
    sessions_[oldest].active = false;
    slot = oldest;
  }

  uint8_t tokenBytes[kSessionTokenLength];
  if (!random_.fill(tokenBytes, kSessionTokenLength)) {
    return SecurityResult::kRandomFailure;
  }

  Session &session = sessions_[slot];
  session.active = true;
  memcpy(session.token, tokenBytes, kSessionTokenLength);
  session.sessionId = nextSessionId_++;
  session.createdAtMs = now;
  session.expiresAtMs = now + kSessionLifetimeMs;
  memcpy(token, tokenBytes, kSessionTokenLength);
  return SecurityResult::kOk;
}

}  // namespace guardian_x
