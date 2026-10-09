#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "guardian_x/security.h"
#include "guardian_x/storage.h"

using guardian_x::AdminCredentialRecord;
using guardian_x::AuthzDecision;
using guardian_x::CredentialStore;
using guardian_x::Principal;
using guardian_x::SecureRandom;
using guardian_x::SecurityCore;
using guardian_x::SecurityResult;
using guardian_x::TimeSource;

namespace {

void assertSecurityResult(SecurityResult expected, SecurityResult actual) {
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected),
                          static_cast<uint8_t>(actual));
}

void assertDenied(AuthzDecision decision) {
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AuthzDecision::kDenied),
                          static_cast<uint8_t>(decision));
}

void assertAllowed(AuthzDecision decision) {
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AuthzDecision::kAllowed),
                          static_cast<uint8_t>(decision));
}

uint32_t recordCrc(const AdminCredentialRecord &record) {
  return guardian_x::crc32(reinterpret_cast<const uint8_t *>(&record),
                           offsetof(AdminCredentialRecord, payloadCrc32));
}

bool containsBytes(const uint8_t *haystack, size_t haystackLength,
                   const char *needle) {
  const size_t needleLength = strlen(needle);
  if (needleLength == 0 || needleLength > haystackLength) {
    return false;
  }
  for (size_t start = 0; start + needleLength <= haystackLength; ++start) {
    if (memcmp(haystack + start, needle, needleLength) == 0) {
      return true;
    }
  }
  return false;
}

// In-memory stand-in for AdminCredentialStorage.  Saves exactly what the
// core hands over, so tests can inspect the persisted bytes (for salt
// uniqueness and plaintext-absence checks) and inject failures.
class MemoryCredentialStore final : public CredentialStore {
 public:
  bool exists() override { return present_; }

  bool load(AdminCredentialRecord *record) override {
    if (!present_ || record == nullptr || failNextLoad_) {
      failNextLoad_ = false;
      return false;
    }
    memcpy(record, &record_, sizeof(record_));
    return true;
  }

  bool save(const AdminCredentialRecord &record) override {
    if (failNextSave_) {
      failNextSave_ = false;
      return false;
    }
    memcpy(&record_, &record, sizeof(record_));
    present_ = true;
    return true;
  }

  bool erase() override {
    if (failNextErase_) {
      failNextErase_ = false;
      return false;
    }
    present_ = false;
    return true;
  }

  // Bypass save() failure injection to plant crafted records.
  void plantRecord(const AdminCredentialRecord &record) {
    memcpy(&record_, &record, sizeof(record_));
    present_ = true;
  }

  void failNextSave() { failNextSave_ = true; }
  void failNextLoad() { failNextLoad_ = true; }
  void failNextErase() { failNextErase_ = true; }

  const AdminCredentialRecord &lastSaved() const { return record_; }

 private:
  AdminCredentialRecord record_;
  bool present_ = false;
  bool failNextSave_ = false;
  bool failNextLoad_ = false;
  bool failNextErase_ = false;
};

// Deterministic byte stream (test-only).  Real firmware uses the ESP32-S3
// hardware RNG; entropy itself is not something a unit test can assert.
class ScriptedRandom final : public SecureRandom {
 public:
  bool fill(uint8_t *buffer, size_t length) override {
    if (buffer == nullptr) {
      return false;
    }
    if (failNext_) {
      failNext_ = false;
      return false;
    }
    for (size_t index = 0; index < length; ++index) {
      buffer[index] = static_cast<uint8_t>(counter_++);
    }
    return true;
  }

  void failNext() { failNext_ = true; }

 private:
  uint32_t counter_ = 0;
  bool failNext_ = false;
};

class FakeTimeSource final : public TimeSource {
 public:
  uint32_t nowMs() override { return now_; }
  void advance(uint32_t milliseconds) { now_ += milliseconds; }
  void set(uint32_t milliseconds) { now_ = milliseconds; }

 private:
  uint32_t now_ = 1000;
};

const char *kUsername = "admin01";
const char *kPassword = "Sup3rSecret!pw";

struct Harness {
  MemoryCredentialStore store;
  ScriptedRandom random;
  FakeTimeSource time;
  SecurityCore core;

  Harness() : core(store, random, time) {}

  void initialize() {
    assertSecurityResult(
        SecurityResult::kOk, core.initializeAdmin(kUsername, kPassword));
  }

  void login(uint8_t token[guardian_x::kSessionTokenLength]) {
    assertSecurityResult(SecurityResult::kOk,
                         core.authenticate(kUsername, kPassword, token));
  }
};

// --- Administrator initialization ------------------------------------------

void test_fresh_core_is_uninitialized() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  TEST_ASSERT_TRUE(harness.core.ready());
  TEST_ASSERT_FALSE(harness.core.isAdminInitialized());
  TEST_ASSERT_FALSE(harness.store.exists());

  uint8_t token[guardian_x::kSessionTokenLength];
  assertSecurityResult(SecurityResult::kNotInitialized,
                       harness.core.authenticate(kUsername, kPassword, token));
}

void test_admin_initialization_succeeds() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  assertSecurityResult(
      SecurityResult::kOk, harness.core.initializeAdmin(kUsername, kPassword));

  TEST_ASSERT_TRUE(harness.core.isAdminInitialized());
  TEST_ASSERT_TRUE(harness.store.exists());

  const AdminCredentialRecord &record = harness.store.lastSaved();
  TEST_ASSERT_EQUAL_UINT8('G', record.magic[0]);
  TEST_ASSERT_EQUAL_UINT8('X', record.magic[1]);
  TEST_ASSERT_EQUAL_UINT8('A', record.magic[2]);
  TEST_ASSERT_EQUAL_UINT8('1', record.magic[3]);
  TEST_ASSERT_EQUAL_UINT8(guardian_x::kCredentialRecordFormatVersion,
                          record.formatVersion);
  TEST_ASSERT_EQUAL_UINT32(guardian_x::kDefaultPbkdf2Iterations,
                           record.iterations);
  TEST_ASSERT_EQUAL_STRING(kUsername, record.username);
  TEST_ASSERT_EQUAL_UINT32(recordCrc(record), record.payloadCrc32);

  uint8_t token[guardian_x::kSessionTokenLength];
  assertSecurityResult(
      SecurityResult::kOk, harness.core.authenticate(kUsername, kPassword, token));
}

void test_initialization_survives_reboot() {
  Harness first;
  assertSecurityResult(SecurityResult::kOk, first.core.begin());
  first.initialize();

  // A fresh core over the same store models a reboot.  The store, random
  // source and clock are shared so the persisted record is really reloaded.
  SecurityCore second(first.store, first.random, first.time);
  assertSecurityResult(SecurityResult::kOk, second.begin());
  TEST_ASSERT_TRUE(second.isAdminInitialized());

  uint8_t token[guardian_x::kSessionTokenLength];
  assertSecurityResult(SecurityResult::kOk,
                       second.authenticate(kUsername, kPassword, token));
  assertAllowed(second.authorize(token));
}

void test_repeated_initialization_is_rejected() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  harness.initialize();

  assertSecurityResult(SecurityResult::kAlreadyInitialized,
                       harness.core.initializeAdmin("intruder", "NewPass123"));

  // The stored record is unchanged: the original credential still works and
  // the replacement does not.
  uint8_t token[guardian_x::kSessionTokenLength];
  assertSecurityResult(
      SecurityResult::kOk, harness.core.authenticate(kUsername, kPassword, token));
  uint8_t other[guardian_x::kSessionTokenLength];
  assertSecurityResult(SecurityResult::kInvalidCredentials,
                       harness.core.authenticate("intruder", "NewPass123", other));
}

void test_invalid_initialization_inputs_are_rejected() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());

  char longUsername[guardian_x::kMaximumAdminUsernameLength + 2];
  memset(longUsername, 'u', sizeof(longUsername) - 1);
  longUsername[sizeof(longUsername) - 1] = '\0';
  char longPassword[guardian_x::kMaximumAdminPasswordLength + 2];
  memset(longPassword, 'p', sizeof(longPassword) - 1);
  longPassword[sizeof(longPassword) - 1] = '\0';
  char nonPrintable[] = "passwor\x01\x02d!!";

  assertSecurityResult(SecurityResult::kInvalidInput,
                       harness.core.initializeAdmin(nullptr, kPassword));
  assertSecurityResult(SecurityResult::kInvalidInput,
                       harness.core.initializeAdmin("", kPassword));
  assertSecurityResult(SecurityResult::kInvalidInput,
                       harness.core.initializeAdmin(longUsername, kPassword));
  assertSecurityResult(SecurityResult::kInvalidInput,
                       harness.core.initializeAdmin("has space", kPassword));
  assertSecurityResult(SecurityResult::kInvalidInput,
                       harness.core.initializeAdmin(kUsername, nullptr));
  assertSecurityResult(SecurityResult::kInvalidInput,
                       harness.core.initializeAdmin(kUsername, "short7!"));
  assertSecurityResult(SecurityResult::kInvalidInput,
                       harness.core.initializeAdmin(kUsername, longPassword));
  assertSecurityResult(SecurityResult::kInvalidInput,
                       harness.core.initializeAdmin(kUsername, nonPrintable));

  TEST_ASSERT_FALSE(harness.core.isAdminInitialized());
  TEST_ASSERT_FALSE(harness.store.exists());
}

void test_failed_persistence_keeps_device_uninitialized() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());

  harness.store.failNextSave();
  assertSecurityResult(SecurityResult::kStoreFailure,
                       harness.core.initializeAdmin(kUsername, kPassword));
  TEST_ASSERT_FALSE(harness.core.isAdminInitialized());
  TEST_ASSERT_FALSE(harness.store.exists());

  // After a reboot nothing exists either.
  Harness reloaded;
  assertSecurityResult(SecurityResult::kOk, reloaded.core.begin());
  TEST_ASSERT_FALSE(reloaded.core.isAdminInitialized());
  uint8_t token[guardian_x::kSessionTokenLength];
  assertSecurityResult(SecurityResult::kNotInitialized,
                       reloaded.core.authenticate(kUsername, kPassword, token));
}

void test_random_failure_blocks_initialization() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());

  harness.random.failNext();
  assertSecurityResult(SecurityResult::kRandomFailure,
                       harness.core.initializeAdmin(kUsername, kPassword));
  TEST_ASSERT_FALSE(harness.core.isAdminInitialized());

  // Once the random source recovers, initialization succeeds normally.
  harness.initialize();
}

// --- Password handling -----------------------------------------------------

void test_correct_and_incorrect_passwords() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  harness.initialize();

  uint8_t token[guardian_x::kSessionTokenLength];
  assertSecurityResult(
      SecurityResult::kOk, harness.core.authenticate(kUsername, kPassword, token));

  uint8_t other[guardian_x::kSessionTokenLength];
  assertSecurityResult(SecurityResult::kInvalidCredentials,
                       harness.core.authenticate(kUsername, "WrongPass1", other));
  assertSecurityResult(SecurityResult::kInvalidCredentials,
                       harness.core.authenticate("admin02", kPassword, other));
  assertSecurityResult(SecurityResult::kInvalidInput,
                       harness.core.authenticate(kUsername, kPassword, nullptr));
}

void test_password_change_verifies_current_password() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  harness.initialize();

  uint8_t token[guardian_x::kSessionTokenLength];
  harness.login(token);

  assertSecurityResult(SecurityResult::kInvalidCredentials,
                       harness.core.changePassword("NotThePass1", "NewPass123"));
  // A rejected change must not disturb the existing session.
  assertAllowed(harness.core.authorize(token));
  TEST_ASSERT_EQUAL_UINT8(1, harness.core.activeSessionCount());
}

void test_password_change_rejects_invalid_new_password() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  harness.initialize();

  assertSecurityResult(SecurityResult::kInvalidInput,
                       harness.core.changePassword(kPassword, "short"));
  assertSecurityResult(SecurityResult::kInvalidInput,
                       harness.core.changePassword(kPassword, nullptr));
}

void test_password_change_persists_and_invalidates_sessions() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  harness.initialize();

  uint8_t token[guardian_x::kSessionTokenLength];
  harness.login(token);

  assertSecurityResult(SecurityResult::kOk,
                       harness.core.changePassword(kPassword, "NewPass123"));
  assertDenied(harness.core.authorize(token));
  TEST_ASSERT_EQUAL_UINT8(0, harness.core.activeSessionCount());

  // Old password rejected, new password accepted, and the new verifier
  // survives a reboot.
  uint8_t other[guardian_x::kSessionTokenLength];
  assertSecurityResult(SecurityResult::kInvalidCredentials,
                       harness.core.authenticate(kUsername, kPassword, other));
  assertSecurityResult(SecurityResult::kOk, harness.core.authenticate(
                                                kUsername, "NewPass123", other));

  // The new verifier survives a reboot (same store, fresh core).
  SecurityCore reloaded(harness.store, harness.random, harness.time);
  assertSecurityResult(SecurityResult::kOk, reloaded.begin());
  assertSecurityResult(SecurityResult::kOk,
                       reloaded.authenticate(kUsername, "NewPass123", other));
}

void test_password_change_store_failure_keeps_old_credential() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  harness.initialize();

  uint8_t token[guardian_x::kSessionTokenLength];
  harness.login(token);

  harness.store.failNextSave();
  assertSecurityResult(SecurityResult::kStoreFailure,
                       harness.core.changePassword(kPassword, "NewPass123"));

  // Nothing changed: old password and the existing session remain valid.
  assertAllowed(harness.core.authorize(token));
  uint8_t other[guardian_x::kSessionTokenLength];
  assertSecurityResult(
      SecurityResult::kOk, harness.core.authenticate(kUsername, kPassword, other));
}

void test_salts_are_unique_per_credential_generation() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  harness.initialize();

  const AdminCredentialRecord before = harness.store.lastSaved();
  assertSecurityResult(SecurityResult::kOk,
                       harness.core.changePassword(kPassword, "NewPass123"));
  const AdminCredentialRecord after = harness.store.lastSaved();

  TEST_ASSERT_TRUE(memcmp(before.salt, after.salt,
                          guardian_x::kPasswordSaltLength) != 0);
  TEST_ASSERT_TRUE(memcmp(before.passwordHash, after.passwordHash,
                          guardian_x::kPasswordHashLength) != 0);
}

void test_plaintext_password_is_never_persisted() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  harness.initialize();

  const AdminCredentialRecord &record = harness.store.lastSaved();
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record);
  TEST_ASSERT_FALSE(containsBytes(bytes, sizeof(record), kPassword));
  // The username is not secret and is stored; the verifier must not hold the
  // password anywhere.
  TEST_ASSERT_FALSE(containsBytes(record.passwordHash,
                                  guardian_x::kPasswordHashLength, kPassword));
}

// --- Sessions --------------------------------------------------------------

void test_valid_session_authorizes_with_principal() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  harness.initialize();

  uint8_t token[guardian_x::kSessionTokenLength];
  harness.login(token);
  TEST_ASSERT_EQUAL_UINT8(1, harness.core.activeSessionCount());

  Principal principal;
  memset(&principal, 0, sizeof(principal));
  assertAllowed(harness.core.authorize(token, &principal));
  TEST_ASSERT_EQUAL_STRING(kUsername, principal.username);
  TEST_ASSERT_TRUE(principal.sessionId > 0);
  TEST_ASSERT_EQUAL_UINT32(1000, principal.authenticatedAtMs);
}

void test_unknown_and_malformed_tokens_are_rejected() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  harness.initialize();

  uint8_t token[guardian_x::kSessionTokenLength];
  harness.login(token);

  uint8_t unknown[guardian_x::kSessionTokenLength] = {0};
  assertDenied(harness.core.authorize(unknown));
  // Flip a single byte of the real token.
  uint8_t tampered[guardian_x::kSessionTokenLength];
  memcpy(tampered, token, sizeof(tampered));
  tampered[0] ^= 0x01;
  assertDenied(harness.core.authorize(tampered));
  assertDenied(harness.core.authorize(nullptr));

  // Two distinct logins must produce distinct tokens.
  uint8_t second[guardian_x::kSessionTokenLength];
  harness.login(second);
  TEST_ASSERT_TRUE(memcmp(token, second, sizeof(token)) != 0);
}

void test_expired_sessions_are_rejected() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  harness.initialize();

  uint8_t token[guardian_x::kSessionTokenLength];
  harness.login(token);
  assertAllowed(harness.core.authorize(token));

  harness.time.advance(guardian_x::kSessionLifetimeMs + 1);
  assertDenied(harness.core.authorize(token));
  // The expired slot was purged by the authorize scan.
  TEST_ASSERT_EQUAL_UINT8(0, harness.core.activeSessionCount());
}

void test_expire_sessions_purges_all_stale_slots() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  harness.initialize();

  uint8_t first[guardian_x::kSessionTokenLength];
  harness.login(first);
  harness.time.advance(1000);
  uint8_t second[guardian_x::kSessionTokenLength];
  harness.login(second);
  TEST_ASSERT_EQUAL_UINT8(2, harness.core.activeSessionCount());

  // Advancing the full lifetime would sit exactly on the second session's
  // expiry instant, so stop 500 ms short: the first session is then 500 ms
  // past its lifetime while the second still has 500 ms left.
  harness.time.advance(guardian_x::kSessionLifetimeMs - 500);
  harness.core.expireSessions();
  TEST_ASSERT_EQUAL_UINT8(1, harness.core.activeSessionCount());
  assertDenied(harness.core.authorize(first));
  assertAllowed(harness.core.authorize(second));
}

void test_logout_invalidates_the_session() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  harness.initialize();

  uint8_t token[guardian_x::kSessionTokenLength];
  harness.login(token);
  assertSecurityResult(SecurityResult::kOk, harness.core.logout(token));
  assertDenied(harness.core.authorize(token));
  TEST_ASSERT_EQUAL_UINT8(0, harness.core.activeSessionCount());

  // A second logout of the same token reports the session as unknown.
  assertSecurityResult(SecurityResult::kInvalidCredentials,
                       harness.core.logout(token));
}

void test_logout_all_invalidates_every_session() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  harness.initialize();

  uint8_t first[guardian_x::kSessionTokenLength];
  uint8_t second[guardian_x::kSessionTokenLength];
  harness.login(first);
  harness.login(second);
  TEST_ASSERT_EQUAL_UINT8(2, harness.core.activeSessionCount());

  assertSecurityResult(SecurityResult::kOk, harness.core.logoutAll());
  assertDenied(harness.core.authorize(first));
  assertDenied(harness.core.authorize(second));
  TEST_ASSERT_EQUAL_UINT8(0, harness.core.activeSessionCount());
}

void test_session_capacity_evicts_oldest() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  harness.initialize();

  uint8_t tokens[guardian_x::kMaximumSessions + 1]
                [guardian_x::kSessionTokenLength];
  for (uint8_t index = 0; index <= guardian_x::kMaximumSessions; ++index) {
    assertSecurityResult(
        SecurityResult::kOk,
        harness.core.authenticate(kUsername, kPassword, tokens[index]));
  }

  TEST_ASSERT_EQUAL_UINT8(guardian_x::kMaximumSessions,
                          harness.core.activeSessionCount());
  // The oldest session was evicted; the other four remain valid.
  assertDenied(harness.core.authorize(tokens[0]));
  for (uint8_t index = 1; index <= guardian_x::kMaximumSessions; ++index) {
    assertAllowed(harness.core.authorize(tokens[index]));
  }
}

void test_reboot_invalidates_all_sessions() {
  Harness first;
  assertSecurityResult(SecurityResult::kOk, first.core.begin());
  first.initialize();
  uint8_t token[guardian_x::kSessionTokenLength];
  first.login(token);
  assertAllowed(first.core.authorize(token));

  // New core (reboot) over the same store: the credential persists, the
  // in-memory session does not.
  SecurityCore second(first.store, first.random, first.time);
  assertSecurityResult(SecurityResult::kOk, second.begin());
  TEST_ASSERT_TRUE(second.isAdminInitialized());
  assertDenied(second.authorize(token));
  TEST_ASSERT_EQUAL_UINT8(0, second.activeSessionCount());
}

// --- Authorization ---------------------------------------------------------

void test_authorization_fails_closed_when_not_ready() {
  Harness harness;  // begin() deliberately not called.
  uint8_t token[guardian_x::kSessionTokenLength] = {0};

  assertDenied(harness.core.authorize(token));
  assertDenied(harness.core.authorize(nullptr));
  assertSecurityResult(SecurityResult::kNotReady,
                       harness.core.authenticate(kUsername, kPassword, token));
  assertSecurityResult(SecurityResult::kNotReady,
                       harness.core.logoutAll());
}

// --- Record integrity and recovery -----------------------------------------

void test_corrupt_records_are_rejected_and_recoverable() {
  // Bad CRC.
  {
    Harness seed;
    assertSecurityResult(SecurityResult::kOk, seed.core.begin());
    seed.initialize();
    AdminCredentialRecord record = seed.store.lastSaved();
    record.payloadCrc32 ^= 0x5A5A;
    seed.store.plantRecord(record);

    // A fresh core over the same store must reject the damaged record.
    SecurityCore reloaded(seed.store, seed.random, seed.time);
    assertSecurityResult(SecurityResult::kCorruptRecord, reloaded.begin());
    TEST_ASSERT_FALSE(reloaded.ready());

    // Recovery erases the bad record and returns to a clean, uninitialized
    // state without touching anything else.
    assertSecurityResult(SecurityResult::kOk,
                         reloaded.resetAdminCredentials());
    assertSecurityResult(SecurityResult::kOk, reloaded.begin());
    TEST_ASSERT_FALSE(reloaded.isAdminInitialized());
  }

  // Bad magic (with a corrected CRC, so only the magic check can fail).
  {
    Harness seed;
    assertSecurityResult(SecurityResult::kOk, seed.core.begin());
    seed.initialize();
    AdminCredentialRecord record = seed.store.lastSaved();
    record.magic[1] = 'B';
    record.payloadCrc32 = recordCrc(record);
    seed.store.plantRecord(record);

    // A fresh core over the same store must reject the damaged record.
    SecurityCore reloaded(seed.store, seed.random, seed.time);
    assertSecurityResult(SecurityResult::kCorruptRecord, reloaded.begin());
  }

  // Iteration count below the accepted floor (CRC corrected).
  {
    Harness seed;
    assertSecurityResult(SecurityResult::kOk, seed.core.begin());
    seed.initialize();
    AdminCredentialRecord record = seed.store.lastSaved();
    record.iterations = 100;
    record.payloadCrc32 = recordCrc(record);
    seed.store.plantRecord(record);

    // A fresh core over the same store must reject the damaged record.
    SecurityCore reloaded(seed.store, seed.random, seed.time);
    assertSecurityResult(SecurityResult::kCorruptRecord, reloaded.begin());
  }

  // Unsupported format version (CRC corrected).
  {
    Harness seed;
    assertSecurityResult(SecurityResult::kOk, seed.core.begin());
    seed.initialize();
    AdminCredentialRecord record = seed.store.lastSaved();
    record.formatVersion = 99;
    record.payloadCrc32 = recordCrc(record);
    seed.store.plantRecord(record);

    // A fresh core over the same store must reject the damaged record.
    SecurityCore reloaded(seed.store, seed.random, seed.time);
    assertSecurityResult(SecurityResult::kCorruptRecord, reloaded.begin());
  }

  // Store reports a file present but unreadable.
  {
    Harness seed;
    assertSecurityResult(SecurityResult::kOk, seed.core.begin());
    seed.initialize();
    seed.store.failNextLoad();

    // A fresh core over the same store must reject the damaged record.
    SecurityCore reloaded(seed.store, seed.random, seed.time);
    assertSecurityResult(SecurityResult::kCorruptRecord, reloaded.begin());
  }
}

void test_reset_admin_credentials_erases_only_credentials() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  harness.initialize();

  uint8_t token[guardian_x::kSessionTokenLength];
  harness.login(token);

  assertSecurityResult(SecurityResult::kOk,
                       harness.core.resetAdminCredentials());
  TEST_ASSERT_FALSE(harness.core.isAdminInitialized());
  TEST_ASSERT_FALSE(harness.store.exists());
  assertDenied(harness.core.authorize(token));

  // The device returns to a clean, re-initializable state.
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  TEST_ASSERT_FALSE(harness.core.isAdminInitialized());
  harness.initialize();
  TEST_ASSERT_TRUE(harness.core.isAdminInitialized());
}

void test_failed_credential_reset_revokes_sessions_and_fails_closed() {
  Harness harness;
  assertSecurityResult(SecurityResult::kOk, harness.core.begin());
  harness.initialize();

  uint8_t token[guardian_x::kSessionTokenLength];
  harness.login(token);
  harness.store.failNextErase();

  assertSecurityResult(SecurityResult::kStoreFailure,
                       harness.core.resetAdminCredentials());
  TEST_ASSERT_FALSE(harness.core.isAdminInitialized());
  TEST_ASSERT_EQUAL_UINT8(0, harness.core.activeSessionCount());
  assertDenied(harness.core.authorize(token));
  TEST_ASSERT_TRUE(harness.store.exists());

  // A reboot can still load the credential if the failed erase left the
  // durable record intact; the current instance remains fail-closed.
  SecurityCore reloaded(harness.store, harness.random, harness.time);
  assertSecurityResult(SecurityResult::kOk, reloaded.begin());
  TEST_ASSERT_TRUE(reloaded.isAdminInitialized());
}

}  // namespace

void setUp() {}
void tearDown() {}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_fresh_core_is_uninitialized);
  RUN_TEST(test_admin_initialization_succeeds);
  RUN_TEST(test_initialization_survives_reboot);
  RUN_TEST(test_repeated_initialization_is_rejected);
  RUN_TEST(test_invalid_initialization_inputs_are_rejected);
  RUN_TEST(test_failed_persistence_keeps_device_uninitialized);
  RUN_TEST(test_random_failure_blocks_initialization);
  RUN_TEST(test_correct_and_incorrect_passwords);
  RUN_TEST(test_password_change_verifies_current_password);
  RUN_TEST(test_password_change_rejects_invalid_new_password);
  RUN_TEST(test_password_change_persists_and_invalidates_sessions);
  RUN_TEST(test_password_change_store_failure_keeps_old_credential);
  RUN_TEST(test_salts_are_unique_per_credential_generation);
  RUN_TEST(test_plaintext_password_is_never_persisted);
  RUN_TEST(test_valid_session_authorizes_with_principal);
  RUN_TEST(test_unknown_and_malformed_tokens_are_rejected);
  RUN_TEST(test_expired_sessions_are_rejected);
  RUN_TEST(test_expire_sessions_purges_all_stale_slots);
  RUN_TEST(test_logout_invalidates_the_session);
  RUN_TEST(test_logout_all_invalidates_every_session);
  RUN_TEST(test_session_capacity_evicts_oldest);
  RUN_TEST(test_reboot_invalidates_all_sessions);
  RUN_TEST(test_authorization_fails_closed_when_not_ready);
  RUN_TEST(test_corrupt_records_are_rejected_and_recoverable);
  RUN_TEST(test_reset_admin_credentials_erases_only_credentials);
  RUN_TEST(test_failed_credential_reset_revokes_sessions_and_fails_closed);
  return UNITY_END();
}
