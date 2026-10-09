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

**Implemented scope:** Phase 4 adds a hardware-independent `SecurityCore`
(`guardian_x/security.h`) plus a LittleFS credential store and ESP32 adapters,
without implementing Phase 5's general HTTP API (the "protected APIs" above
are represented only by the reusable authorization boundary; see below).

- **Administrator initialization.** First-time setup happens only over the
  physical serial console (`a <username> <password>`), never through the
  open Phase 3 setup AP. There is no hardcoded default password and no
  credential material in source. Usernames (1–32 printable characters, no
  spaces) and passwords (8–64 printable characters) are validated before
  anything is persisted. Re-initializing an already-initialized device is
  rejected and never silently replaces the stored credential. A storage
  failure during initialization is reported as `kStoreFailure` and leaves the
  device uninitialized — success is only reported after the credential record
  is durably written.
- **Password hashing.** Passwords are hashed with PBKDF2-HMAC-SHA256 using
  the mbedTLS implementation already provided by the ESP-IDF framework
  (2.28.x; no new device dependency and no custom cryptography): 100,000
  iterations, a 16-byte random salt, and a 32-byte output. Every credential
  generation — including each password change — draws a fresh salt, and the
  record stores the iteration count and format version so parameters can be
  verified (and later migrated). Hash comparisons are constant-time. Plaintext
  passwords and hashes are never logged or persisted anywhere else.
- **Randomness.** Salts and session tokens come from `esp_fill_random()` (the
  ESP32-S3 hardware TRNG) behind an injectable `SecureRandom` interface; time
  comes from `millis()` behind an injectable `TimeSource` so the core stays
  host-testable.
- **Credential persistence.** The credential verifier lives in a dedicated
  LittleFS file `/guardian-x-admin.bin` (96-byte, padding-free record:
  magic `GXA1`, format version, iterations, salt, hash, username, CRC32),
  written atomically via a temporary file and rename. It is deliberately
  separate from the DeviceCore snapshot: the snapshot is a byte-stable,
  dual-slot migration format that is wiped by Phase 3 slot recovery, while
  the credential needs independent erase-and-recover semantics and must not
  be touched by Wi-Fi or slot-level snapshot recovery. A full LittleFS
  reformat still erases both application data and credentials, after which
  the administrator must be initialized again. Session tokens are not
  persisted at all.
- **Sessions.** Sessions are in-memory only: 256-bit tokens from the hardware
  RNG, a fixed capacity of 4 slots, and a 30-minute absolute lifetime.
  Expired slots are purged lazily and by `expireSessions()`; when all slots
  are live, the session with the earliest expiry is evicted. `logout`
  invalidates one token, `logoutAll` every token, and any password change
  invalidates every session. A reboot invalidates all sessions because none
  are persisted. Tokens are never logged.
- **Authorization boundary.** Authentication is separate from authorization:
  `authenticate()` proves an identity and returns a token, while
  `authorize(token, &principal)` is the reusable fail-closed check that
  Phase 5 HTTP routes must call per request (denied on any error, unknown or
  expired token, or uninitialized core). It returns a minimal `Principal`
  (username, session id, authentication time); there is no role-based access
  control, and Wi-Fi connectivity, IP knowledge, or setup-AP access are never
  treated as authentication.
- **Recovery.** Lost credentials are recovered by design, not by guesswork:
  the `A` serial command erases only the credential file and returns the
  device to a clean uninitialized state, after which `a` can initialize a new
  administrator. It never touches classes, students, guardians, palms, or
  dismissal data. A corrupt credential record (bad CRC, bad magic, unsupported
  version, or out-of-range parameters) is rejected at boot with
  `kCorruptRecord` and can be erased the same way. The `f` factory reset now
  explicitly clears both the network/provisioning state and the administrator
  credential while preserving application data. Wi-Fi retries, router
  outages, and repeated provisioning submissions never read or modify the
  credential record.
- **Threat model and known limitations.** The trust anchor is physical access
  to the USB serial console: anyone holding the device can erase credentials
  with `A`, so the model protects against network and setup-AP attackers, not
  against an attacker with physical console access (this matches the device's
  actual hardware, which has no reset button or secure element). The serial
  console transmits passwords in cleartext over USB; the Phase 5 HTTP login
  must use the session flow instead. Passwords are validated for length and
  printability only — no complexity or dictionary policy. PBKDF2 parameters
  have not been measured on-device; benchmark in Phase 10 before tuning.
  Sessions are lost on reboot by design.
- **Validation.** 26 native security tests plus the 12 existing Phase 2/3
  regression tests pass against freshly compiled sources (MSVC host build;
  PlatformIO's GCC-only native environment cannot compile mbedTLS on this
  machine). A Phase 4-only ESP32-S3 firmware build succeeds with RAM 38.6 %
  (126,552 B) and flash 24.6 % (823,269 B). **Hardware validation is not yet performed**:
  serial admin setup, login timing, and recovery have not been exercised on a
  connected device.

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

Phases 0–4 are implemented. Phases 5–10 remain planned and must not be
started automatically.
