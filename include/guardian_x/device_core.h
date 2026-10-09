#pragma once

#include <stdint.h>

#include "guardian_x/model.h"
#include "guardian_x/storage.h"

namespace guardian_x {

enum class CoreResult : uint8_t {
  kOk = 0,
  kNotReady,
  kStorageUnavailable,
  kRecoveryRequired,
  kInvalidArgument,
  kNotFound,
  kDuplicate,
  kConflict,
  kCapacity,
  kIntegrityError,
  kUnsupportedFormat,
};

const char *coreResultText(CoreResult result);

// Storage- and hardware-independent local application core.  Callers supply
// timestamps from a future trusted clock layer; a zero timestamp explicitly
// means "time unavailable" during this pre-provisioning phase.
class DeviceCore {
 public:
  explicit DeviceCore(StorageBackend &storage);

  CoreResult begin();
  bool ready() const;
  uint32_t generation() const;
  const Snapshot &snapshot() const;
  bool isProvisioned() const;

  // Phase 3 provisioning helpers.  Network connection is deliberately not
  // attempted here; callers persist only after their hardware adapter reports
  // a real successful connection.
  static bool validateNetworkConfiguration(const char *ssid,
                                           const char *passphrase);
  CoreResult configureNetwork(const char *ssid, const char *passphrase);
  CoreResult clearNetworkConfiguration();

  CoreResult configure(const char *deviceLabel, const char *siteLabel,
                       int16_t timezoneOffsetMinutes);

  CoreResult createClass(const char *name, const char *section,
                         uint32_t *classId);
  CoreResult updateClass(uint32_t classId, const char *name,
                         const char *section, bool active);
  CoreResult removeClass(uint32_t classId);

  CoreResult createStudent(const char *admissionId, const char *name,
                           uint32_t classId, uint32_t *studentId);
  CoreResult updateStudent(uint32_t studentId, const char *admissionId,
                           const char *name, uint32_t classId,
                           StudentStatus status);
  CoreResult removeStudent(uint32_t studentId);

  CoreResult createGuardian(const char *name, const char *contact,
                            uint32_t *guardianId);
  CoreResult updateGuardian(uint32_t guardianId, const char *name,
                            const char *contact);
  CoreResult removeGuardian(uint32_t guardianId);

  CoreResult linkGuardianToStudent(uint32_t guardianId, uint32_t studentId);
  CoreResult unlinkGuardianFromStudent(uint32_t guardianId,
                                       uint32_t studentId);

  // The caller must supply a UID returned by a real SEN0677 enrollment flow.
  // This method performs no sensor I/O and makes no biometric claim.
  CoreResult registerGuardianPalm(uint32_t guardianId, uint16_t sensorUid,
                                 uint32_t registeredAt, uint32_t *palmId);
  CoreResult setGuardianPalmStatus(uint32_t palmId, PalmStatus status);
  CoreResult removeGuardianPalm(uint32_t palmId);

  CoreResult createDismissalRequest(uint32_t studentId,
                                    const char *requestedBy,
                                    uint32_t createdAt,
                                    uint32_t *requestId);
  CoreResult transitionDismissalRequest(uint32_t requestId,
                                        DismissalStatus status,
                                        uint32_t guardianId,
                                        uint32_t updatedAt);

  static bool validateSnapshot(const Snapshot &snapshot);

 private:
  CoreResult prepareEdit();
  CoreResult commitEdit();
  CoreResult allocateId(uint32_t *id);

  StorageBackend &storage_;
  Snapshot snapshot_;
  Snapshot working_;
  uint32_t generation_;
  uint8_t activeSlot_;
  bool ready_;
};

}  // namespace guardian_x
