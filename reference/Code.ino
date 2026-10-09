/*
  PALM RECOGNITION DOOR LOCK - MAIN FIRMWARE
  DFRobot AI10 (SEN0677) + ESP32

  Complete application: palm unlock, enrollment by button or web UI,
  user management, and the feedback patterns from PROJECT_PLAN.md.

  =====================================================================
  HOW ENROLLMENT WORKS
  =====================================================================
  Two alternative triggers - you never do both in sequence:

    WEB UI (normal)   Log in, type a name, click Enroll.
    BUTTON (fallback) Hold the REGISTRATION button (GPIO5) for 1.5s.
                      Auto-named; rename later from the web page.

  EITHER WAY, AN ADMIN PALM MUST THEN AUTHORISE AT THE DOOR. The button
  or the web page only opens the request - the palm approves it. So a
  stolen dashboard login cannot enroll anyone, and neither can somebody
  who simply reaches the button.

  The EXIT button (GPIO6) is unlock-only on the S2 pin map. On the old
  board, which has just one button, a 3s hold on it starts enrollment
  instead.

  The web dashboard itself is behind a login - see ADMIN_USER /
  ADMIN_PASS at the top of this file.

  BOOTSTRAP: if the sensor has zero users, the first enrollment needs no
  admin and is registered AS admin. That is the only unauthenticated
  enrollment this firmware ever permits.

  =====================================================================
  CONCURRENCY - THE RULE THAT KEEPS THIS RELIABLE
  =====================================================================
  loop() is the SOLE owner of Serial1 / the AI10. Web handlers run on the
  AsyncTCP task and must never call ai10.* - they push a command onto a
  FreeRTOS queue and return immediately. Two tasks writing the sensor's
  UART corrupts the protocol mid-packet: it works on the bench and fails
  intermittently on the door.

  =====================================================================
  WIRING
  =====================================================================
  OLED is a 4-wire SPI SSD1309 panel. Connect GND to ESP32-S2 GND,
  CLK/D0 to GPIO12, MOSI/D1/SDA to GPIO11, CS to GPIO10, DC/A0 to GPIO9,
  and RES/RST to GPIO8. Use the voltage specified by the OLED module;
  ESP32 signal pins are 3.3V.

  The AI10 sensor needs 5-12V @ ~330mA from its own feed - it cannot run
  off the ESP32's 3.3V pin.

  =====================================================================
  SERIAL COMMANDS (115200 baud, send Newline)
  =====================================================================
    l            list users
    e <name>     enroll (admin palm required unless sensor is empty)
    b <name>     enroll via the button path (no admin palm)
    d <uid>      delete user
    r <uid> <nm> rename (stored on the ESP32, not the sensor)
    x <uid>      block  (keeps the palm enrolled, refuses entry)
    p <uid>      permit (unblock)
    u            unlock now
    w <ssid> <pw>  set WiFi credentials and reconnect
    i            network info
    ?            help
*/

#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <SPI.h>
#include <U8g2lib.h>
#include "DFRobot_AI10.h"

// =====================================================================
//  REFERENCE-ONLY WEB DASHBOARD LOGIN
// =====================================================================
// This legacy sketch is not built by GUARDIAN X.  If it is ever compiled
// independently, provide all values as compiler macros; never commit them.
//
#if !defined(GUARDIAN_X_REFERENCE_ADMIN_USER) || \
    !defined(GUARDIAN_X_REFERENCE_ADMIN_PASS) || \
    !defined(GUARDIAN_X_REFERENCE_AP_SSID) || \
    !defined(GUARDIAN_X_REFERENCE_AP_PASS)
#error "Reference credentials must be supplied externally; do not commit them"
#endif

const char *ADMIN_USER = GUARDIAN_X_REFERENCE_ADMIN_USER;
const char *ADMIN_PASS = GUARDIAN_X_REFERENCE_ADMIN_PASS;
// =====================================================================

// =====================================================================
// BOARD SELECT -- this sketch is configured for the ESP32-S2 DevKitC-1.
// Set 0 only when using the legacy classic-ESP32 TechiesMS board wiring.
#define BOARD_NEW_PCB 1

// OLED SPI pins on ESP32-S2 DevKitC-1, as requested.
#define OLED_SCK  12  // Display SCL/CLK
#define OLED_MOSI 11  // Display SDA/DIN
#define OLED_CS   10
#define OLED_DC    9
#define OLED_RST   8

#if BOARD_NEW_PCB

// ESP32-S2 DevKitC-1 peripheral wiring. GPIO18 drives the onboard RGB LED;
// GPIO26-32 are used by flash/PSRAM, so this map avoids those pins.
const uint8_t SENSOR_RX_PIN = 17; // ESP32 -> sensor RX
const uint8_t SENSOR_TX_PIN = 16; // sensor TX -> ESP32
const uint8_t RELAY_PIN     = 13;
const uint8_t BUZZER_PIN    = 14;
const uint8_t LED_RED       = 15;
const uint8_t LED_GREEN     = 7;
const uint8_t BTN_REX       = 6;
const uint8_t BTN_ENROLL    = 5;
const uint8_t TAMPER_PIN    = 4;  // normally-closed loop to GND
const uint8_t REED_PIN      = 3;  // optional door sensor
const bool USE_ENROLL_BUTTON = true;
const bool RELAY_ACTIVE_HIGH = true;

#else

const uint8_t SENSOR_RX_PIN = 21;
const uint8_t SENSOR_TX_PIN = 19;
const uint8_t RELAY_PIN     = 33;
const uint8_t BUZZER_PIN    = 26;
const uint8_t LED_RED       = 13;
const uint8_t LED_GREEN     = 14;
const uint8_t BTN_REX       = 32;
const uint8_t BTN_ENROLL    = 4;  // unused on this board
const uint8_t TAMPER_PIN    = 27;
const uint8_t REED_PIN      = 25;

const bool USE_ENROLL_BUTTON = false; // only one button here

// The techiesms board sinks the opto through the GPIO: LOW energises.
const bool RELAY_ACTIVE_HIGH = false;

#endif

// Does energising the coil RELEASE the door? True for both sane wirings:
//   lock on the NO contact + fail-secure strike (power to open), or
//   lock on the NC contact + magnetic lock      (power to hold shut).
// Set false only if you deliberately wired the opposite sense.
//
// WARNING - the combination to avoid: a fail-secure strike on the NC
// contact. That leaves the lock POWERED whenever the board is idle, so
// the door is openable at rest and no firmware setting can fix it. This
// is the NO/NC question on J6 that still needs checking on the sheet.
const bool ENERGIZE_TO_UNLOCK = true;

// ---------------- Behaviour ----------------
const uint32_t UNLOCK_MS      = 5000;
const uint8_t  RECOG_TIMEOUT_S = 3;    // min 3 per DFRobot docs
const uint8_t  ADMIN_WAIT_S   = 15;    // how long to wait for the admin palm
const uint8_t  ENROLL_S       = 15;    // how long to wait for the new palm
const uint32_t LONG_PRESS_MS  = 3000; // exit button, when it doubles as enroll
const uint32_t ENROLL_PRESS_MS = 1500; // hold on the dedicated enroll button

// These two only exist on the new PCB. Leave false on the current board,
// where the pins float and would read as permanently tripped/open.
// *** Only enable USE_TAMPER once the switch is physically fitted. ***
// The tamper loop is normally CLOSED, so an unfitted input floats HIGH
// through the pull-up - which reads as TAMPERED. Turn this on with no
// switch wired and the lock will refuse every unlock, permanently.
const bool USE_TAMPER = false;
const bool USE_REED   = false;

// 2.42-inch SSD1309 SPI panel, 128x64. If it fails to initialize, OLED
// drawing is skipped and the lock application continues without a display.
const bool    USE_OLED  = true;
const uint8_t OLED_W    = 128;
const uint8_t OLED_H    = 64;

// An admin palm must authorise every enrolment, whichever way it was
// started - registration button or web dashboard. Pressing the button
// only opens the request; the palm is what approves it.
// (The bootstrap case still applies: on a completely empty sensor the
// first enrolment needs no admin and becomes the admin.)
const bool REQUIRE_ADMIN_FOR_BUTTON_ENROLL = true;

// Fallback access point, used when WiFi credentials are unset or fail.
const char *AP_SSID = GUARDIAN_X_REFERENCE_AP_SSID;
const char *AP_PASS = GUARDIAN_X_REFERENCE_AP_PASS;

DFRobot_AI10_UART ai10(&Serial1, 115200, SENSOR_RX_PIN, SENSOR_TX_PIN);
Preferences prefs;

// The ESP32 core's own WebServer, not ESPAsyncWebServer. The popular
// lacamera fork of the async library calls mbedtls_md5_*_ret(), which
// ESP-IDF 5.x removed, so it no longer builds against core 3.x.
//
// WebServer is synchronous, so handleClient() runs in its own FreeRTOS
// task. This keeps the dashboard responsive while the main loop waits on
// sensor recognition, including on the single-core ESP32-S2.
WebServer server(80);
TaskHandle_t webTaskHandle = nullptr;

U8G2_SSD1309_128X64_NONAME0_F_4W_HW_SPI oled(U8G2_R0, OLED_CS, OLED_DC, OLED_RST);
bool oledOk = false;
char gIp[24] = "-";

// ---------------- Shared state ----------------
// Written by loop(), read by the AsyncTCP task. Guarded by stMux.

const uint8_t MAX_USERS = 50;

struct UserRec {
  uint16_t uid;
  char     name[32];
  bool     blocked;
  bool     admin;
};

SemaphoreHandle_t stMux;
UserRec  gUsers[MAX_USERS];
uint8_t  gUserN     = 0;
char     gState[16] = "starting";
char     gLastMsg[96] = "";
bool     gSensorOk  = false;

#define ST_LOCK()   xSemaphoreTake(stMux, portMAX_DELAY)
#define ST_UNLOCK() xSemaphoreGive(stMux)

void setState(const char *s) {
  ST_LOCK();
  strncpy(gState, s, sizeof(gState) - 1);
  gState[sizeof(gState) - 1] = 0;
  ST_UNLOCK();
}

void setMsg(const char *fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  ST_LOCK();
  strncpy(gLastMsg, buf, sizeof(gLastMsg) - 1);
  gLastMsg[sizeof(gLastMsg) - 1] = 0;
  ST_UNLOCK();

  Serial.print(F("    "));
  Serial.println(buf);
}

// ---------------- Command queue ----------------
// The ONLY channel from the web task into the sensor.

enum CmdType : uint8_t {
  CMD_ENROLL_WEB = 1, // needs admin palm
  CMD_ENROLL_BTN,     // no admin palm
  CMD_DELETE,
  CMD_RENAME,
  CMD_BLOCK,
  CMD_PERMIT,
  CMD_UNLOCK,
};

struct Cmd {
  uint8_t  type;
  uint16_t uid;
  char     name[32];
};

QueueHandle_t cmdQueue;

bool pushCmd(uint8_t type, uint16_t uid, const char *name) {
  Cmd c;
  memset(&c, 0, sizeof(c));
  c.type = type;
  c.uid  = uid;
  if (name) strncpy(c.name, name, sizeof(c.name) - 1);
  return xQueueSend(cmdQueue, &c, 0) == pdTRUE;
}

// ---------------- Lock state ----------------
bool     doorUnlocked  = false;
uint32_t unlockStartMs = 0;

// ---------------------------------------------------------------------
// NVS name map
//
// getUserInfo() is protected in the library and there is no rename call,
// so getAllUserIDs() gives us UIDs and nothing else. The ESP32 therefore
// keeps its own UID->name map. It is self-healing: every recognition
// returns userName, and we write it back, so the map fills in over time
// even for users enrolled before this firmware existed.
// ---------------------------------------------------------------------

void keyFor(char *out, size_t n, char prefix, uint16_t uid) {
  snprintf(out, n, "%c%u", prefix, uid);
}

String nameOf(uint16_t uid) {
  char k[12];
  keyFor(k, sizeof(k), 'n', uid);
  return prefs.getString(k, "");
}

void setNameOf(uint16_t uid, const char *name) {
  char k[12];
  keyFor(k, sizeof(k), 'n', uid);
  prefs.putString(k, name);
}

bool isBlocked(uint16_t uid) {
  char k[12];
  keyFor(k, sizeof(k), 'b', uid);
  return prefs.getBool(k, false);
}

void setBlocked(uint16_t uid, bool v) {
  char k[12];
  keyFor(k, sizeof(k), 'b', uid);
  prefs.putBool(k, v);
}

bool isAdminUid(uint16_t uid) {
  char k[12];
  keyFor(k, sizeof(k), 'a', uid);
  return prefs.getBool(k, false);
}

void setAdminUid(uint16_t uid, bool v) {
  char k[12];
  keyFor(k, sizeof(k), 'a', uid);
  prefs.putBool(k, v);
}

void forgetUid(uint16_t uid) {
  char k[12];
  keyFor(k, sizeof(k), 'n', uid); prefs.remove(k);
  keyFor(k, sizeof(k), 'b', uid); prefs.remove(k);
  keyFor(k, sizeof(k), 'a', uid); prefs.remove(k);
}

// ---------------------------------------------------------------------
// Feedback - LEDs and buzzer
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// OLED
//
// Only loop() ever draws. The web task must never touch the display -
// same single-owner rule as the sensor UART, for the same reason.
// No default arguments anywhere here: the Arduino builder hoists
// generated prototypes, and defaults in the definition then clash.
// ---------------------------------------------------------------------

void oledInit() {
  if (!USE_OLED) return;
  // Set ESP32-S2 hardware SPI pins explicitly. The OLED has no MISO.
  SPI.begin(OLED_SCK, -1, OLED_MOSI, OLED_CS);
  oledOk = oled.begin();
  if (!oledOk) {
    Serial.println(F("OLED: init failed - continuing without it"));
    return;
  }
  oled.setContrast(255);
  oled.setFontMode(1);
  oled.setDrawColor(1);
  oled.clearBuffer();
  oled.sendBuffer();
}

void oledSetFont(uint8_t size) {
  if (size >= 3)      oled.setFont(u8g2_font_helvB18_tf);
  else if (size == 2) oled.setFont(u8g2_font_helvB12_tf);
  else                oled.setFont(u8g2_font_6x10_tf);
}

// U8g2 cursor Y coordinates are text baselines.
void oledLines(const char *l1, const char *l2, const char *l3) {
  if (!oledOk) return;
  oled.clearBuffer();
  oled.setDrawColor(1);
  oled.setFont(u8g2_font_6x10_tf);
  oled.setCursor(0, 12); oled.print(l1);
  oled.setCursor(0, 33); oled.print(l2);
  oled.setCursor(0, 54); oled.print(l3);
  oled.sendBuffer();
}

void oledCentered(const char *text, uint8_t size, int16_t y) {
  if (!oledOk) return;
  oledSetFont(size);
  int16_t w = (int16_t)oled.getStrWidth(text);
  int16_t x = ((int16_t)OLED_W - w) / 2;
  if (x < 0) x = 0;
  oled.setCursor(x, y);
  oled.print(text);
}

uint8_t oledFitSize(const char *text, uint8_t maxSize) {
  for (uint8_t s = maxSize; s > 1; s--) {
    oledSetFont(s);
    if (oled.getStrWidth(text) <= OLED_W - 4) return s;
  }
  return 1;
}

void oledBanner(const char *big, const char *sub) {
  if (!oledOk) return;
  oled.clearBuffer();
  oled.setDrawColor(1);
  oledCentered(big, 2, 20);
  oledCentered(sub, oledFitSize(sub, 3), 54);
  oled.sendBuffer();
}

void oledIdle() {
  if (!oledOk) return;
  oled.clearBuffer();
  oled.setDrawColor(1);
  oledCentered("Ready", 2, 25);
  oledCentered("Present your palm", 1, 44);
  oledCentered(gIp, 1, 61);
  oled.sendBuffer();
}
void beep(uint16_t onMs, uint8_t times = 1, uint16_t gapMs = 80) {
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(onMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i + 1 < times) delay(gapMs);
  }
}

void leds(bool red, bool green) {
  digitalWrite(LED_RED, red ? HIGH : LOW);
  digitalWrite(LED_GREEN, green ? HIGH : LOW);
}

void sigHeartbeat()   { leds(false, true); delay(25); leds(false, false); }
void sigGranted()     { leds(false, true); beep(120); }
void sigDenied()      { leds(true, false); beep(60, 3); delay(1200); leds(false, false); }
void sigEnrollArmed() { beep(80, 2); }
void sigEnrollOk()    { leds(false, true); beep(80, 2); delay(600); leds(false, false); }
void sigEnrollFail()  { leds(true, false); beep(500); delay(600); leds(false, false); }

void blinkWhile(uint32_t ms, uint16_t periodMs, bool red, bool green) {
  uint32_t t0 = millis();
  bool on = false;
  while (millis() - t0 < ms) {
    on = !on;
    leds(red && on, green && on);
    delay(periodMs / 2);
  }
  leds(false, false);
}

// ---------------------------------------------------------------------
// Lock control
// ---------------------------------------------------------------------

// Single place that turns "should the door be open?" into a pin level,
// combining the board's relay polarity with the lock's sense. If either
// assumption turns out wrong, this is a one-line fix rather than a hunt.
void writeLock(bool open) {
  bool energize = ENERGIZE_TO_UNLOCK ? open : !open;
  digitalWrite(RELAY_PIN, energize == RELAY_ACTIVE_HIGH ? HIGH : LOW);
}

void unlockDoor(const char *reason) {
  writeLock(true);
  doorUnlocked  = true;
  unlockStartMs = millis();
  setState("unlocked");
  oledBanner("WELCOME", reason);
  sigGranted();
  setMsg("DOOR UNLOCKED (%s)", reason);
}

void lockDoor() {
  writeLock(false);
  leds(false, false);
  doorUnlocked = false;
  setState("idle");
  oledIdle();
  setMsg("door locked");
}

// ---------------------------------------------------------------------
// Sensor operations - ONLY ever called from loop()
// ---------------------------------------------------------------------

void refreshUsers() {
  sAllUserID_t all = ai10.getAllUserIDs();
  if (all.result != eSuccess) {
    setMsg("could not read user list (code %u)", all.result);
    return;
  }

  ST_LOCK();
  gUserN = 0;
  for (uint16_t i = 0; i < all.userNum && gUserN < MAX_USERS; i++) {
    uint16_t uid = all.UIDS[i];
    gUsers[gUserN].uid     = uid;
    gUsers[gUserN].blocked = isBlocked(uid);
    gUsers[gUserN].admin   = isAdminUid(uid);
    String n = nameOf(uid);
    if (n.length() == 0) snprintf(gUsers[gUserN].name, 32, "User%u", uid);
    else                 strncpy(gUsers[gUserN].name, n.c_str(), 31);
    gUsers[gUserN].name[31] = 0;
    gUserN++;
  }
  ST_UNLOCK();
}

uint16_t userCount() {
  ST_LOCK();
  uint16_t n = gUserN;
  ST_UNLOCK();
  return n;
}

// Runs the whole enrollment sequence. Blocking by design - it owns the
// sensor for its duration, which is exactly why it lives here and not in
// a web handler.
void doEnroll(const char *name, bool needAdmin) {
  bool bootstrap = (userCount() == 0);

  if (bootstrap) {
    needAdmin = false;
    setMsg("sensor empty - first user, enrolling as ADMIN");
  }

  if (needAdmin) {
    setState("enroll-admin");
    setMsg("present an ADMIN palm to authorise");
    oledLines("ENROLLMENT", "Present an ADMIN", "palm to authorise");
    sigEnrollArmed();
    leds(true, true);

    sRecognitionData_t r = ai10.getRecognitionResult(ADMIN_WAIT_S);
    leds(false, false);

    bool ok = (r.result == eSuccess && r.type == ePalm &&
               (r.userData.admin == eAdmin || isAdminUid(r.userData.UID)));
    if (!ok) {
      setState("idle");
      if (r.result == eSuccess) {
        setMsg("DENIED - that palm is not an admin");
        oledBanner("DENIED", "not an admin");
      } else {
        setMsg("enrollment cancelled - no admin palm seen");
        oledBanner("CANCELLED", "no admin palm");
      }
      sigEnrollFail();
      oledIdle();
      return;
    }
    setMsg("admin %u authorised", r.userData.UID);
  }

  setState("enroll-window");
  setMsg("present the NEW palm now (~15cm)");
  oledLines("ENROLLMENT", "Present the NEW", "palm now, ~15cm");
  beep(80, 2);
  leds(false, true);

  uint8_t role = bootstrap ? eAdmin : eNormal;
  sUserData_t u = ai10.enrollUser(role, name, ENROLL_S);
  leds(false, false);
  setState("idle");

  if (u.result == eSuccess) {
    setNameOf(u.UID, name);
    if (bootstrap) setAdminUid(u.UID, true);
    refreshUsers();
    oledBanner("ENROLLED", name);
    sigEnrollOk();
    setMsg("enrolled '%s' as UID %u (%s)", name, u.UID,
           u.type == ePalm ? "palm" : (u.type == eFace ? "FACE - move closer" : "other"));
    delay(1200);
    oledIdle();
  } else {
    oledBanner("FAILED", "see serial / web");
    sigEnrollFail();
    switch (u.result) {
      case eFailedTimeout:      setMsg("enroll timed out - nothing seen"); break;
      case eFailedFaceEnrolled: setMsg("already enrolled"); break;
      case eFailedMaxUser:      setMsg("sensor storage full"); break;
      case eFailedCamera:       setMsg("CAMERA ERROR - check the sensor's supply"); break;
      default:                  setMsg("enroll failed, code %u", u.result); break;
    }
    delay(1200);
    oledIdle();
  }
}

void doDelete(uint16_t uid) {
  if (ai10.deleteUser(uid)) {
    forgetUid(uid);
    refreshUsers();
    setMsg("deleted UID %u", uid);
    beep(80, 2);
  } else {
    setMsg("could not delete UID %u", uid);
    sigEnrollFail();
  }
}

// Takes plain scalars rather than the Cmd struct: Arduino hoists generated
// prototypes above the struct definition, so a custom type here fails to
// compile for reasons that have nothing to do with the logic.
void executeCmd(uint8_t type, uint16_t uid, const char *name) {
  switch (type) {
    case CMD_ENROLL_WEB: doEnroll(name, true);  break;
    case CMD_ENROLL_BTN: doEnroll(name, REQUIRE_ADMIN_FOR_BUTTON_ENROLL); break;
    case CMD_DELETE:     doDelete(uid);         break;
    case CMD_RENAME:
      setNameOf(uid, name);
      refreshUsers();
      setMsg("renamed UID %u to '%s'", uid, name);
      break;
    case CMD_BLOCK:
      setBlocked(uid, true);  refreshUsers();
      setMsg("blocked UID %u", uid);
      break;
    case CMD_PERMIT:
      setBlocked(uid, false); refreshUsers();
      setMsg("permitted UID %u", uid);
      break;
    case CMD_UNLOCK:
      unlockDoor("web/serial command");
      break;
  }
}

// ---------------------------------------------------------------------
// Recognition
// ---------------------------------------------------------------------

void recognitionCycle() {
  sigHeartbeat();

  sRecognitionData_t r = ai10.getRecognitionResult(RECOG_TIMEOUT_S);

  if (r.result == eSuccess && r.type == ePalm) {
    uint16_t uid = r.userData.UID;

    // Self-heal the name map from what the module reports.
    if (strlen(r.userData.userName) > 0 && nameOf(uid) != r.userData.userName) {
      setNameOf(uid, r.userData.userName);
      refreshUsers();
    }
    if (r.userData.admin == eAdmin && !isAdminUid(uid)) {
      setAdminUid(uid, true);
      refreshUsers();
    }

    if (isBlocked(uid)) {
      setMsg("DENIED - UID %u is blocked", uid);
      oledBanner("DENIED", "access blocked");
      sigDenied();
      oledIdle();
      return;
    }

    String n = nameOf(uid);
    unlockDoor(n.length() ? n.c_str() : "palm recognised");

  } else if (r.result == eSuccess) {
    // Face or QR matched. This lock authorises on palm only.
    setMsg("ignored a non-palm match (type %u)", r.type);

  } else if (r.result == eFailedTimeout) {
    // Idle. Stay quiet.

  } else if (r.result == eFailedUnknowUser) {
    setMsg("DENIED - palm not enrolled");
    oledBanner("DENIED", "not enrolled");
    sigDenied();
    oledIdle();

  } else if (r.result == eFailedLiveCheck) {
    setMsg("DENIED - liveness check failed");
    oledBanner("DENIED", "liveness check");
    sigDenied();
    oledIdle();

  } else {
    setMsg("recognition error, code %u", r.result);
  }
}

// ---------------------------------------------------------------------
// Button - short press unlocks, long press starts enrollment
// ---------------------------------------------------------------------

// Waits for the button to be released, signalling once the hold
// threshold is crossed so the user knows to let go. Returns how long it
// was held. Blocking is fine here - somebody has their finger on it.
uint32_t waitRelease(uint8_t pin, uint32_t thresholdMs) {
  uint32_t t0 = millis();
  bool signalled = false;

  while (digitalRead(pin) == LOW) {
    uint32_t held = millis() - t0;
    if (thresholdMs && held >= thresholdMs && !signalled) {
      signalled = true;
      beep(60, 2);
      leds(true, true);
    }
    if (held > 10000) break; // stuck button - give up
    delay(10);
  }
  leds(false, false);
  return millis() - t0;
}

void startButtonEnroll() {
  char autoName[32];
  snprintf(autoName, sizeof(autoName), "User%lu", (unsigned long)(millis() % 10000));
  doEnroll(autoName, REQUIRE_ADMIN_FOR_BUTTON_ENROLL);
}

// Exit button. Unlock-only when a dedicated enroll button exists;
// otherwise a long press still starts enrollment.
void handleButton() {
  uint32_t threshold = USE_ENROLL_BUTTON ? 0 : LONG_PRESS_MS;
  uint32_t held = waitRelease(BTN_REX, threshold);

  if (!USE_ENROLL_BUTTON && held >= LONG_PRESS_MS) startButtonEnroll();
  else if (held > 40)                              unlockDoor("exit button");
}

// Dedicated registration button. Requires a deliberate hold so a brush
// against it cannot open an enrollment window.
void handleEnrollButton() {
  uint32_t held = waitRelease(BTN_ENROLL, ENROLL_PRESS_MS);
  if (held >= ENROLL_PRESS_MS) startButtonEnroll();
  else setMsg("registration button: hold it for 1.5s");
}

// ---------------------------------------------------------------------
// Web UI
// ---------------------------------------------------------------------

static const char PAGE_HTML[] PROGMEM = R"HTML(
<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>Palm Lock</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;padding:16px;background:#111;color:#eee}
h1{font-size:20px;margin:0 0 4px}
.s{background:#1c1c1c;border:1px solid #333;border-radius:8px;padding:12px;margin:12px 0}
.msg{color:#9c9;font-size:14px;min-height:18px}
.st{font-size:13px;color:#888}
table{width:100%;border-collapse:collapse;font-size:14px}
td,th{text-align:left;padding:6px 4px;border-bottom:1px solid #2a2a2a}
button{background:#2a2a2a;color:#eee;border:1px solid #444;border-radius:6px;padding:6px 10px;cursor:pointer;font-size:13px}
button:hover{background:#333}
button.p{background:#2d5;color:#000;border-color:#2d5}
input{background:#000;color:#eee;border:1px solid #444;border-radius:6px;padding:7px;font-size:14px}
.b{color:#e66}.a{color:#fc6}
</style>
<h1>Palm lock</h1>
<div class=st id=st>connecting...</div>
<div class=s><div class=msg id=msg></div></div>
<div class=s>
  <input id=nm placeholder="Name for the new user" maxlength=31>
  <button class=p onclick=enroll()>Enroll</button>
  <div class=st style=margin-top:8px>An admin palm must confirm at the door.</div>
</div>
<div class=s><table id=tb></table></div>
<div class=s><button onclick=post('/api/unlock')>Unlock now</button></div>
<script>
function post(u){return fetch(u,{method:'POST'}).then(r=>r.text()).then(()=>tick())}
function enroll(){let n=document.getElementById('nm').value.trim();if(!n)return alert('Enter a name');
 post('/api/enroll?name='+encodeURIComponent(n));document.getElementById('nm').value=''}
function ren(u,c){let n=prompt('New name',c);if(n)post('/api/rename?uid='+u+'&name='+encodeURIComponent(n))}
function del(u,c){if(confirm('Delete '+c+'? The palm is removed from the sensor.'))post('/api/delete?uid='+u)}
function tick(){fetch('/api/status').then(r=>r.json()).then(d=>{
 document.getElementById('st').textContent='state: '+d.state+'  -  '+d.users.length+' user(s)'+(d.sensor?'':'  -  SENSOR FAULT');
 document.getElementById('msg').textContent=d.msg;
 let h='<tr><th>UID<th>Name<th></tr>';
 for(const u of d.users){h+='<tr><td>'+u.uid+'<td>'+u.name+
  (u.admin?' <span class=a>[admin]</span>':'')+(u.blocked?' <span class=b>[blocked]</span>':'')+
  '<td style=text-align:right><button onclick="ren('+u.uid+',\''+u.name+'\')">Rename</button> '+
  '<button onclick="post(\'/api/'+(u.blocked?'permit':'block')+'?uid='+u.uid+'\')">'+(u.blocked?'Permit':'Block')+'</button> '+
  '<button onclick="del('+u.uid+',\''+u.name+'\')">Delete</button></tr>'}
 document.getElementById('tb').innerHTML=h})}
setInterval(tick,1500);tick();
</script>
)HTML";

String jsonEscape(const char *s) {
  String o;
  for (const char *p = s; *p; p++) {
    if (*p == '"' || *p == '\\') { o += '\\'; o += *p; }
    else if (*p >= 32)           { o += *p; }
  }
  return o;
}

String buildStatusJson() {
  String j = "{";
  ST_LOCK();
  j += "\"state\":\"" + jsonEscape(gState) + "\",";
  j += "\"msg\":\"" + jsonEscape(gLastMsg) + "\",";
  j += "\"sensor\":" + String(gSensorOk ? "true" : "false") + ",";
  j += "\"users\":[";
  for (uint8_t i = 0; i < gUserN; i++) {
    if (i) j += ",";
    j += "{\"uid\":" + String(gUsers[i].uid) +
         ",\"name\":\"" + jsonEscape(gUsers[i].name) + "\"" +
         ",\"blocked\":" + (gUsers[i].blocked ? "true" : "false") +
         ",\"admin\":"   + (gUsers[i].admin   ? "true" : "false") + "}";
  }
  ST_UNLOCK();
  j += "]}";
  return j;
}

// Every handler below only touches the command queue and the mutexed
// status. None of them call ai10.* - that is the rule that keeps the
// sensor's UART owned by exactly one task.

// Every endpoint goes through this first. Returns false and sends the
// challenge if the caller has not logged in, so handlers can simply do
//   if (!authed()) return;
bool authed() {
  if (server.authenticate(ADMIN_USER, ADMIN_PASS)) return true;
  server.requestAuthentication(DIGEST_AUTH, "Palm Lock",
                               "Login required");
  return false;
}

void uidCmd(uint8_t type) {
  if (!authed()) return;
  if (!server.hasArg("uid")) { server.send(400, "text/plain", "uid required"); return; }
  uint16_t uid = server.arg("uid").toInt();
  String n;
  const char *nm = nullptr;
  if (server.hasArg("name")) {
    n = server.arg("name");
    n.trim();
    nm = n.c_str();
  }
  server.send(pushCmd(type, uid, nm) ? 202 : 409, "text/plain", "queued");
}

void setupWeb() {
  server.on("/", HTTP_GET, []() {
    if (!authed()) return;
    server.send_P(200, "text/html", PAGE_HTML);
  });

  server.on("/api/status", HTTP_GET, []() {
    if (!authed()) return;
    server.send(200, "application/json", buildStatusJson());
  });

  server.on("/api/enroll", HTTP_POST, []() {
    if (!authed()) return;
    if (!server.hasArg("name")) { server.send(400, "text/plain", "name required"); return; }
    String n = server.arg("name");
    n.trim();
    if (n.length() == 0 || n.length() > 31) { server.send(400, "text/plain", "bad name"); return; }
    server.send(pushCmd(CMD_ENROLL_WEB, 0, n.c_str()) ? 202 : 409, "text/plain", "queued");
  });

  server.on("/api/delete", HTTP_POST, []() { uidCmd(CMD_DELETE); });
  server.on("/api/rename", HTTP_POST, []() { uidCmd(CMD_RENAME); });
  server.on("/api/block",  HTTP_POST, []() { uidCmd(CMD_BLOCK);  });
  server.on("/api/permit", HTTP_POST, []() { uidCmd(CMD_PERMIT); });

  server.on("/api/unlock", HTTP_POST, []() {
    if (!authed()) return;
    server.send(pushCmd(CMD_UNLOCK, 0, nullptr) ? 202 : 409, "text/plain", "queued");
  });

  server.begin();
// Web handling runs in its own task so requests can be serviced while the loop waits on the sensor.
  xTaskCreatePinnedToCore(
    [](void *) {
      for (;;) {
        server.handleClient();
        delay(2);
      }
    },
    "web", 8192, nullptr, 1, &webTaskHandle, 0);
}

// ---------------------------------------------------------------------
// WiFi - never allowed to block the door
// ---------------------------------------------------------------------

void startWiFi() {
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");

  if (ssid.length()) {
    Serial.print(F("WiFi: connecting to "));
    Serial.println(ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(200);
    if (WiFi.status() == WL_CONNECTED) {
      WiFi.setAutoReconnect(true);
      snprintf(gIp, sizeof(gIp), "%s", WiFi.localIP().toString().c_str());
      Serial.print(F("WiFi: connected, http://"));
      Serial.println(WiFi.localIP());
      if (MDNS.begin("palmlock")) Serial.println(F("WiFi: also http://palmlock.local"));
      return;
    }
    Serial.println(F("WiFi: connect failed"));
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  snprintf(gIp, sizeof(gIp), "%s", WiFi.softAPIP().toString().c_str());
  Serial.print(F("WiFi: access point '"));
  Serial.print(AP_SSID);
  Serial.print(F("' -> http://"));
  Serial.println(WiFi.softAPIP());
}

// ---------------------------------------------------------------------
// Serial admin
// ---------------------------------------------------------------------

void printHelp() {
  Serial.println(F("l | e <name> | b <name> | d <uid> | r <uid> <name>"));
  Serial.println(F("x <uid> | p <uid> | u | w <ssid> <pass> | i | ?"));
}

void listUsers() {
  ST_LOCK();
  Serial.print(F("Users: "));
  Serial.println(gUserN);
  for (uint8_t i = 0; i < gUserN; i++) {
    Serial.print(F("  UID "));
    Serial.print(gUsers[i].uid);
    Serial.print(F("  "));
    Serial.print(gUsers[i].name);
    if (gUsers[i].admin)   Serial.print(F("  [admin]"));
    if (gUsers[i].blocked) Serial.print(F("  [BLOCKED]"));
    Serial.println();
  }
  ST_UNLOCK();
}

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (!line.length()) return;

  char cmd = line.charAt(0);
  String arg = line.substring(1);
  arg.trim();

  switch (cmd) {
    case 'l': listUsers(); break;
    case 'e': pushCmd(CMD_ENROLL_WEB, 0, arg.length() ? arg.c_str() : "User"); break;
    case 'b': pushCmd(CMD_ENROLL_BTN, 0, arg.length() ? arg.c_str() : "User"); break;
    case 'd': pushCmd(CMD_DELETE, arg.toInt(), nullptr); break;
    case 'x': pushCmd(CMD_BLOCK,  arg.toInt(), nullptr); break;
    case 'p': pushCmd(CMD_PERMIT, arg.toInt(), nullptr); break;
    case 'u': pushCmd(CMD_UNLOCK, 0, nullptr); break;
    case 'r': {
      int sp = arg.indexOf(' ');
      if (sp < 1) { Serial.println(F("usage: r <uid> <name>")); break; }
      pushCmd(CMD_RENAME, arg.substring(0, sp).toInt(), arg.substring(sp + 1).c_str());
      break;
    }
    case 'w': {
      int sp = arg.indexOf(' ');
      String ss = (sp < 0) ? arg : arg.substring(0, sp);
      String pw = (sp < 0) ? ""  : arg.substring(sp + 1);
      prefs.putString("ssid", ss);
      prefs.putString("pass", pw);
      Serial.println(F("saved - reconnecting"));
      startWiFi();
      break;
    }
    case 'i':
      Serial.print(F("mode "));
      Serial.print(WiFi.getMode() == WIFI_AP ? F("AP") : F("STA"));
      Serial.print(F("  ip "));
      Serial.println(WiFi.getMode() == WIFI_AP ? WiFi.softAPIP() : WiFi.localIP());
      break;
    default: printHelp(); break;
  }
}

// ---------------------------------------------------------------------

void setup() {
  // Secure the lock before anything else can glitch the pin.
  pinMode(RELAY_PIN, OUTPUT);
  writeLock(false);

  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);
  pinMode(LED_RED, OUTPUT);    digitalWrite(LED_RED, LOW);
  pinMode(LED_GREEN, OUTPUT);  digitalWrite(LED_GREEN, LOW);
  pinMode(BTN_REX, INPUT_PULLUP);
  if (USE_ENROLL_BUTTON) pinMode(BTN_ENROLL, INPUT_PULLUP);
  if (USE_TAMPER) pinMode(TAMPER_PIN, INPUT_PULLUP);
  if (USE_REED)   pinMode(REED_PIN, INPUT_PULLUP);

  Serial.begin(115200);
  delay(300);

  stMux    = xSemaphoreCreateMutex();
  cmdQueue = xQueueCreate(8, sizeof(Cmd));

  prefs.begin("palmlock", false);

  Serial.println();
  Serial.println(F("=== PALM RECOGNITION DOOR LOCK ==="));

  oledInit();
  oledLines("PALM LOCK", "starting...", "");

  setState("starting");
  if (ai10.begin()) {
    gSensorOk = true;
    Serial.println(F("Sensor: connected"));
    refreshUsers();
    listUsers();
    if (userCount() == 0) {
      Serial.println(F("Sensor is EMPTY. The first enrollment needs no admin"));
      Serial.println(F("and becomes the admin account. Hold the registration button 1.5s, or"));
      Serial.println(F("use the web page."));
    }
  } else {
    // Fail secure, but keep running: the button and web UI still work, so
    // you can diagnose without a laptop, and the door stays locked.
    gSensorOk = false;
    Serial.println(F("Sensor: NO RESPONSE."));
    Serial.println(F("  Check power first - recognition needs 5-12V @ ~330mA."));
    Serial.println(F("  The ESP32's 3V3 pin cannot supply this."));
    setMsg("SENSOR FAULT - door stays locked");
    oledLines("SENSOR FAULT", "Check power:", "5-12V @ 330mA");
  }

  startWiFi();
  setupWeb();
  printHelp();
  setState("idle");
  beep(100);
  if (gSensorOk) oledIdle();
  Serial.println(F("Ready."));
}

void loop() {
  handleSerial();

  if (doorUnlocked) {
    bool doorOpened = USE_REED && digitalRead(REED_PIN) == HIGH;
    if (doorOpened || millis() - unlockStartMs >= UNLOCK_MS) lockDoor();
    delay(10);
    return;
  }

  // Tamper is a NORMALLY-CLOSED loop to GND, so healthy reads LOW. An open
  // circuit (HIGH, via the internal pull-up) means the enclosure was opened
  // OR the cable was cut - both must alarm. That is the whole reason tamper
  // contacts are NC: a switch that only trips on "opened" can be defeated by
  // snipping one wire.
  if (USE_TAMPER && digitalRead(TAMPER_PIN) == HIGH) {
    setState("tamper");
    setMsg("TAMPER - reader unit opened. Entry refused.");
    oledBanner("TAMPER", "entry refused");
    blinkWhile(2000, 200, true, false);
    beep(400);
    return;
  }

  if (digitalRead(BTN_REX) == LOW) {
    delay(30); // debounce
    if (digitalRead(BTN_REX) == LOW) { handleButton(); return; }
  }

  if (USE_ENROLL_BUTTON && digitalRead(BTN_ENROLL) == LOW) {
    delay(30);
    if (digitalRead(BTN_ENROLL) == LOW) { handleEnrollButton(); return; }
  }

  // Queued work from the web/serial takes priority over scanning.
  Cmd c;
  if (xQueueReceive(cmdQueue, &c, 0) == pdTRUE) {
    executeCmd(c.type, c.uid, c.name);
    return;
  }

  if (gSensorOk) recognitionCycle();
  else           delay(200);
}
