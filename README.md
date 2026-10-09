# GUARDIAN X

GUARDIAN X is a local-first palm-based guardian/student dismissal system built around an ESP32-S3 and a SEN0677 palm sensor.

## Current Status

**IMPLEMENTED:** Phase 1 — Hardware Foundation. The firmware initializes the
SSD1309 and SEN0677, then provides a Serial Monitor test interface for direct
sensor operations.

**IMPLEMENTED:** Phase 2 — Local Storage & Device Core. The firmware persists
versioned, CRC-validated local data for configuration, classes, students,
guardians, guardian palm UIDs, relationships, and dismissal records. This core
has no sensor I/O and does not perform biometric verification.

**IMPLEMENTED:** Phase 3 — Device Provisioning. The snapshot gains a persistent
provisioned/unprovisioned state plus the configured Wi-Fi SSID/passphrase. An
unprovisioned device opens an open setup access point (`GUARDIAN-X-` + last
four hex characters of the MAC) serving an embedded setup page. Credentials
are validated, then tested with a real connection attempt (20 s timeout)
before being persisted; only a confirmed connection marks the device
provisioned and restarts it into normal mode. Provisioned devices reconnect
non-blockingly (30 s attempt timeout, 5 s cadence), so router outages never
clear provisioning. Unreadable storage recovers automatically to a fresh
unprovisioned state. The `f` serial command factory-resets only
network/provisioning state (and reinitializes unreadable storage as a last
resort). Hardware validation is **not yet performed**.

**PLANNED:** Security, the local API/web UI, palm enrollment workflow, and
dismissal workflows remain scheduled for later validated phases.

## Development Stack

- VS Code
- PlatformIO
- Arduino framework
- ESP32-S3
- C++

## Hardware

- ESP32-S3
- DFRobot SEN0677
- SSD1309 128x64 SPI OLED

The Phase 1 hardware layer initializes only these two peripherals. Phase 2 adds
LittleFS-backed local storage but no Wi-Fi, authentication, web/API
functionality, or dismissal workflow. Phase 3 uses the ESP32-S3 radio for
provisioning only: a setup access point when unprovisioned and a station-mode
connection to the configured network when provisioned. No pin mapping changes.

## Build

From the project root, run:

```sh
pio run
```

## Upload

When a supported ESP32-S3 is connected, run:

```sh
pio run -t upload
```

## Serial Monitor

```sh
pio device monitor -b 115200
```

After connecting at 115200 baud with newline line endings, use:

| Command | Hardware operation |
| --- | --- |
| `l` | List registered SEN0677 user IDs. |
| `e <name>` | Enroll a palm/user with the supplied 1–31 character test name. |
| `r` | Run one palm-recognition attempt. |
| `d <uid>` | Delete a registered SEN0677 user by its numeric UID. |
| `f` | Factory reset: clears only network/provisioning state (keeps classes, students, guardians, palms, dismissals) and restarts into setup mode. If storage is unreadable, reinitializes it to a fresh unprovisioned state instead. |
| `h` or `?` | Show the command help. |

These commands call the live SEN0677 APIs. They must be tested with connected hardware after firmware upload.

## Project Structure

- `src/` — Firmware source. Phase 1 owns the SSD1309/SEN0677 diagnostics;
  Phase 2 adds the storage adapter and device core; Phase 3 adds the
  provisioning service (setup AP + embedded setup page).
- `include/` — Domain models, storage boundary, and device-core headers, plus
  the Phase 3 provisioning service interface.
- `lib/` — Reserved for future project-local libraries.
- `test/` — Native device-core validation tests.
- `data/` — Reserved for future PlatformIO data assets when a validated phase
  requires them.
- `reference/` — Hardware reference implementations. `Code.ino`, if supplied, is preserved as reference-only material.
- `docs/` — Supporting documentation for future phases.
- `ARCHITECTURE.md` — Target architecture and documented pin mapping.
- `PHASES.md` — Validated, prerequisite-ordered delivery roadmap.

## Development Method

GUARDIAN X is implemented phase-by-phase. Each phase must meet its completion criteria and be validated before work begins on the next one. Later-phase functionality must not be introduced before its prerequisites are validated.
