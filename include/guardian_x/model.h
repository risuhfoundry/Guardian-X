#pragma once

#include <stddef.h>
#include <stdint.h>

namespace guardian_x {

// Phase 2 keeps the device core allocation-free.  These bounds are part of
// the on-device storage format, so increasing one is a storage migration.
constexpr uint16_t kCurrentSchemaVersion = 2;
constexpr size_t kMaximumClasses = 32;
constexpr size_t kMaximumStudents = 128;
constexpr size_t kMaximumGuardians = 128;
constexpr size_t kMaximumGuardianPalms = 128;
constexpr size_t kMaximumGuardianStudentLinks = 256;
constexpr size_t kMaximumDismissalRequests = 128;
constexpr size_t kMaximumWifiSsidLength = 32;
constexpr size_t kMaximumWifiPassphraseLength = 63;

enum class StudentStatus : uint8_t {
  kActive = 1,
  kInactive = 2,
};

enum class PalmStatus : uint8_t {
  kActive = 1,
  kDisabled = 2,
};

enum class ProvisioningState : uint8_t {
  kUnprovisioned = 0,
  kProvisioned = 1,
};

// This enum represents persisted workflow state only.  Phase 2 does not
// perform palm verification or make approval decisions.
enum class DismissalStatus : uint8_t {
  kPending = 1,
  kApproved = 2,
  kRejected = 3,
  kCompleted = 4,
  kCancelled = 5,
};

// The Phase 2 layout is retained solely to migrate already-persisted
// snapshots.  It must remain byte-for-byte compatible with schema version 1.
struct DeviceConfigurationV1 {
  char deviceLabel[32];
  char siteLabel[48];
  int16_t timezoneOffsetMinutes;
  uint16_t reserved;
};

struct DeviceConfiguration {
  char deviceLabel[32];
  char siteLabel[48];
  int16_t timezoneOffsetMinutes;
  uint16_t reserved;

  // Credentials are supplied only through the local provisioning flow and
  // are never returned by that flow.  A blank passphrase represents an open
  // Wi-Fi network; a non-blank passphrase is validated as WPA2-length.
  char wifiSsid[kMaximumWifiSsidLength + 1];
  char wifiPassphrase[kMaximumWifiPassphraseLength + 1];
  uint8_t provisioningState;
  // Sized so the growth over DeviceConfigurationV1 stays a multiple of 4:
  // the V1-to-V2 migration shifts the record tail as one contiguous block
  // (see migrateSnapshotV1InPlace), which is only valid while the shift
  // introduces no padding between the counts and the record arrays.
  uint8_t provisioningReserved[5];
};

struct ClassRecord {
  uint32_t id;
  char name[32];
  char section[16];
  uint8_t active;
  uint8_t reserved[3];
};

struct StudentRecord {
  uint32_t id;
  uint32_t classId;
  char admissionId[24];
  char name[64];
  uint8_t status;
  uint8_t reserved[3];
};

struct GuardianRecord {
  uint32_t id;
  char name[64];
  char contact[32];
};

// Only the numeric identifier returned by the SEN0677 is stored here.  No
// biometric template or recognition result is persisted by the device core.
struct GuardianPalmRecord {
  uint32_t id;
  uint32_t guardianId;
  uint32_t registeredAt;
  uint16_t sensorUid;
  uint8_t status;
  uint8_t reserved;
};

struct GuardianStudentLink {
  uint32_t guardianId;
  uint32_t studentId;
};

struct DismissalRequest {
  uint32_t id;
  uint32_t studentId;
  // Zero means no guardian has been associated with this request.
  uint32_t guardianId;
  uint32_t createdAt;
  uint32_t updatedAt;
  char requestedBy[40];
  uint8_t status;
  uint8_t reserved[3];
};

// Retained only for deterministic Phase 2-to-Phase 3 migration.  All entity
// records are intentionally unchanged; Phase 3 adds only configuration data.
struct SnapshotV1 {
  uint16_t schemaVersion;
  uint16_t reserved;
  uint32_t nextId;
  DeviceConfigurationV1 configuration;

  uint16_t classCount;
  uint16_t studentCount;
  uint16_t guardianCount;
  uint16_t palmCount;
  uint16_t guardianStudentLinkCount;
  uint16_t dismissalRequestCount;

  ClassRecord classes[kMaximumClasses];
  StudentRecord students[kMaximumStudents];
  GuardianRecord guardians[kMaximumGuardians];
  GuardianPalmRecord palms[kMaximumGuardianPalms];
  GuardianStudentLink guardianStudentLinks[kMaximumGuardianStudentLinks];
  DismissalRequest dismissalRequests[kMaximumDismissalRequests];
};

struct Snapshot {
  uint16_t schemaVersion;
  uint16_t reserved;
  uint32_t nextId;
  DeviceConfiguration configuration;

  uint16_t classCount;
  uint16_t studentCount;
  uint16_t guardianCount;
  uint16_t palmCount;
  uint16_t guardianStudentLinkCount;
  uint16_t dismissalRequestCount;

  ClassRecord classes[kMaximumClasses];
  StudentRecord students[kMaximumStudents];
  GuardianRecord guardians[kMaximumGuardians];
  GuardianPalmRecord palms[kMaximumGuardianPalms];
  GuardianStudentLink guardianStudentLinks[kMaximumGuardianStudentLinks];
  DismissalRequest dismissalRequests[kMaximumDismissalRequests];
};

}  // namespace guardian_x
