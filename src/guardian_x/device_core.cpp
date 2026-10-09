#include "guardian_x/device_core.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

namespace guardian_x {
namespace {

constexpr char kStorageMagic[] = {'G', 'X', 'D', '3'};
constexpr char kLegacyStorageMagic[] = {'G', 'X', 'D', '2'};
constexpr uint16_t kLegacyStorageFormatVersion = 1;
constexpr uint16_t kLegacySchemaVersion = 1;

static_assert(sizeof(SnapshotV1) <= sizeof(Snapshot),
              "Phase 3 migration requires a non-shrinking snapshot");
// The in-place migration shifts the whole record tail in one memmove, which
// is only valid while the tail keeps an identical internal layout.  Any
// configuration growth that is not a multiple of the record alignment would
// insert padding between the counts and the record arrays and corrupt the
// migrated tail.
static_assert(offsetof(Snapshot, classCount) - offsetof(SnapshotV1, classCount) ==
                  sizeof(Snapshot) - sizeof(SnapshotV1),
              "Snapshot tail layout must stay contiguous for in-place migration");

bool isNonBlankText(const char *value, size_t capacity, bool required) {
  bool hasNonWhitespace = false;

  for (size_t index = 0; index < capacity; ++index) {
    const char character = value[index];
    if (character == '\0') {
      return !required || hasNonWhitespace;
    }
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte < 0x20 || byte == 0x7f) {
      return false;
    }
    if (character != ' ' && character != '\t') {
      hasNonWhitespace = true;
    }
  }

  return false;
}

bool copyText(char *destination, size_t capacity, const char *source,
              bool required) {
  if (source == nullptr) {
    return !required;
  }

  size_t length = 0;
  bool hasNonWhitespace = false;
  while (source[length] != '\0') {
    const unsigned char byte = static_cast<unsigned char>(source[length]);
    if (length + 1 >= capacity || byte < 0x20 || byte == 0x7f) {
      return false;
    }
    if (source[length] != ' ' && source[length] != '\t') {
      hasNonWhitespace = true;
    }
    ++length;
  }

  if (required && !hasNonWhitespace) {
    return false;
  }

  memset(destination, 0, capacity);
  if (length > 0) {
    memcpy(destination, source, length);
  }
  return true;
}

bool isStudentStatusValid(uint8_t value) {
  return value == static_cast<uint8_t>(StudentStatus::kActive) ||
         value == static_cast<uint8_t>(StudentStatus::kInactive);
}

bool isPalmStatusValid(uint8_t value) {
  return value == static_cast<uint8_t>(PalmStatus::kActive) ||
         value == static_cast<uint8_t>(PalmStatus::kDisabled);
}

bool isProvisioningStateValid(uint8_t value) {
  return value == static_cast<uint8_t>(ProvisioningState::kUnprovisioned) ||
         value == static_cast<uint8_t>(ProvisioningState::kProvisioned);
}

bool isWifiPassphraseValid(const char *value, size_t capacity) {
  size_t length = 0;
  while (length < capacity && value[length] != '\0') {
    ++length;
  }
  return length < capacity && (length == 0 || length >= 8);
}

bool hasValidNetworkConfiguration(const DeviceConfiguration &configuration) {
  return isNonBlankText(configuration.wifiSsid,
                        sizeof(configuration.wifiSsid), true) &&
         isNonBlankText(configuration.wifiPassphrase,
                        sizeof(configuration.wifiPassphrase), false) &&
         isWifiPassphraseValid(configuration.wifiPassphrase,
                               sizeof(configuration.wifiPassphrase));
}

bool isDismissalStatusValid(uint8_t value) {
  return value == static_cast<uint8_t>(DismissalStatus::kPending) ||
         value == static_cast<uint8_t>(DismissalStatus::kApproved) ||
         value == static_cast<uint8_t>(DismissalStatus::kRejected) ||
         value == static_cast<uint8_t>(DismissalStatus::kCompleted) ||
         value == static_cast<uint8_t>(DismissalStatus::kCancelled);
}

int findClassIndex(const Snapshot &snapshot, uint32_t id) {
  for (uint16_t index = 0; index < snapshot.classCount; ++index) {
    if (snapshot.classes[index].id == id) {
      return index;
    }
  }
  return -1;
}

int findStudentIndex(const Snapshot &snapshot, uint32_t id) {
  for (uint16_t index = 0; index < snapshot.studentCount; ++index) {
    if (snapshot.students[index].id == id) {
      return index;
    }
  }
  return -1;
}

int findGuardianIndex(const Snapshot &snapshot, uint32_t id) {
  for (uint16_t index = 0; index < snapshot.guardianCount; ++index) {
    if (snapshot.guardians[index].id == id) {
      return index;
    }
  }
  return -1;
}

int findPalmIndex(const Snapshot &snapshot, uint32_t id) {
  for (uint16_t index = 0; index < snapshot.palmCount; ++index) {
    if (snapshot.palms[index].id == id) {
      return index;
    }
  }
  return -1;
}

int findRequestIndex(const Snapshot &snapshot, uint32_t id) {
  for (uint16_t index = 0; index < snapshot.dismissalRequestCount; ++index) {
    if (snapshot.dismissalRequests[index].id == id) {
      return index;
    }
  }
  return -1;
}

bool linkExists(const Snapshot &snapshot, uint32_t guardianId, uint32_t studentId) {
  for (uint16_t index = 0; index < snapshot.guardianStudentLinkCount;
       ++index) {
    const GuardianStudentLink &link = snapshot.guardianStudentLinks[index];
    if (link.guardianId == guardianId && link.studentId == studentId) {
      return true;
    }
  }
  return false;
}

bool canTransition(DismissalStatus from, DismissalStatus to) {
  if (from == DismissalStatus::kPending) {
    return to == DismissalStatus::kApproved ||
           to == DismissalStatus::kRejected ||
           to == DismissalStatus::kCancelled;
  }
  if (from == DismissalStatus::kApproved) {
    return to == DismissalStatus::kCompleted ||
           to == DismissalStatus::kCancelled;
  }
  return false;
}

bool headerIsCurrent(const StorageHeader &header) {
  return memcmp(header.magic, kStorageMagic, sizeof(kStorageMagic)) == 0 &&
         header.formatVersion == kStorageFormatVersion &&
         header.payloadSize == sizeof(Snapshot);
}

bool headerIsLegacy(const StorageHeader &header) {
  return memcmp(header.magic, kLegacyStorageMagic,
                sizeof(kLegacyStorageMagic)) == 0 &&
         header.formatVersion == kLegacyStorageFormatVersion &&
         header.payloadSize == sizeof(SnapshotV1);
}

StorageHeader makeHeader(uint32_t generation, const Snapshot &snapshot) {
  StorageHeader header;
  memset(&header, 0, sizeof(header));
  memcpy(header.magic, kStorageMagic, sizeof(kStorageMagic));
  header.formatVersion = kStorageFormatVersion;
  header.generation = generation;
  header.payloadSize = sizeof(Snapshot);
  header.payloadCrc32 = crc32(reinterpret_cast<const uint8_t *>(&snapshot),
                              sizeof(snapshot));
  return header;
}

void makeEmptySnapshot(Snapshot *snapshot) {
  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->schemaVersion = kCurrentSchemaVersion;
  snapshot->nextId = 1;
  snapshot->configuration.provisioningState =
      static_cast<uint8_t>(ProvisioningState::kUnprovisioned);
}

bool migrateSnapshotV1InPlace(Snapshot *snapshot) {
  if (snapshot == nullptr) {
    return false;
  }

  uint16_t legacySchemaVersion = 0;
  memcpy(&legacySchemaVersion,
         reinterpret_cast<const uint8_t *>(snapshot) +
             offsetof(SnapshotV1, schemaVersion),
         sizeof(legacySchemaVersion));
  if (legacySchemaVersion != kLegacySchemaVersion) {
    return false;
  }

  DeviceConfigurationV1 legacyConfiguration;
  memcpy(&legacyConfiguration,
         reinterpret_cast<const uint8_t *>(snapshot) +
             offsetof(SnapshotV1, configuration),
         sizeof(legacyConfiguration));

  // The Phase 3 configuration is larger.  Move all Phase 2 counters and
  // records up before writing the new configuration fields, without placing a
  // full legacy snapshot on the ESP32 setup-task stack.
  const size_t legacyTailOffset = offsetof(SnapshotV1, classCount);
  const size_t currentTailOffset = offsetof(Snapshot, classCount);
  memmove(reinterpret_cast<uint8_t *>(snapshot) + currentTailOffset,
          reinterpret_cast<const uint8_t *>(snapshot) + legacyTailOffset,
          sizeof(SnapshotV1) - legacyTailOffset);

  snapshot->schemaVersion = kCurrentSchemaVersion;
  snapshot->reserved = 0;
  memset(&snapshot->configuration, 0, sizeof(snapshot->configuration));
  memcpy(snapshot->configuration.deviceLabel, legacyConfiguration.deviceLabel,
         sizeof(legacyConfiguration.deviceLabel));
  memcpy(snapshot->configuration.siteLabel, legacyConfiguration.siteLabel,
         sizeof(legacyConfiguration.siteLabel));
  snapshot->configuration.timezoneOffsetMinutes =
      legacyConfiguration.timezoneOffsetMinutes;
  snapshot->configuration.provisioningState =
      static_cast<uint8_t>(ProvisioningState::kUnprovisioned);
  return DeviceCore::validateSnapshot(*snapshot);
}

template <typename Record>
void eraseRecord(Record *records, uint16_t *count, uint16_t index) {
  for (uint16_t current = index; current + 1 < *count; ++current) {
    records[current] = records[current + 1];
  }
  --(*count);
  memset(&records[*count], 0, sizeof(Record));
}

}  // namespace

uint32_t crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = (crc & 1U) == 0 ? 0U : UINT32_MAX;
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return ~crc;
}

const char *coreResultText(CoreResult result) {
  switch (result) {
    case CoreResult::kOk:
      return "ok";
    case CoreResult::kNotReady:
      return "not ready";
    case CoreResult::kStorageUnavailable:
      return "storage unavailable";
    case CoreResult::kRecoveryRequired:
      return "storage recovery required";
    case CoreResult::kInvalidArgument:
      return "invalid argument";
    case CoreResult::kNotFound:
      return "not found";
    case CoreResult::kDuplicate:
      return "duplicate";
    case CoreResult::kConflict:
      return "relationship conflict";
    case CoreResult::kCapacity:
      return "capacity reached";
    case CoreResult::kIntegrityError:
      return "integrity error";
    case CoreResult::kUnsupportedFormat:
      return "unsupported storage format";
  }
  return "unknown";
}

DeviceCore::DeviceCore(StorageBackend &storage)
    : storage_(storage), generation_(0), activeSlot_(0), ready_(false) {
  memset(&snapshot_, 0, sizeof(snapshot_));
  memset(&working_, 0, sizeof(working_));
}

CoreResult DeviceCore::begin() {
  ready_ = false;
  generation_ = 0;
  activeSlot_ = 0;

  if (!storage_.begin()) {
    return CoreResult::kStorageUnavailable;
  }

  bool anySlotExists = false;
  bool sawUnsupportedFormat = false;
  bool foundValidSlot = false;
  bool selectedLegacySlot = false;
  StorageHeader selectedHeader;
  memset(&selectedHeader, 0, sizeof(selectedHeader));

  for (uint8_t slot = 0; slot < kStorageSlotCount; ++slot) {
    if (!storage_.slotExists(slot)) {
      continue;
    }
    anySlotExists = true;

    StorageHeader header;
    bool isLegacySlot = false;

    // Probe the current payload size first.  The storage adapter validates
    // the file length before copying, so corrupt or partial files are never
    // treated as a snapshot.
    if (storage_.readSlot(slot, &header, &working_, sizeof(working_))) {
      if (!headerIsCurrent(header) ||
          header.payloadCrc32 !=
              crc32(reinterpret_cast<const uint8_t *>(&working_),
                    sizeof(working_)) ||
          !validateSnapshot(working_)) {
        continue;
      }
    } else {
      // Phase 2 used the GXD2/v1 layout.  Reuse the pre-allocated working
      // buffer for a bounded, in-place migration rather than using another
      // full snapshot of RAM during boot.
      if (!storage_.readSlot(slot, &header, &working_, sizeof(SnapshotV1))) {
        const bool matchingMagic =
            memcmp(header.magic, kStorageMagic, sizeof(kStorageMagic)) == 0 ||
            memcmp(header.magic, kLegacyStorageMagic,
                   sizeof(kLegacyStorageMagic)) == 0;
        if (matchingMagic) {
          sawUnsupportedFormat = true;
        }
        continue;
      }
      if (!headerIsLegacy(header) ||
          header.payloadCrc32 !=
              crc32(reinterpret_cast<const uint8_t *>(&working_),
                    sizeof(SnapshotV1)) ||
          !migrateSnapshotV1InPlace(&working_)) {
        continue;
      }
      isLegacySlot = true;
    }

    if (!foundValidSlot || header.generation > selectedHeader.generation) {
      snapshot_ = working_;
      selectedHeader = header;
      activeSlot_ = slot;
      selectedLegacySlot = isLegacySlot;
      foundValidSlot = true;
    }
  }

  if (!foundValidSlot) {
    if (anySlotExists) {
      return sawUnsupportedFormat ? CoreResult::kUnsupportedFormat
                                  : CoreResult::kRecoveryRequired;
    }

    makeEmptySnapshot(&snapshot_);
    const StorageHeader header = makeHeader(1, snapshot_);
    if (!storage_.writeSlot(0, header, &snapshot_, sizeof(snapshot_))) {
      return CoreResult::kStorageUnavailable;
    }
    generation_ = header.generation;
    activeSlot_ = 0;
    ready_ = true;
    return CoreResult::kOk;
  }

  generation_ = selectedHeader.generation;
  if (selectedLegacySlot) {
    const uint8_t nextSlot = activeSlot_ == 0 ? 1 : 0;
    const StorageHeader migratedHeader = makeHeader(generation_ + 1, snapshot_);
    if (!storage_.writeSlot(nextSlot, migratedHeader, &snapshot_,
                            sizeof(snapshot_))) {
      return CoreResult::kStorageUnavailable;
    }
    generation_ = migratedHeader.generation;
    activeSlot_ = nextSlot;
  }
  ready_ = true;
  return CoreResult::kOk;
}

bool DeviceCore::ready() const { return ready_; }

uint32_t DeviceCore::generation() const { return generation_; }

const Snapshot &DeviceCore::snapshot() const { return snapshot_; }

bool DeviceCore::isProvisioned() const {
  return ready_ && snapshot_.configuration.provisioningState ==
                       static_cast<uint8_t>(ProvisioningState::kProvisioned);
}

bool DeviceCore::validateNetworkConfiguration(const char *ssid,
                                              const char *passphrase) {
  DeviceConfiguration candidate;
  memset(&candidate, 0, sizeof(candidate));
  return copyText(candidate.wifiSsid, sizeof(candidate.wifiSsid), ssid, true) &&
         copyText(candidate.wifiPassphrase, sizeof(candidate.wifiPassphrase),
                  passphrase, false) &&
         hasValidNetworkConfiguration(candidate);
}

CoreResult DeviceCore::configureNetwork(const char *ssid, const char *passphrase) {
  if (!validateNetworkConfiguration(ssid, passphrase)) {
    return CoreResult::kInvalidArgument;
  }
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  if (!copyText(working_.configuration.wifiSsid,
                sizeof(working_.configuration.wifiSsid), ssid, true) ||
      !copyText(working_.configuration.wifiPassphrase,
                sizeof(working_.configuration.wifiPassphrase), passphrase,
                false)) {
    return CoreResult::kInvalidArgument;
  }
  working_.configuration.provisioningState =
      static_cast<uint8_t>(ProvisioningState::kProvisioned);
  return commitEdit();
}

CoreResult DeviceCore::clearNetworkConfiguration() {
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  memset(working_.configuration.wifiSsid, 0,
         sizeof(working_.configuration.wifiSsid));
  memset(working_.configuration.wifiPassphrase, 0,
         sizeof(working_.configuration.wifiPassphrase));
  working_.configuration.provisioningState =
      static_cast<uint8_t>(ProvisioningState::kUnprovisioned);
  return commitEdit();
}

CoreResult DeviceCore::prepareEdit() {
  if (!ready_) {
    return CoreResult::kNotReady;
  }
  memcpy(&working_, &snapshot_, sizeof(snapshot_));
  return CoreResult::kOk;
}

CoreResult DeviceCore::commitEdit() {
  if (!validateSnapshot(working_)) {
    return CoreResult::kIntegrityError;
  }
  if (generation_ == UINT32_MAX) {
    return CoreResult::kCapacity;
  }

  const uint8_t nextSlot = activeSlot_ == 0 ? 1 : 0;
  const StorageHeader header = makeHeader(generation_ + 1, working_);
  if (!storage_.writeSlot(nextSlot, header, &working_, sizeof(working_))) {
    return CoreResult::kStorageUnavailable;
  }

  snapshot_ = working_;
  generation_ = header.generation;
  activeSlot_ = nextSlot;
  return CoreResult::kOk;
}

CoreResult DeviceCore::allocateId(uint32_t *id) {
  if (id == nullptr || working_.nextId == 0 ||
      working_.nextId == UINT32_MAX) {
    return CoreResult::kCapacity;
  }
  *id = working_.nextId++;
  return CoreResult::kOk;
}

CoreResult DeviceCore::configure(const char *deviceLabel, const char *siteLabel,
                                 int16_t timezoneOffsetMinutes) {
  if (timezoneOffsetMinutes < -720 || timezoneOffsetMinutes > 840) {
    return CoreResult::kInvalidArgument;
  }
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  if (!copyText(working_.configuration.deviceLabel,
                sizeof(working_.configuration.deviceLabel), deviceLabel, false) ||
      !copyText(working_.configuration.siteLabel,
                sizeof(working_.configuration.siteLabel), siteLabel, false)) {
    return CoreResult::kInvalidArgument;
  }
  working_.configuration.timezoneOffsetMinutes = timezoneOffsetMinutes;
  return commitEdit();
}

CoreResult DeviceCore::createClass(const char *name, const char *section,
                                   uint32_t *classId) {
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  if (working_.classCount >= kMaximumClasses) {
    return CoreResult::kCapacity;
  }

  ClassRecord record;
  memset(&record, 0, sizeof(record));
  if (!copyText(record.name, sizeof(record.name), name, true) ||
      !copyText(record.section, sizeof(record.section), section, true)) {
    return CoreResult::kInvalidArgument;
  }
  result = allocateId(&record.id);
  if (result != CoreResult::kOk) {
    return result;
  }
  record.active = 1;
  working_.classes[working_.classCount++] = record;
  result = commitEdit();
  if (result == CoreResult::kOk && classId != nullptr) {
    *classId = record.id;
  }
  return result;
}

CoreResult DeviceCore::updateClass(uint32_t classId, const char *name,
                                   const char *section, bool active) {
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  const int index = findClassIndex(working_, classId);
  if (index < 0) {
    return CoreResult::kNotFound;
  }
  ClassRecord &record = working_.classes[index];
  if (!copyText(record.name, sizeof(record.name), name, true) ||
      !copyText(record.section, sizeof(record.section), section, true)) {
    return CoreResult::kInvalidArgument;
  }
  record.active = active ? 1 : 0;
  return commitEdit();
}

CoreResult DeviceCore::removeClass(uint32_t classId) {
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  const int index = findClassIndex(working_, classId);
  if (index < 0) {
    return CoreResult::kNotFound;
  }
  for (uint16_t student = 0; student < working_.studentCount; ++student) {
    if (working_.students[student].classId == classId) {
      return CoreResult::kConflict;
    }
  }
  eraseRecord(working_.classes, &working_.classCount,
              static_cast<uint16_t>(index));
  return commitEdit();
}

CoreResult DeviceCore::createStudent(const char *admissionId, const char *name,
                                     uint32_t classId, uint32_t *studentId) {
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  if (working_.studentCount >= kMaximumStudents) {
    return CoreResult::kCapacity;
  }
  if (findClassIndex(working_, classId) < 0) {
    return CoreResult::kNotFound;
  }

  StudentRecord record;
  memset(&record, 0, sizeof(record));
  if (!copyText(record.admissionId, sizeof(record.admissionId), admissionId,
                true) ||
      !copyText(record.name, sizeof(record.name), name, true)) {
    return CoreResult::kInvalidArgument;
  }
  for (uint16_t index = 0; index < working_.studentCount; ++index) {
    if (strcmp(working_.students[index].admissionId, record.admissionId) == 0) {
      return CoreResult::kDuplicate;
    }
  }
  result = allocateId(&record.id);
  if (result != CoreResult::kOk) {
    return result;
  }
  record.classId = classId;
  record.status = static_cast<uint8_t>(StudentStatus::kActive);
  working_.students[working_.studentCount++] = record;
  result = commitEdit();
  if (result == CoreResult::kOk && studentId != nullptr) {
    *studentId = record.id;
  }
  return result;
}

CoreResult DeviceCore::updateStudent(uint32_t studentId, const char *admissionId,
                                     const char *name, uint32_t classId,
                                     StudentStatus status) {
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  const int index = findStudentIndex(working_, studentId);
  if (index < 0 || findClassIndex(working_, classId) < 0) {
    return CoreResult::kNotFound;
  }
  if (!isStudentStatusValid(static_cast<uint8_t>(status))) {
    return CoreResult::kInvalidArgument;
  }

  StudentRecord &record = working_.students[index];
  char candidateAdmissionId[sizeof(record.admissionId)];
  if (!copyText(candidateAdmissionId, sizeof(candidateAdmissionId), admissionId,
                true) ||
      !copyText(record.name, sizeof(record.name), name, true)) {
    return CoreResult::kInvalidArgument;
  }
  for (uint16_t other = 0; other < working_.studentCount; ++other) {
    if (other != static_cast<uint16_t>(index) &&
        strcmp(working_.students[other].admissionId, candidateAdmissionId) == 0) {
      return CoreResult::kDuplicate;
    }
  }
  memcpy(record.admissionId, candidateAdmissionId, sizeof(candidateAdmissionId));
  record.classId = classId;
  record.status = static_cast<uint8_t>(status);
  return commitEdit();
}

CoreResult DeviceCore::removeStudent(uint32_t studentId) {
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  const int index = findStudentIndex(working_, studentId);
  if (index < 0) {
    return CoreResult::kNotFound;
  }
  for (uint16_t link = 0; link < working_.guardianStudentLinkCount; ++link) {
    if (working_.guardianStudentLinks[link].studentId == studentId) {
      return CoreResult::kConflict;
    }
  }
  for (uint16_t request = 0; request < working_.dismissalRequestCount;
       ++request) {
    if (working_.dismissalRequests[request].studentId == studentId) {
      return CoreResult::kConflict;
    }
  }
  eraseRecord(working_.students, &working_.studentCount,
              static_cast<uint16_t>(index));
  return commitEdit();
}

CoreResult DeviceCore::createGuardian(const char *name, const char *contact,
                                      uint32_t *guardianId) {
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  if (working_.guardianCount >= kMaximumGuardians) {
    return CoreResult::kCapacity;
  }
  GuardianRecord record;
  memset(&record, 0, sizeof(record));
  if (!copyText(record.name, sizeof(record.name), name, true) ||
      !copyText(record.contact, sizeof(record.contact), contact, false)) {
    return CoreResult::kInvalidArgument;
  }
  result = allocateId(&record.id);
  if (result != CoreResult::kOk) {
    return result;
  }
  working_.guardians[working_.guardianCount++] = record;
  result = commitEdit();
  if (result == CoreResult::kOk && guardianId != nullptr) {
    *guardianId = record.id;
  }
  return result;
}

CoreResult DeviceCore::updateGuardian(uint32_t guardianId, const char *name,
                                      const char *contact) {
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  const int index = findGuardianIndex(working_, guardianId);
  if (index < 0) {
    return CoreResult::kNotFound;
  }
  GuardianRecord &record = working_.guardians[index];
  if (!copyText(record.name, sizeof(record.name), name, true) ||
      !copyText(record.contact, sizeof(record.contact), contact, false)) {
    return CoreResult::kInvalidArgument;
  }
  return commitEdit();
}

CoreResult DeviceCore::removeGuardian(uint32_t guardianId) {
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  const int index = findGuardianIndex(working_, guardianId);
  if (index < 0) {
    return CoreResult::kNotFound;
  }
  for (uint16_t palm = 0; palm < working_.palmCount; ++palm) {
    if (working_.palms[palm].guardianId == guardianId) {
      return CoreResult::kConflict;
    }
  }
  for (uint16_t link = 0; link < working_.guardianStudentLinkCount; ++link) {
    if (working_.guardianStudentLinks[link].guardianId == guardianId) {
      return CoreResult::kConflict;
    }
  }
  for (uint16_t request = 0; request < working_.dismissalRequestCount;
       ++request) {
    if (working_.dismissalRequests[request].guardianId == guardianId) {
      return CoreResult::kConflict;
    }
  }
  eraseRecord(working_.guardians, &working_.guardianCount,
              static_cast<uint16_t>(index));
  return commitEdit();
}

CoreResult DeviceCore::linkGuardianToStudent(uint32_t guardianId,
                                             uint32_t studentId) {
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  if (findGuardianIndex(working_, guardianId) < 0 ||
      findStudentIndex(working_, studentId) < 0) {
    return CoreResult::kNotFound;
  }
  if (linkExists(working_, guardianId, studentId)) {
    return CoreResult::kDuplicate;
  }
  if (working_.guardianStudentLinkCount >= kMaximumGuardianStudentLinks) {
    return CoreResult::kCapacity;
  }
  GuardianStudentLink &link =
      working_.guardianStudentLinks[working_.guardianStudentLinkCount++];
  link.guardianId = guardianId;
  link.studentId = studentId;
  return commitEdit();
}

CoreResult DeviceCore::unlinkGuardianFromStudent(uint32_t guardianId,
                                                 uint32_t studentId) {
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  for (uint16_t index = 0; index < working_.guardianStudentLinkCount;
       ++index) {
    const GuardianStudentLink &link = working_.guardianStudentLinks[index];
    if (link.guardianId == guardianId && link.studentId == studentId) {
      for (uint16_t request = 0; request < working_.dismissalRequestCount;
           ++request) {
        const DismissalRequest &dismissal = working_.dismissalRequests[request];
        if (dismissal.guardianId == guardianId && dismissal.studentId == studentId) {
          return CoreResult::kConflict;
        }
      }
      eraseRecord(working_.guardianStudentLinks,
                  &working_.guardianStudentLinkCount, index);
      return commitEdit();
    }
  }
  return CoreResult::kNotFound;
}

CoreResult DeviceCore::registerGuardianPalm(uint32_t guardianId,
                                            uint16_t sensorUid,
                                            uint32_t registeredAt,
                                            uint32_t *palmId) {
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  if (sensorUid == 0) {
    return CoreResult::kInvalidArgument;
  }
  if (findGuardianIndex(working_, guardianId) < 0) {
    return CoreResult::kNotFound;
  }
  if (working_.palmCount >= kMaximumGuardianPalms) {
    return CoreResult::kCapacity;
  }
  for (uint16_t index = 0; index < working_.palmCount; ++index) {
    if (working_.palms[index].sensorUid == sensorUid) {
      return CoreResult::kDuplicate;
    }
  }
  GuardianPalmRecord record;
  memset(&record, 0, sizeof(record));
  result = allocateId(&record.id);
  if (result != CoreResult::kOk) {
    return result;
  }
  record.guardianId = guardianId;
  record.sensorUid = sensorUid;
  record.registeredAt = registeredAt;
  record.status = static_cast<uint8_t>(PalmStatus::kActive);
  working_.palms[working_.palmCount++] = record;
  result = commitEdit();
  if (result == CoreResult::kOk && palmId != nullptr) {
    *palmId = record.id;
  }
  return result;
}

CoreResult DeviceCore::setGuardianPalmStatus(uint32_t palmId,
                                              PalmStatus status) {
  if (!isPalmStatusValid(static_cast<uint8_t>(status))) {
    return CoreResult::kInvalidArgument;
  }
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  const int index = findPalmIndex(working_, palmId);
  if (index < 0) {
    return CoreResult::kNotFound;
  }
  working_.palms[index].status = static_cast<uint8_t>(status);
  return commitEdit();
}

CoreResult DeviceCore::removeGuardianPalm(uint32_t palmId) {
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  const int index = findPalmIndex(working_, palmId);
  if (index < 0) {
    return CoreResult::kNotFound;
  }
  eraseRecord(working_.palms, &working_.palmCount,
              static_cast<uint16_t>(index));
  return commitEdit();
}

CoreResult DeviceCore::createDismissalRequest(uint32_t studentId,
                                              const char *requestedBy,
                                              uint32_t createdAt,
                                              uint32_t *requestId) {
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  if (findStudentIndex(working_, studentId) < 0) {
    return CoreResult::kNotFound;
  }
  if (working_.dismissalRequestCount >= kMaximumDismissalRequests) {
    return CoreResult::kCapacity;
  }
  DismissalRequest record;
  memset(&record, 0, sizeof(record));
  if (!copyText(record.requestedBy, sizeof(record.requestedBy), requestedBy,
                true)) {
    return CoreResult::kInvalidArgument;
  }
  result = allocateId(&record.id);
  if (result != CoreResult::kOk) {
    return result;
  }
  record.studentId = studentId;
  record.createdAt = createdAt;
  record.updatedAt = createdAt;
  record.status = static_cast<uint8_t>(DismissalStatus::kPending);
  working_.dismissalRequests[working_.dismissalRequestCount++] = record;
  result = commitEdit();
  if (result == CoreResult::kOk && requestId != nullptr) {
    *requestId = record.id;
  }
  return result;
}

CoreResult DeviceCore::transitionDismissalRequest(uint32_t requestId,
                                                  DismissalStatus status,
                                                  uint32_t guardianId,
                                                  uint32_t updatedAt) {
  if (!isDismissalStatusValid(static_cast<uint8_t>(status))) {
    return CoreResult::kInvalidArgument;
  }
  CoreResult result = prepareEdit();
  if (result != CoreResult::kOk) {
    return result;
  }
  const int index = findRequestIndex(working_, requestId);
  if (index < 0) {
    return CoreResult::kNotFound;
  }
  DismissalRequest &record = working_.dismissalRequests[index];
  if (!canTransition(static_cast<DismissalStatus>(record.status), status)) {
    return CoreResult::kConflict;
  }
  const bool needsGuardian = status == DismissalStatus::kApproved ||
                             status == DismissalStatus::kCompleted;
  if (needsGuardian && guardianId == 0) {
    return CoreResult::kInvalidArgument;
  }
  if (guardianId != 0 &&
      (findGuardianIndex(working_, guardianId) < 0 ||
       !linkExists(working_, guardianId, record.studentId))) {
    return CoreResult::kConflict;
  }
  record.guardianId = guardianId;
  record.status = static_cast<uint8_t>(status);
  record.updatedAt = updatedAt;
  return commitEdit();
}

bool DeviceCore::validateSnapshot(const Snapshot &snapshot) {
  if (snapshot.schemaVersion != kCurrentSchemaVersion || snapshot.nextId == 0 ||
      snapshot.classCount > kMaximumClasses ||
      snapshot.studentCount > kMaximumStudents ||
      snapshot.guardianCount > kMaximumGuardians ||
      snapshot.palmCount > kMaximumGuardianPalms ||
      snapshot.guardianStudentLinkCount > kMaximumGuardianStudentLinks ||
      snapshot.dismissalRequestCount > kMaximumDismissalRequests ||
      !isProvisioningStateValid(snapshot.configuration.provisioningState) ||
      !isNonBlankText(snapshot.configuration.deviceLabel,
                      sizeof(snapshot.configuration.deviceLabel), false) ||
      !isNonBlankText(snapshot.configuration.siteLabel,
                      sizeof(snapshot.configuration.siteLabel), false) ||
      snapshot.configuration.timezoneOffsetMinutes < -720 ||
      snapshot.configuration.timezoneOffsetMinutes > 840) {
    return false;
  }

  if (snapshot.configuration.provisioningState ==
      static_cast<uint8_t>(ProvisioningState::kUnprovisioned)) {
    if (snapshot.configuration.wifiSsid[0] != '\0' ||
        snapshot.configuration.wifiPassphrase[0] != '\0') {
      return false;
    }
  } else if (!hasValidNetworkConfiguration(snapshot.configuration)) {
    return false;
  }

  for (uint16_t index = 0; index < snapshot.classCount; ++index) {
    const ClassRecord &record = snapshot.classes[index];
    if (record.id == 0 || (record.active != 0 && record.active != 1) ||
        !isNonBlankText(record.name, sizeof(record.name), true) ||
        !isNonBlankText(record.section, sizeof(record.section), true)) {
      return false;
    }
    for (uint16_t other = 0; other < index; ++other) {
      if (snapshot.classes[other].id == record.id) {
        return false;
      }
    }
  }

  for (uint16_t index = 0; index < snapshot.studentCount; ++index) {
    const StudentRecord &record = snapshot.students[index];
    if (record.id == 0 || findClassIndex(snapshot, record.classId) < 0 ||
        !isStudentStatusValid(record.status) ||
        !isNonBlankText(record.admissionId, sizeof(record.admissionId), true) ||
        !isNonBlankText(record.name, sizeof(record.name), true)) {
      return false;
    }
    for (uint16_t other = 0; other < index; ++other) {
      if (snapshot.students[other].id == record.id ||
          strcmp(snapshot.students[other].admissionId, record.admissionId) == 0) {
        return false;
      }
    }
  }

  for (uint16_t index = 0; index < snapshot.guardianCount; ++index) {
    const GuardianRecord &record = snapshot.guardians[index];
    if (record.id == 0 ||
        !isNonBlankText(record.name, sizeof(record.name), true) ||
        !isNonBlankText(record.contact, sizeof(record.contact), false)) {
      return false;
    }
    for (uint16_t other = 0; other < index; ++other) {
      if (snapshot.guardians[other].id == record.id) {
        return false;
      }
    }
  }

  for (uint16_t index = 0; index < snapshot.palmCount; ++index) {
    const GuardianPalmRecord &record = snapshot.palms[index];
    if (record.id == 0 || record.sensorUid == 0 ||
        findGuardianIndex(snapshot, record.guardianId) < 0 ||
        !isPalmStatusValid(record.status)) {
      return false;
    }
    for (uint16_t other = 0; other < index; ++other) {
      if (snapshot.palms[other].id == record.id ||
          snapshot.palms[other].sensorUid == record.sensorUid) {
        return false;
      }
    }
  }

  for (uint16_t index = 0; index < snapshot.guardianStudentLinkCount;
       ++index) {
    const GuardianStudentLink &link = snapshot.guardianStudentLinks[index];
    if (findGuardianIndex(snapshot, link.guardianId) < 0 ||
        findStudentIndex(snapshot, link.studentId) < 0) {
      return false;
    }
    for (uint16_t other = 0; other < index; ++other) {
      const GuardianStudentLink &existing = snapshot.guardianStudentLinks[other];
      if (existing.guardianId == link.guardianId &&
          existing.studentId == link.studentId) {
        return false;
      }
    }
  }

  for (uint16_t index = 0; index < snapshot.dismissalRequestCount; ++index) {
    const DismissalRequest &record = snapshot.dismissalRequests[index];
    if (record.id == 0 || findStudentIndex(snapshot, record.studentId) < 0 ||
        !isDismissalStatusValid(record.status) ||
        !isNonBlankText(record.requestedBy, sizeof(record.requestedBy), true)) {
      return false;
    }
    if (record.guardianId != 0 &&
        (findGuardianIndex(snapshot, record.guardianId) < 0 ||
         !linkExists(snapshot, record.guardianId, record.studentId))) {
      return false;
    }
    const DismissalStatus status = static_cast<DismissalStatus>(record.status);
    if ((status == DismissalStatus::kApproved ||
         status == DismissalStatus::kCompleted) &&
        record.guardianId == 0) {
      return false;
    }
    for (uint16_t other = 0; other < index; ++other) {
      if (snapshot.dismissalRequests[other].id == record.id) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace guardian_x
