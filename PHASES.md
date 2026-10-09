# GUARDIAN X Delivery Roadmap

## Dependency model

```text
Phase 0
   ↓
Phase 1
   ↓
Phase 2
   ↓
Phase 3
   ↓
Phase 4
   ↓
Phase 5
   ↓
Phase 6
   ↓
Phase 7
   ↓
Phase 8
   ↓
Phase 9
   ↓
Phase 10
```

Do not skip prerequisites. A phase must be validated before the next phase begins.

## Phase 0 — Project Foundation

- **Objective:** Establish a clean, compilable ESP32-S3 PlatformIO/Arduino project.
- **Major components:** PlatformIO configuration, minimal serial boot firmware, repository hygiene, and project documentation.
- **Dependencies:** None.
- **Expected output:** A buildable project targeting the ESP32-S3 DevKitC-1.
- **Completion criteria:** `pio run` succeeds; required directories and documentation exist; no application functionality is added.

## Phase 1 — Hardware Foundation

- **Objective:** Validate the ESP32-S3, SEN0677, and SSD1309 hardware.
- **Major components:** Sensor initialization, palm enrollment and recognition, UID listing/deletion, OLED status, serial test commands, and error handling.
- **Dependencies:** Validated Phase 0 and the reference material at `reference/Code.ino`.
- **Expected output:** Independently validated sensor and display hardware behavior.
- **Completion criteria:** The listed hardware functions and error paths work against the actual connected devices.

**Implemented scope:** Phase 1 ports the reference SSD1309 initialization and direct SEN0677 operations into the PlatformIO project. It provides serial commands for user-ID listing, palm enrollment, one recognition attempt, and UID deletion. It does not include any door-lock behavior, Wi-Fi, web UI/API, storage, or authentication.

## Phase 2 — Local Storage & Device Core

- **Objective:** Create the local data and device-core foundation.
- **Major components:** Classes, students, guardians, guardian-palms, student-guardian relationships, dismissal requests, and device configuration.
- **Dependencies:** Validated Phase 1.
- **Expected output:** Versioned, recoverable local application data and core domain models.
- **Completion criteria:** Data models persist, recover, and pass their storage/device-core validation suite.

**Implemented scope:** Phase 2 adds a hardware-independent `DeviceCore` with
allocation-free models for device configuration, classes, students, guardians,
guardian-to-student relationships, guardian palm UID records, and dismissal
requests. Snapshots are stored in alternating LittleFS slots with a versioned
header and CRC32 validation; startup selects the newest valid snapshot and
reports a deterministic recovery error when neither slot is usable. The core
does not call the SEN0677, persist biometric templates or recognition results,
or make a verification decision.

## Phase 3 — Device Provisioning

- **Objective:** Provision a new device safely on first boot.
- **Major components:** First boot flow, setup AP, local setup page, Wi-Fi configuration, persistent network configuration, and factory reset.
- **Dependencies:** Validated Phase 2.
- **Expected output:** A locally provisioned device that can recover to factory setup.
- **Completion criteria:** Provisioning, persistence, reconnection, and reset paths are validated on hardware.

**Implemented scope:** Phase 3 adds a persistent provisioning state to the
Phase 2 device configuration snapshot (`unprovisioned`/`provisioned`). An
unprovisioned device starts an open (passwordless, single-client) setup Access
Point named `GUARDIAN-X-<last 4 MAC hex>` and serves an embedded,
single-purpose setup page (HTTP on the AP only) that accepts a Wi-Fi
SSID/passphrase, validates the input, attempts a real connection (20 s
timeout), and only then persists the configuration and restarts into normal
mode. A failed connection keeps the setup AP alive so the user receives the
error and can retry, and never marks the device provisioned. A provisioned
device skips the AP and connects to the configured network with non-blocking
retries (30 s attempt timeout, 5 s cadence) so serial diagnostics stay
responsive during a router outage; a temporary outage never clears
provisioning. Recovery is layered: an invalid slot falls back to the newest
CRC-valid snapshot; if no snapshot validates, the invalid slots are erased and
a fresh unprovisioned snapshot is created; if the filesystem cannot be mounted
at all (blank factory-fresh flash or unrecoverable damage), it is reformatted.
Each layer only discards data that is already unreadable and always lands in
setup mode. The `f` serial command performs a deliberate factory reset: it
clears only the network/provisioning state and restarts into setup mode —
classes, students, guardians, palm records, and dismissal records are
preserved. Wi-Fi credentials live only in the snapshot (the Wi-Fi library's
NVS credential cache is disabled) and are never logged. The setup endpoints
expose no application data and perform no authentication (Phase 4
responsibility). Hardware validation is **not yet performed**: the on-device
provisioning flow (AP discovery, setup page, live connection, reset, and
Phase 1 sensor regression) must still be verified against a connected
ESP32-S3.

## Phase 4 — Authentication & Security

- **Objective:** Protect local administration and device configuration.
- **Major components:** Admin account, password hashing, sessions, authorization, protected APIs, logout, and secure configuration.
- **Dependencies:** Validated Phase 3.
- **Expected output:** Authenticated, authorized local administration.
- **Completion criteria:** Security flows and protected-route tests pass, including invalid and expired session cases.

## Phase 5 — Local Web Server & API

- **Objective:** Deliver a local HTTP interface over the validated core.
- **Major components:** HTTP endpoints, request validation, authentication integration, storage integration, hardware integration, and application-logic integration.
- **Dependencies:** Validated Phase 4.
- **Expected output:** A tested local HTTP server and API.
- **Completion criteria:** API contract, authorization, validation, storage, and hardware interaction tests pass.

```text
Browser
   |
   v
ESP32 local HTTP server
   |
   +-- Authentication
   +-- Storage
   +-- Hardware
   +-- Application logic
```

## Phase 6 — GUARDIAN X Web UI

- **Objective:** Build the focused local management interface.
- **Major components:** Dashboard, classes, students, guardians, guardian palms, dismissal requests, and device status/settings.
- **Dependencies:** Validated Phase 5.
- **Expected output:** A local browser UI for the named GUARDIAN X functions.
- **Completion criteria:** All named areas work with the API and accessibility/usability checks are complete.

Out of scope: attendance, fees, exams, homework, timetable, transport, messaging, parent portal, QR/QR scanning, face recognition, fingerprint, iris, surveillance, and unrelated analytics.

## Phase 7 — Guardian Palm Enrollment

- **Objective:** Register and associate a guardian palm identity.
- **Major components:** Admin-controlled enrollment, SEN0677 UID capture, and guardian-UID linking.
- **Dependencies:** Validated Phases 1–6.
- **Expected output:** A validated guardian palm enrollment flow.
- **Completion criteria:** Enrollment, UID storage/linking, duplicate/error handling, and authorization checks pass.

```text
Admin
 ↓
Guardian
 ↓
Register Palm
 ↓
SEN0677
 ↓
Palm UID
 ↓
Link UID to Guardian
```

## Phase 8 — Real Dismissal Workflow

- **Objective:** Validate guardian authorization for a requested student dismissal.
- **Major components:** Teacher request creation, pending state, palm identification, guardian/student relationship validation, and approval/denial results.
- **Dependencies:** Validated Phases 1–7.
- **Expected output:** A complete local dismissal state machine.
- **Completion criteria:** Approved, denied, timeout, invalid relationship, and error workflows are validated end-to-end.

```text
Teacher
 ↓
Select student
 ↓
Create dismissal request
 ↓
Pending
 ↓
Guardian presents palm
 ↓
SEN0677 identifies guardian
 ↓
Guardian/student relationship validated
 ↓
Dismissal approved or denied
```

## Phase 9 — OLED / Hardware UX Integration

- **Objective:** Integrate clear device feedback with the verified workflows.
- **Major components:** Ready, enrollment, verification, approved, denied, timeout, and sensor-error OLED states.
- **Dependencies:** Validated Phase 8.
- **Expected output:** Consistent, tested hardware feedback for all user-visible states.
- **Completion criteria:** Each state is readable, reachable, and verified on the device, including error recovery.

## Phase 10 — Testing, Hardening & Release

- **Objective:** Make the validated system ready for production release.
- **Major components:** Hardware, storage, authentication, API, dismissal, and network tests; failure recovery; security testing; long-running tests; memory/heap testing; and release validation.
- **Dependencies:** Validated Phase 9.
- **Expected output:** Release candidate and operational validation record.
- **Completion criteria:** All acceptance, reliability, recovery, security, and resource-use gates pass before release.

## Current implementation boundary

Phases 0–2 are implemented. Phases 3–10 remain planned and must not be
started automatically.
