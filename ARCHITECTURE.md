# GUARDIAN X Architecture

## Scope status

**IMPLEMENTED IN PHASES 0–1:** Project layout, PlatformIO configuration, a
minimal serial interface, direct SEN0677 UART operations, and SSD1309
hardware-status screens.

**IMPLEMENTED IN PHASE 2:** A storage adapter for LittleFS and a
hardware-independent application core. They persist versioned, CRC-protected
snapshots for configuration, classes, students, guardians, palm UIDs,
relationships, and dismissal records. Phase 2 does not perform sensor I/O or
biometric verification.

**IMPLEMENTED IN PHASE 3:** Device provisioning. The Phase 2 snapshot gains a
persistent provisioned/unprovisioned state plus the configured Wi-Fi SSID and
passphrase. On first boot (or after factory reset) the device opens an open
(passwordless, single-client) setup access point — SSID `GUARDIAN-X-` plus the
last four hex characters of the MAC — and serves a single-purpose embedded
setup page. Submitted credentials are validated first, then tested with a real
station-mode connection attempt (20 s timeout) before being persisted; only a
confirmed connection marks the device provisioned and restarts it into normal
mode. A failed attempt keeps the setup AP running so the user can retry.
Provisioned devices skip the AP and connect to the configured network using
non-blocking retries (30 s attempt timeout, 5 s cadence), so a temporary
router outage never re-enters setup mode and serial diagnostics stay
responsive. Recovery is layered: invalid slots fall back to the newest
CRC-valid snapshot; when no snapshot validates, the invalid slots are erased
and a fresh unprovisioned snapshot is written; when the filesystem cannot be
mounted at all (blank factory-fresh flash or unrecoverable damage), it is
reformatted. Each layer discards only data that is already unreadable and
lands in setup mode. The `f` serial command performs a factory reset that
clears only network/provisioning state and preserves all Phase 2 application
data; when the core itself is unusable, `f` reinitializes storage as a last
resort. Wi-Fi credentials are stored only in the snapshot (the Wi-Fi
library's NVS credential cache is disabled) and are never logged. Hardware
validation has **not yet been performed**.

**IMPLEMENTED IN PHASE 4:** Authentication and security. A hardware-
independent `SecurityCore` provides first-time administrator initialization,
PBKDF2-HMAC-SHA256 password hashing (100,000 iterations, 16-byte random salt,
32-byte output, via the ESP-IDF mbedTLS already present in the framework),
constant-time verification, in-memory sessions (256-bit hardware-RNG tokens,
4 slots, 30-minute lifetime, evict-oldest at capacity), logout and
logout-all, and a reusable fail-closed `authorize()` check that returns a
minimal `Principal`. The credential verifier (magic `GXA1`, format version,
iterations, salt, hash, username, CRC32) is stored in its own LittleFS file
`/guardian-x-admin.bin` written atomically via temporary-file-plus-rename —
deliberately outside the DeviceCore snapshot so slot-level snapshot recovery
cannot erase credentials and credential recovery cannot destroy application
data. A full LittleFS reformat still erases both, and requires administrator
initialization again. Admin setup and recovery run only over the physical
serial console (`a` initialize, `A` erase credentials); the open setup AP
cannot touch credentials. The `f` factory reset now clears network state and credentials
while preserving all Phase 2 application data. Sessions are memory-only, so
a reboot invalidates them all. Passwords, hashes, and session tokens are
never logged or persisted in plaintext. No HTTP API exists yet; the
authorization boundary is what Phase 5 routes will call.

**PLANNED:** The local API/HTTP server, application workflow, and web UI
components below remain target architecture only. The device does not run a
dismissal workflow. The Phase 3 setup page is a single-purpose provisioning
interface — it is not the Phase 5 API or the Phase 6 web UI.

## Target architecture

```text
                    GUARDIAN X
                         |
                    ESP32-S3
                         |
        +----------------+----------------+
        |                |                |
        v                v                v
     SEN0677          SSD1309          Storage
        |                |                |
        +----------------+----------------+
                         |
                         v
                    Core System
                         |
              +----------+----------+
              |                     |
              v                     v
          Local API              Workflow
              |                     |
              v                     v
           Web UI             Dismissal Engine
```

This is the target architecture. Phases 0–4 establish the project, hardware
foundation, local core, provisioning, and authentication/security layers from
which later phases can implement and validate the remaining layers.

## Future hardware pin reference

These pins are implemented for the Phase 1 hardware foundation. Phase 2 local
storage and data models add no hardware wiring. Phase 3 uses the ESP32-S3 radio
(setup AP when unprovisioned, station mode when provisioned) and adds no wired
peripherals or pin changes. Phase 4 adds no hardware: authentication uses the
existing serial console as its physical trust anchor and the ESP32-S3 hardware
random generator for salts and session tokens. No application workflow or HTTP
API exists yet.

| Peripheral | Signal | ESP32-S3 mapping |
| --- | --- | --- |
| SEN0677 | TX | GPIO16 (ESP32 RX) |
| SEN0677 | RX | GPIO17 (ESP32 TX) |
| SEN0677 UART | Baud rate | 115200 |
| SSD1309 | SCK | GPIO12 |
| SSD1309 | MOSI | GPIO11 |
| SSD1309 | CS | GPIO10 |
| SSD1309 | DC | GPIO9 |
| SSD1309 | RST | GPIO8 |

Specifically, ESP32 GPIO17 transmits to the SEN0677 RX pin, and ESP32 GPIO16 receives from the SEN0677 TX pin. Phase 1 uses this UART and the documented SSD1309 SPI pins only.

## Layered responsibilities

Layers 1–3 are implemented by Phases 0–4; layers 4–7 remain planned for later
validated phases.

### 1. Hardware Layer

Responsibilities: SEN0677, SSD1309, GPIO, UART, and SPI. **Implemented**
(Phases 0–1).

### 2. Storage Layer

Responsibilities: persistent device configuration, local application data, data versioning, and recovery. **Implemented** (Phase 2).

### 3. Authentication Layer

Responsibilities: admin authentication, password hashing, session management, and authorization. **Implemented** (Phase 4): the `SecurityCore` provides
first-time administrator initialization, PBKDF2-HMAC-SHA256 password
verifiers, in-memory sessions with bounded capacity and expiry, and reusable
fail-closed authorization checks; see the scope status at the top of this
document.

### 4. API Layer

Responsibilities: HTTP endpoints, request validation, authentication checks, and response serialization.

### 5. Application Layer

Responsibilities: classes, students, guardians, guardian palms, dismissal requests, and the dismissal state machine.

### 6. UI Layer

Responsibilities: the local web dashboard, device interface, management screens, and dismissal interface.

### 7. Hardware UX Layer

Responsibilities: OLED states plus enrollment, verification, dismissal, and error feedback.
