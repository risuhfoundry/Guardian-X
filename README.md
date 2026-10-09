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
unprovisioned state. Hardware validation is **not yet performed**.

**IMPLEMENTED:** Phase 4 — Authentication & Security. A first-time
administrator account is initialized from the serial console only (`a`
command) — never through the open setup AP — with validated inputs, no
default password, and re-initialization rejected. The password verifier is
PBKDF2-HMAC-SHA256 (100,000 iterations, 16-byte random salt, 32-byte output)
computed with the ESP-IDF mbedTLS 2.28.x implementation; salts and session
tokens come from the ESP32-S3 hardware random generator, comparisons are
constant-time, and plaintext passwords are never stored or logged. The
credential record lives in a dedicated LittleFS file (`/guardian-x-admin.bin`,
CRC-protected, atomic tmp+rename write) separate from the application
snapshot. Sessions are in-memory only (4 slots, 256-bit tokens, 30-minute
lifetime, evict-oldest at capacity), so reboot invalidates them; `logout`,
password changes, factory reset, and credential recovery all invalidate
sessions. Authorization is a reusable fail-closed check (`authorize`) that
Phase 5 routes must invoke per token — Wi-Fi connectivity, IP knowledge, or
setup-AP access are never treated as authentication. `A` erases only the
credential record; the factory reset `f` clears network config plus
credentials while preserving all Phase 2 application data. Hardware
validation is **not yet performed**.

**PLANNED:** The local API/web UI, palm enrollment workflow, and dismissal
workflows remain scheduled for later validated phases.

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
Phase 4 adds no hardware at all: authentication uses the existing serial
console as its physical trust anchor and the ESP32-S3 hardware random
generator for salts and session tokens.

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

| Command | Operation |
| --- | --- |
| `l` | List registered SEN0677 user IDs. |
| `e <name>` | Enroll a palm/user with the supplied 1–31 character test name. |
| `r` | Run one palm-recognition attempt. |
| `d <uid>` | Delete a registered SEN0677 user by its numeric UID. |
| `a <user> <pw>` | Initialize the administrator account (first time only; rejected if an administrator already exists). |
| `login <user> <pw>` | Verify administrator credentials and create a session. Passwords are never echoed or logged. |
| `logout` | Invalidate all administrator sessions. |
| `s` | Security status: whether the administrator account exists and how many sessions are active. |
| `f` | Factory reset: clears network/provisioning state **and** the administrator credential record (keeps classes, students, guardians, palms, dismissals) and restarts into setup mode. If storage is unreadable, reinitializes it to a fresh unprovisioned state instead. |
| `A` | Admin recovery: erases **only** the administrator credential record (case-sensitive on purpose, so a mistyped `a` cannot trigger it). Application data is untouched. |
| `h` or `?` | Show the command help. |

The `l`, `e`, `r`, and `d` commands call the live SEN0677 APIs and must be
tested with connected hardware after firmware upload. The Phase 4 commands
exercise the security core (PBKDF2 verification, sessions, recovery) over
serial; they too are hardware-validated only after upload — see Security
below.

## Security

**Administrator initialization.** The administrator account is created with
the serial `a <username> <password>` command — serial only, never via the
Phase 3 setup access point, which stays a single-purpose provisioning page.
There is no hardcoded or default password. Usernames are 1–32 printable
characters without spaces; passwords are 8–64 printable characters. Repeated
initialization is rejected (`already initialized`), so an existing credential
can only be replaced after deliberate recovery (`A`) or factory reset (`f`).
A storage failure during initialization reports `storage failure` and leaves
the device uninitialized.

**Password hashing.** Passwords are verified against PBKDF2-HMAC-SHA256 with
100,000 iterations, a 16-byte random salt, and a 32-byte output, computed by
the maintained mbedTLS 2.28.x implementation bundled with the ESP-IDF
framework (hardware-accelerated where available). The record stores the
format version ("GXA1"), the iteration count, salt, hash, and username so
parameters can evolve; verifier comparison is constant-time. Plaintext
passwords are never stored, logged, or echoed. Each credential generation
(initialization, password change) draws a fresh salt.

**Randomness.** Salts and 256-bit session tokens come from the ESP32-S3
hardware random generator (`esp_fill_random`), behind a small injectable
`SecureRandom` interface so tests can inject deterministic bytes and failure.

**Credential persistence.** The 96-byte credential record lives in its own
LittleFS file `/guardian-x-admin.bin`, written atomically (tmp + rename) with
a CRC32 that is validated on load. It is deliberately separate from the
dual-slot application snapshot: credentials must survive the snapshot's
slot-level recovery and must never be swept into snapshot rollbacks, and no
credential material or session token ever enters the application snapshot.

**Sessions.** Sessions are in-memory only: 4 fixed slots, 256-bit random
tokens, a 30-minute absolute lifetime, constant-time token comparison, lazy
expiry plus an explicit `expireSessions()` hook, evict-oldest at capacity,
and `logout` (all sessions). Memory-only storage means a reboot or power
cycle invalidates every session by design. A password change invalidates all
sessions. Tokens are never logged or persisted.

**Authorization boundary.** Authentication (`authenticate`) is separate from
authorization (`authorize`). `authorize` is a fail-closed check: any error,
unknown token, expired session, or unready core denies. It returns a minimal
`Principal` (username, session id, authentication time) for Phase 5's HTTP
routes, which must call it per request. Wi-Fi connectivity, knowing the
device IP, or access to the setup AP is never treated as authentication.

**Recovery.** Lost administrator credentials are recovered over serial with
`A` (erase the credential record only; all application data is preserved) or
`f` (factory reset: network configuration plus credentials, application data
preserved). A corrupt credential record is reported at boot and the same `A`
path recovers it. Wi-Fi retries and router outages never touch the credential
record, and re-submitting the provisioning page never overwrites the
administrator account.

**Threat model and known limitations.** The serial console is the physical
trust anchor: whoever can open the USB serial port can run `A` and reclaim
the device, so physical possession of the device implies administrative
recovery. This matches the device's actual hardware — there is no reset
button, secure element, or trusted display, and none is invented. Passwords
typed on the serial console travel in cleartext over USB, so provisioning
should be done on a trusted machine. There is deliberately no password
complexity policy beyond length/printability, no rate limiting beyond PBKDF2
cost (on-device timing is not yet measured), and no persistent bearer tokens.
The setup AP remains open (passwordless) until provisioning succeeds by Phase
3 design; it exposes no administrator or application functionality.

**Validation.** 26 security tests plus 12 Phase 2/3 regression tests pass on
a fresh native host build (MSVC; see `test/native_test_build.sh` — the host
build compiles mbedTLS 2.28.8 sources, which is why the PlatformIO `native`
environment skips the security suite). A Phase 4-only ESP32-S3 firmware build
succeeds: RAM 38.6 % (126,552 B), flash 24.6 % (823,269 B) — the increase
over the Phase 3 baseline is the PBKDF2/session/security code plus the serial
command surface. **Hardware validation has not yet been performed**: the on-device
flow (account creation, login latency, sessions across reboots, `A`/`f`
recovery, and Phase 1–3 regressions) must still be verified against a
connected ESP32-S3.

## Project Structure

- `src/` — Firmware source. Phase 1 owns the SSD1309/SEN0677 diagnostics;
  Phase 2 adds the storage adapter and device core; Phase 3 adds the
  provisioning service (setup AP + embedded setup page); Phase 4 adds the
  security core, the credential-record storage adapter, and the
  ESP32-specific security adapters (`guardian_x/`).
- `include/` — Domain models, storage boundary, and device-core headers, plus
  the Phase 3 provisioning service interface and the Phase 4 security API
  (`guardian_x/security.h`).
- `lib/` — Reserved for future project-local libraries.
- `test/` — Native validation tests: the Phase 2/3 device-core suite and the
  Phase 4 security suite, built fresh on the host by
  `test/native_test_build.sh` (which compiles the mbedTLS 2.28.8 sources,
  since the PlatformIO native environment ships no mbedTLS).
- `data/` — Reserved for future PlatformIO data assets when a validated phase
  requires them.
- `reference/` — Hardware reference implementations. `Code.ino`, if supplied, is preserved as reference-only material.
- `docs/` — Supporting documentation for future phases.
- `ARCHITECTURE.md` — Target architecture and documented pin mapping.
- `PHASES.md` — Validated, prerequisite-ordered delivery roadmap.

## Development Method

GUARDIAN X is implemented phase-by-phase. Each phase must meet its completion criteria and be validated before work begins on the next one. Later-phase functionality must not be introduced before its prerequisites are validated.
