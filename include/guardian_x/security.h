#pragma once

#include <stddef.h>
#include <stdint.h>

namespace guardian_x {

// ---------------------------------------------------------------------------
// Phase 4 security parameters.
// ---------------------------------------------------------------------------

// PBKDF2-HMAC-SHA256 parameters.  kDefaultPbkdf2Iterations is used for every
// newly stored credential; other values are only accepted when loading a
// record written by a different firmware version, within the bounds below.
constexpr size_t kPasswordSaltLength = 16;
constexpr size_t kPasswordHashLength = 32;
constexpr uint32_t kDefaultPbkdf2Iterations = 100000;
constexpr uint32_t kMinimumPbkdf2Iterations = 10000;
constexpr uint32_t kMaximumPbkdf2Iterations = 10000000;

constexpr size_t kMaximumAdminUsernameLength = 32;
constexpr size_t kMinimumAdminPasswordLength = 8;
constexpr size_t kMaximumAdminPasswordLength = 64;

// Sessions live in a fixed in-memory array (see SecurityCore).  A reboot
// invalidates every session; nothing session-related is persisted.
constexpr size_t kSessionTokenLength = 32;  // 256-bit bearer token
constexpr size_t kMaximumSessions = 4;
constexpr uint32_t kSessionLifetimeMs = 30u * 60u * 1000u;

constexpr uint8_t kCredentialRecordFormatVersion = 1;

// Persisted administrator credential record.  It intentionally lives in its
// own LittleFS file rather than the general DeviceCore snapshot: the snapshot
// is application data with a schema/migration path that must stay Phase 2/3
// compatible, while this record has an independent security lifecycle
// (credential recovery erases it without touching application data).
//
// The layout is padding-free by construction so the on-disk bytes are
// identical on the device (ESP32 GCC) and host (MSVC) builds; payloadCrc32
// covers every preceding byte.
struct AdminCredentialRecord {
  char magic[4];  // "GXA1"
  uint8_t formatVersion;
  uint8_t reserved[3];
  uint32_t iterations;  // PBKDF2 iteration count used for this credential
  uint8_t salt[kPasswordSaltLength];
  uint8_t passwordHash[kPasswordHashLength];
  // Fixed-width, zero-padded; guaranteed NUL-terminated only when the stored
  // username is shorter than the field.
  char username[kMaximumAdminUsernameLength];
  uint32_t payloadCrc32;
};

static_assert(sizeof(AdminCredentialRecord) == 96,
              "credential record layout must stay padding-free");

enum class SecurityResult : uint8_t {
  kOk = 0,
  kNotReady,
  kAlreadyInitialized,
  kNotInitialized,
  kInvalidInput,
  kInvalidCredentials,
  kStoreFailure,
  kRandomFailure,
  kCryptoFailure,
  kCorruptRecord,
};

const char *securityResultText(SecurityResult result);

// Authorization decision.  kDenied is the default for every error path.
enum class AuthzDecision : uint8_t {
  kDenied = 0,
  kAllowed = 1,
};

// Minimal authenticated-principal representation for future phases.  Carries
// no role: the device has exactly one administrator account.
struct Principal {
  char username[kMaximumAdminUsernameLength + 1];
  uint32_t sessionId;
  uint32_t authenticatedAtMs;
};

// Injectable boundaries so the security core stays testable without hardware.
class SecureRandom {
 public:
  virtual ~SecureRandom() = default;
  // Fills buffer with cryptographically secure random bytes.
  virtual bool fill(uint8_t *buffer, size_t length) = 0;
};

class TimeSource {
 public:
  virtual ~TimeSource() = default;
  virtual uint32_t nowMs() = 0;
};

class CredentialStore {
 public:
  virtual ~CredentialStore() = default;
  // True when a record file is present, regardless of whether it is valid.
  virtual bool exists() = 0;
  // False on any IO or size failure; does not validate record contents.
  virtual bool load(AdminCredentialRecord *record) = 0;
  // Atomic replace; false when the record was not durably stored.
  virtual bool save(const AdminCredentialRecord &record) = 0;
  // Idempotent: absence after the call is a successful erasure.
  virtual bool erase() = 0;
};

// Constant-time byte comparison; safe for hashes and session tokens.
bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t length);

// Usernames: 1..32 printable ASCII characters, no spaces (serial command
// parsing separates arguments on spaces).  Passwords: 8..64 printable ASCII
// characters, spaces allowed.
bool validateAdminUsername(const char *username);
bool validateAdminPassword(const char *password);

// Phase 4 security core: administrator credential lifecycle, PBKDF2
// verification, in-memory bearer sessions, and the fail-closed authorization
// boundary that Phase 5 HTTP routes must call.
//
// Reboot behavior: sessions live only in RAM, so every reboot invalidates all
// sessions.  The credential record itself survives reboot.
class SecurityCore {
 public:
  SecurityCore(CredentialStore &store, SecureRandom &random, TimeSource &time);

  SecurityResult begin();
  bool ready() const;
  bool isAdminInitialized() const;

  SecurityResult initializeAdmin(const char *username, const char *password);
  SecurityResult authenticate(const char *username, const char *password,
                              uint8_t sessionToken[kSessionTokenLength]);
  AuthzDecision authorize(const uint8_t sessionToken[kSessionTokenLength],
                          Principal *principal = nullptr);
  SecurityResult logout(const uint8_t sessionToken[kSessionTokenLength]);
  SecurityResult logoutAll();

  // Re-verifies the current password before applying the change, then
  // invalidates every session.
  SecurityResult changePassword(const char *currentPassword,
                                const char *newPassword);

  // Erases only the credential record (never application data) and
  // invalidates all sessions.  Works even when begin() failed, so a corrupt
  // record can always be recovered.
  SecurityResult resetAdminCredentials();

  // Called periodically; deactivates expired sessions.
  void expireSessions();

  uint8_t activeSessionCount() const;
  static uint8_t sessionCapacity();

 private:
  struct Session {
    bool active;
    uint8_t token[kSessionTokenLength];
    uint32_t sessionId;
    uint32_t createdAtMs;
    uint32_t expiresAtMs;
  };

  bool validateRecord(const AdminCredentialRecord &record) const;
  static uint32_t computeRecordCrc(const AdminCredentialRecord &record);
  static bool deriveHash(const char *password, const uint8_t *salt,
                         uint32_t iterations, uint8_t *out);
  static bool usernameMatches(const char *candidate,
                              const AdminCredentialRecord &record);
  static bool isExpired(uint32_t expiresAtMs, uint32_t nowMs);
  int findSession(const uint8_t *token);
  SecurityResult createSession(uint8_t sessionToken[kSessionTokenLength]);

  CredentialStore &store_;
  SecureRandom &random_;
  TimeSource &time_;
  Session sessions_[kMaximumSessions];
  AdminCredentialRecord record_;
  bool ready_;
  bool recordLoaded_;
  uint32_t nextSessionId_;
};

}  // namespace guardian_x
