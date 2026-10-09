#include <string.h>

#include <unity.h>

#include "guardian_x/device_core.h"

using guardian_x::CoreResult;
using guardian_x::DeviceCore;
using guardian_x::DismissalStatus;
using guardian_x::Snapshot;
using guardian_x::SnapshotV1;
using guardian_x::StorageBackend;
using guardian_x::StorageHeader;

namespace {

class MemoryStorage final : public StorageBackend {
 public:
  MemoryStorage() : available_(true), failNextWrite_(false), lastWritten_(0) {
    memset(slots_, 0, sizeof(slots_));
  }

  bool begin() override { return available_; }

  bool slotExists(uint8_t slot) const override {
    return slot < guardian_x::kStorageSlotCount && slots_[slot].present;
  }

  bool readSlot(uint8_t slot, StorageHeader *header, void *payload,
                size_t payloadSize) override {
    if (!slotExists(slot) || header == nullptr || payload == nullptr ||
        payloadSize != slots_[slot].payloadSize) {
      return false;
    }
    *header = slots_[slot].header;
    memcpy(payload, slots_[slot].payload, payloadSize);
    return true;
  }

  bool writeSlot(uint8_t slot, const StorageHeader &header,
                 const void *payload, size_t payloadSize) override {
    if (slot >= guardian_x::kStorageSlotCount || payload == nullptr ||
        payloadSize == 0 || payloadSize > sizeof(Slot::payload)) {
      return false;
    }
    if (failNextWrite_) {
      failNextWrite_ = false;
      return false;
    }
    slots_[slot].header = header;
    memcpy(slots_[slot].payload, payload, payloadSize);
    slots_[slot].payloadSize = payloadSize;
    slots_[slot].present = true;
    lastWritten_ = slot;
    return true;
  }

  // Overwrite the newest slot payload with a syntactically valid, CRC-correct
  // snapshot so begin() must reject it through snapshot validation alone.
  void replaceNewestSnapshot(const Snapshot &snapshot) {
    if (lastWritten_ >= guardian_x::kStorageSlotCount) {
      return;
    }
    slots_[lastWritten_].payloadSize = sizeof(Snapshot);
    memcpy(slots_[lastWritten_].payload, &snapshot, sizeof(Snapshot));
    slots_[lastWritten_].header.payloadCrc32 = guardian_x::crc32(
        slots_[lastWritten_].payload, sizeof(Snapshot));
    slots_[lastWritten_].present = true;
  }

  // Replace the newest slot with a legacy Phase 2 (schema v1) snapshot.
  void replaceNewestSlotWithLegacySnapshot(const SnapshotV1 &snapshot) {
    if (lastWritten_ >= guardian_x::kStorageSlotCount) {
      return;
    }
    StorageHeader legacyHeader;
    memset(&legacyHeader, 0, sizeof(legacyHeader));
    legacyHeader.magic[0] = 'G';
    legacyHeader.magic[1] = 'X';
    legacyHeader.magic[2] = 'D';
    legacyHeader.magic[3] = '2';
    legacyHeader.formatVersion = 1;
    legacyHeader.generation = slots_[lastWritten_].header.generation + 1;
    legacyHeader.payloadSize = sizeof(SnapshotV1);
    legacyHeader.payloadCrc32 = guardian_x::crc32(
        reinterpret_cast<const uint8_t *>(&snapshot), sizeof(SnapshotV1));
    slots_[lastWritten_].header = legacyHeader;
    slots_[lastWritten_].payloadSize = sizeof(SnapshotV1);
    memcpy(slots_[lastWritten_].payload, &snapshot, sizeof(SnapshotV1));
    slots_[lastWritten_].present = true;
  }

  void failNextWrite() { failNextWrite_ = true; }

  void corruptNewestSnapshot() {
    SlottedSnapshot *newest = newestSnapshot();
    if (newest != nullptr) {
      newest->nextId = 0;
    }
  }

  Snapshot *newestSnapshot() {
    if (lastWritten_ >= guardian_x::kStorageSlotCount) {
      return nullptr;
    }
    return reinterpret_cast<Snapshot *>(slots_[lastWritten_].payload);
  }

 private:
  struct Slot {
    bool present;
    StorageHeader header;
    uint8_t payload[sizeof(Snapshot)];
    size_t payloadSize;
  };

  typedef Snapshot SlottedSnapshot;

  Slot slots_[guardian_x::kStorageSlotCount];
  bool available_;
  bool failNextWrite_;
  uint8_t lastWritten_;
};

void assertResult(CoreResult expected, CoreResult actual) {
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected),
                          static_cast<uint8_t>(actual));
}

void createLinkedStudentAndGuardian(DeviceCore *core, uint32_t *studentId,
                                    uint32_t *guardianId) {
  uint32_t classId = 0;
  assertResult(CoreResult::kOk,
               core->createClass("Year 5", "A", &classId));
  assertResult(CoreResult::kOk,
               core->createStudent("ADM-101", "Student Name", classId,
                                   studentId));
  assertResult(CoreResult::kOk,
               core->createGuardian("Guardian Name", "", guardianId));
  assertResult(CoreResult::kOk,
               core->linkGuardianToStudent(*guardianId, *studentId));
}

void test_models_persist_and_enforce_relationships() {
  MemoryStorage storage;
  DeviceCore core(storage);
  assertResult(CoreResult::kOk, core.begin());

  uint32_t studentId = 0;
  uint32_t guardianId = 0;
  createLinkedStudentAndGuardian(&core, &studentId, &guardianId);

  assertResult(CoreResult::kDuplicate,
               core.linkGuardianToStudent(guardianId, studentId));

  uint32_t palmId = 0;
  assertResult(CoreResult::kOk,
               core.registerGuardianPalm(guardianId, 77, 0, &palmId));
  assertResult(CoreResult::kDuplicate,
               core.registerGuardianPalm(guardianId, 77, 0, nullptr));

  uint32_t requestId = 0;
  assertResult(CoreResult::kOk,
               core.createDismissalRequest(studentId, "teacher-ref", 0,
                                           &requestId));
  assertResult(CoreResult::kInvalidArgument,
               core.transitionDismissalRequest(requestId,
                                               DismissalStatus::kApproved, 0, 0));
  assertResult(CoreResult::kOk,
               core.transitionDismissalRequest(requestId,
                                               DismissalStatus::kApproved,
                                               guardianId, 0));
  assertResult(CoreResult::kOk,
               core.transitionDismissalRequest(requestId,
                                               DismissalStatus::kCompleted,
                                               guardianId, 0));

  DeviceCore reloaded(storage);
  assertResult(CoreResult::kOk, reloaded.begin());
  TEST_ASSERT_EQUAL_UINT16(1, reloaded.snapshot().classCount);
  TEST_ASSERT_EQUAL_UINT16(1, reloaded.snapshot().studentCount);
  TEST_ASSERT_EQUAL_UINT16(1, reloaded.snapshot().guardianCount);
  TEST_ASSERT_EQUAL_UINT16(1, reloaded.snapshot().palmCount);
  TEST_ASSERT_EQUAL_UINT16(1, reloaded.snapshot().guardianStudentLinkCount);
  TEST_ASSERT_EQUAL_UINT16(1, reloaded.snapshot().dismissalRequestCount);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DismissalStatus::kCompleted),
                          reloaded.snapshot().dismissalRequests[0].status);
}

void test_recovery_uses_last_valid_slot() {
  MemoryStorage storage;
  DeviceCore core(storage);
  assertResult(CoreResult::kOk, core.begin());

  uint32_t classId = 0;
  assertResult(CoreResult::kOk,
               core.createClass("Year 5", "B", &classId));
  uint32_t guardianId = 0;
  assertResult(CoreResult::kOk,
               core.createGuardian("Guardian Name", "", &guardianId));
  storage.corruptNewestSnapshot();

  DeviceCore recovered(storage);
  assertResult(CoreResult::kOk, recovered.begin());
  TEST_ASSERT_EQUAL_UINT16(1, recovered.snapshot().classCount);
  TEST_ASSERT_EQUAL_UINT16(0, recovered.snapshot().guardianCount);
  TEST_ASSERT_TRUE(recovered.generation() < core.generation());
}

void test_failed_write_does_not_publish_unpersisted_state() {
  MemoryStorage storage;
  DeviceCore core(storage);
  assertResult(CoreResult::kOk, core.begin());
  const uint32_t generation = core.generation();

  storage.failNextWrite();
  assertResult(CoreResult::kStorageUnavailable,
               core.createClass("Year 6", "A", nullptr));
  TEST_ASSERT_EQUAL_UINT16(0, core.snapshot().classCount);
  TEST_ASSERT_EQUAL_UINT32(generation, core.generation());

  DeviceCore reloaded(storage);
  assertResult(CoreResult::kOk, reloaded.begin());
  TEST_ASSERT_EQUAL_UINT16(0, reloaded.snapshot().classCount);
}

void test_orphaned_data_and_referenced_deletes_are_rejected() {
  MemoryStorage storage;
  DeviceCore core(storage);
  assertResult(CoreResult::kOk, core.begin());

  assertResult(CoreResult::kNotFound,
               core.registerGuardianPalm(999, 10, 0, nullptr));
  assertResult(CoreResult::kNotFound,
               core.linkGuardianToStudent(1, 1));

  uint32_t studentId = 0;
  uint32_t guardianId = 0;
  createLinkedStudentAndGuardian(&core, &studentId, &guardianId);
  assertResult(CoreResult::kConflict, core.removeStudent(studentId));
  assertResult(CoreResult::kConflict, core.removeGuardian(guardianId));
}

// --- Phase 3 provisioning tests -------------------------------------------

void test_fresh_device_is_unprovisioned() {
  MemoryStorage storage;
  DeviceCore core(storage);
  assertResult(CoreResult::kOk, core.begin());

  TEST_ASSERT_FALSE(core.isProvisioned());
  TEST_ASSERT_EQUAL_UINT8(0, core.snapshot().configuration.provisioningState);
  TEST_ASSERT_EQUAL_UINT8('\0', core.snapshot().configuration.wifiSsid[0]);
  TEST_ASSERT_EQUAL_UINT8('\0',
                          core.snapshot().configuration.wifiPassphrase[0]);
}

void test_network_configuration_persists_and_marks_provisioned() {
  MemoryStorage storage;
  DeviceCore core(storage);
  assertResult(CoreResult::kOk, core.begin());

  assertResult(CoreResult::kOk,
               core.configureNetwork("SchoolWiFi", "guardian-passphrase"));
  TEST_ASSERT_TRUE(core.isProvisioned());
  TEST_ASSERT_EQUAL_STRING("SchoolWiFi",
                           core.snapshot().configuration.wifiSsid);
  TEST_ASSERT_EQUAL_STRING("guardian-passphrase",
                           core.snapshot().configuration.wifiPassphrase);

  // The configuration must survive a reboot (new core over same storage).
  DeviceCore reloaded(storage);
  assertResult(CoreResult::kOk, reloaded.begin());
  TEST_ASSERT_TRUE(reloaded.isProvisioned());
  TEST_ASSERT_EQUAL_STRING("SchoolWiFi",
                           reloaded.snapshot().configuration.wifiSsid);
  TEST_ASSERT_EQUAL_STRING("guardian-passphrase",
                           reloaded.snapshot().configuration.wifiPassphrase);
}

void test_open_network_with_empty_passphrase_is_accepted() {
  MemoryStorage storage;
  DeviceCore core(storage);
  assertResult(CoreResult::kOk, core.begin());

  assertResult(CoreResult::kOk, core.configureNetwork("GuestWiFi", ""));
  TEST_ASSERT_TRUE(core.isProvisioned());
  TEST_ASSERT_EQUAL_STRING("GuestWiFi",
                           core.snapshot().configuration.wifiSsid);

  DeviceCore reloaded(storage);
  assertResult(CoreResult::kOk, reloaded.begin());
  TEST_ASSERT_TRUE(reloaded.isProvisioned());
}

void test_invalid_network_configuration_is_rejected() {
  MemoryStorage storage;
  DeviceCore core(storage);
  assertResult(CoreResult::kOk, core.begin());
  const uint32_t generation = core.generation();

  TEST_ASSERT_FALSE(DeviceCore::validateNetworkConfiguration("", ""));
  TEST_ASSERT_FALSE(DeviceCore::validateNetworkConfiguration("  ", ""));
  TEST_ASSERT_FALSE(DeviceCore::validateNetworkConfiguration(nullptr, ""));
  TEST_ASSERT_FALSE(
      DeviceCore::validateNetworkConfiguration("SchoolWiFi", "short"));
  TEST_ASSERT_FALSE(DeviceCore::validateNetworkConfiguration(
      "SchoolWiFi", "0123456789012345678901234567890123456789012345678901234567890123"));
  TEST_ASSERT_FALSE(DeviceCore::validateNetworkConfiguration(
      "this-ssid-is-far-too-long-to-store", "valid-passphrase"));
  TEST_ASSERT_FALSE(DeviceCore::validateNetworkConfiguration(
      "SchoolWiFi", "valid\npassphrase"));

  assertResult(CoreResult::kInvalidArgument,
               core.configureNetwork("", ""));
  assertResult(CoreResult::kInvalidArgument,
               core.configureNetwork("SchoolWiFi", "short"));
  assertResult(CoreResult::kInvalidArgument,
               core.configureNetwork("  ", "valid-passphrase"));

  // Failed validation must leave the device unprovisioned and unchanged.
  TEST_ASSERT_FALSE(core.isProvisioned());
  TEST_ASSERT_EQUAL_UINT32(generation, core.generation());
  TEST_ASSERT_EQUAL_UINT8('\0', core.snapshot().configuration.wifiSsid[0]);
}

void test_failed_persistence_keeps_device_unprovisioned() {
  MemoryStorage storage;
  DeviceCore core(storage);
  assertResult(CoreResult::kOk, core.begin());

  storage.failNextWrite();
  assertResult(CoreResult::kStorageUnavailable,
               core.configureNetwork("SchoolWiFi", "guardian-passphrase"));
  TEST_ASSERT_FALSE(core.isProvisioned());
  TEST_ASSERT_EQUAL_UINT8('\0', core.snapshot().configuration.wifiSsid[0]);

  DeviceCore reloaded(storage);
  assertResult(CoreResult::kOk, reloaded.begin());
  TEST_ASSERT_FALSE(reloaded.isProvisioned());
}

void test_factory_reset_returns_to_unprovisioned_and_keeps_application_data() {
  MemoryStorage storage;
  DeviceCore core(storage);
  assertResult(CoreResult::kOk, core.begin());

  assertResult(CoreResult::kOk,
               core.configureNetwork("SchoolWiFi", "guardian-passphrase"));
  uint32_t studentId = 0;
  uint32_t guardianId = 0;
  createLinkedStudentAndGuardian(&core, &studentId, &guardianId);
  TEST_ASSERT_TRUE(core.isProvisioned());

  assertResult(CoreResult::kOk, core.clearNetworkConfiguration());
  TEST_ASSERT_FALSE(core.isProvisioned());
  TEST_ASSERT_EQUAL_UINT8('\0', core.snapshot().configuration.wifiSsid[0]);
  TEST_ASSERT_EQUAL_UINT8('\0',
                          core.snapshot().configuration.wifiPassphrase[0]);

  // Factory reset is limited to provisioning state; Phase 2 data survives.
  TEST_ASSERT_EQUAL_UINT16(1, core.snapshot().classCount);
  TEST_ASSERT_EQUAL_UINT16(1, core.snapshot().studentCount);
  TEST_ASSERT_EQUAL_UINT16(1, core.snapshot().guardianCount);
  TEST_ASSERT_EQUAL_UINT16(1, core.snapshot().guardianStudentLinkCount);

  DeviceCore reloaded(storage);
  assertResult(CoreResult::kOk, reloaded.begin());
  TEST_ASSERT_FALSE(reloaded.isProvisioned());
  TEST_ASSERT_EQUAL_UINT16(1, reloaded.snapshot().classCount);
  TEST_ASSERT_EQUAL_UINT16(1, reloaded.snapshot().studentCount);
}

void test_invalid_provisioned_configuration_falls_back_to_recovery() {
  MemoryStorage storage;
  DeviceCore core(storage);
  assertResult(CoreResult::kOk, core.begin());

  assertResult(CoreResult::kOk,
               core.configureNetwork("SchoolWiFi", "guardian-passphrase"));
  TEST_ASSERT_TRUE(core.isProvisioned());

  // Corrupt the newest slot into a CRC-valid but invalid snapshot: a device
  // claiming to be provisioned without network credentials must be rejected.
  Snapshot tampered = core.snapshot();
  tampered.configuration.wifiSsid[0] = '\0';
  storage.replaceNewestSnapshot(tampered);

  DeviceCore recovered(storage);
  assertResult(CoreResult::kOk, recovered.begin());
  TEST_ASSERT_FALSE(recovered.isProvisioned());
}

void test_legacy_phase2_snapshot_migrates_as_unprovisioned() {
  MemoryStorage storage;
  DeviceCore core(storage);
  assertResult(CoreResult::kOk, core.begin());
  assertResult(CoreResult::kOk,
               core.configureNetwork("SchoolWiFi", "guardian-passphrase"));

  // Replace the newest slot with a Phase 2 (schema v1) snapshot that carries
  // application data but has no network configuration at all.
  SnapshotV1 legacy;
  memset(&legacy, 0, sizeof(legacy));
  legacy.schemaVersion = 1;
  legacy.nextId = 1;
  strcpy(legacy.configuration.deviceLabel, "Front Gate");
  legacy.classCount = 1;
  legacy.classes[0].id = 1;
  strcpy(legacy.classes[0].name, "Year 5");
  strcpy(legacy.classes[0].section, "A");
  legacy.classes[0].active = 1;
  storage.replaceNewestSlotWithLegacySnapshot(legacy);

  DeviceCore migrated(storage);
  assertResult(CoreResult::kOk, migrated.begin());
  TEST_ASSERT_FALSE(migrated.isProvisioned());
  TEST_ASSERT_EQUAL_STRING("Front Gate",
                           migrated.snapshot().configuration.deviceLabel);
  TEST_ASSERT_EQUAL_UINT16(1, migrated.snapshot().classCount);

  DeviceCore reloaded(storage);
  assertResult(CoreResult::kOk, reloaded.begin());
  TEST_ASSERT_FALSE(reloaded.isProvisioned());
  TEST_ASSERT_EQUAL_UINT16(1, reloaded.snapshot().classCount);
}

}  // namespace

void setUp() {}
void tearDown() {}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_models_persist_and_enforce_relationships);
  RUN_TEST(test_recovery_uses_last_valid_slot);
  RUN_TEST(test_failed_write_does_not_publish_unpersisted_state);
  RUN_TEST(test_orphaned_data_and_referenced_deletes_are_rejected);
  RUN_TEST(test_fresh_device_is_unprovisioned);
  RUN_TEST(test_network_configuration_persists_and_marks_provisioned);
  RUN_TEST(test_open_network_with_empty_passphrase_is_accepted);
  RUN_TEST(test_invalid_network_configuration_is_rejected);
  RUN_TEST(test_failed_persistence_keeps_device_unprovisioned);
  RUN_TEST(test_factory_reset_returns_to_unprovisioned_and_keeps_application_data);
  RUN_TEST(test_invalid_provisioned_configuration_falls_back_to_recovery);
  RUN_TEST(test_legacy_phase2_snapshot_migrates_as_unprovisioned);
  return UNITY_END();
}
