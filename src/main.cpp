#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <WiFi.h>

#include "DFRobot_AI10.h"
#include "guardian_x/device_core.h"
#include "guardian_x/littlefs_storage.h"
#include "guardian_x/provisioning.h"

namespace {

// SEN0677 UART: ESP32-S3 GPIO17 (TX) -> SEN0677 RX,
//               ESP32-S3 GPIO16 (RX) <- SEN0677 TX.
constexpr uint8_t kSensorUartTxPin = 17;
constexpr uint8_t kSensorUartRxPin = 16;
constexpr uint32_t kSensorUartBaud = 115200;

// SSD1309 SPI wiring.
constexpr uint8_t kOledSckPin = 12;
constexpr uint8_t kOledMosiPin = 11;
constexpr uint8_t kOledCsPin = 10;
constexpr uint8_t kOledDcPin = 9;
constexpr uint8_t kOledResetPin = 8;
constexpr uint8_t kOledWidth = 128;

constexpr uint8_t kRecognitionTimeoutSeconds = 5;
constexpr uint8_t kEnrollmentTimeoutSeconds = 15;
constexpr unsigned long kResultScreenDurationMs = 1800;
constexpr size_t kMaximumUserNameLength = 31;
constexpr unsigned long kWifiReconnectDelayMs = 5000;
constexpr unsigned long kWifiConnectTimeoutMs = 30000;

DFRobot_AI10_UART ai10(
    &Serial1, kSensorUartBaud, kSensorUartTxPin, kSensorUartRxPin);
U8G2_SSD1309_128X64_NONAME0_F_4W_HW_SPI oled(
    U8G2_R0, kOledCsPin, kOledDcPin, kOledResetPin);

guardian_x::LittleFsStorage deviceStorage;
guardian_x::DeviceCore deviceCore(deviceStorage);
guardian_x::ProvisioningService provisioningService(deviceCore);

enum class WifiConnectionPhase : uint8_t {
  kIdle,
  kConnecting,
};

bool oledReady = false;
bool sensorReady = false;
WifiConnectionPhase wifiConnectionPhase = WifiConnectionPhase::kIdle;
unsigned long wifiAttemptStartMs = 0;
unsigned long lastWifiReconnectAttempt = 0;

void drawStatus(const char *title, const char *line2 = "", const char *line3 = "") {
  if (!oledReady) {
    return;
  }

  oled.clearBuffer();
  oled.setDrawColor(1);
  oled.setFont(u8g2_font_helvB12_tf);
  oled.setCursor(0, 15);
  oled.print(title);
  oled.setFont(u8g2_font_6x10_tf);
  oled.setCursor(0, 37);
  oled.print(line2);
  oled.setCursor(0, 55);
  oled.print(line3);
  oled.sendBuffer();
}

void showReadyScreen() {
  drawStatus("SENSOR READY", "PRESENT PALM", "Serial: r to scan");
}

void showSensorError(const char *detail) {
  Serial.print(F("Sensor error: "));
  Serial.println(detail);
  drawStatus("SENSOR ERROR", detail, "See Serial Monitor");
}

void initializeOled() {
  SPI.begin(kOledSckPin, -1, kOledMosiPin, kOledCsPin);
  oledReady = oled.begin();

  if (!oledReady) {
    Serial.println(F("OLED initialization failed; continuing with serial diagnostics."));
    return;
  }

  oled.setContrast(255);
  drawStatus("BOOTING", "GUARDIAN X", "PHASE 3");
  Serial.println(F("OLED: ready"));
}

void initializeSensor() {
  Serial.println(F("Initializing SEN0677 on Serial1 at 115200 baud..."));
  if (ai10.begin()) {
    sensorReady = true;
    Serial.println(F("Sensor: ready"));
    showReadyScreen();
  } else {
    showSensorError("Initialization failed");
  }
}

void printHelp() {
  Serial.println();
  Serial.println(F("GUARDIAN X Phase 3 serial commands:"));
  Serial.println(F("  l              List registered SEN0677 user IDs"));
  Serial.println(F("  e <name>       Enroll a palm with the given test name"));
  Serial.println(F("  r              Recognize one presented palm"));
  Serial.println(F("  d <uid>        Delete a registered SEN0677 user ID"));
  Serial.println(F("  f              Factory reset (clear network configuration)"));
  Serial.println(F("  h              Show this help"));
}

bool requireSensorReady() {
  if (sensorReady) {
    return true;
  }

  showSensorError("Not initialized");
  return false;
}

void listUsers() {
  if (!requireSensorReady()) {
    return;
  }

  Serial.println(F("Reading registered SEN0677 user IDs..."));
  const sAllUserID_t users = ai10.getAllUserIDs();
  if (users.result != eSuccess) {
    char detail[32];
    snprintf(detail, sizeof(detail), "List failed: %u", users.result);
    showSensorError(detail);
    return;
  }

  Serial.print(F("Registered users: "));
  Serial.println(users.userNum);
  if (users.userNum == 0) {
    Serial.println(F("  (none)"));
    return;
  }

  for (uint16_t index = 0; index < users.userNum; ++index) {
    Serial.print(F("  UID "));
    Serial.println(users.UIDS[index]);
  }
}

void enrollUser(const String &name) {
  if (!requireSensorReady()) {
    return;
  }

  if (name.length() == 0 || name.length() > kMaximumUserNameLength) {
    Serial.println(F("Enrollment failed: name must be 1-31 characters."));
    drawStatus("ENROLLMENT", "INVALID NAME", "Use 1-31 chars");
    return;
  }

  Serial.print(F("Enrollment started for '"));
  Serial.print(name);
  Serial.println(F("'. Present the palm to the SEN0677."));
  drawStatus("ENROLLMENT", "PRESENT PALM", name.c_str());

  const sUserData_t user = ai10.enrollUser(eNormal, name.c_str(), kEnrollmentTimeoutSeconds);
  if (user.result == eSuccess) {
    char uidText[24];
    snprintf(uidText, sizeof(uidText), "UID: %u", user.UID);
    Serial.print(F("Enrollment succeeded. "));
    Serial.println(uidText);
    if (user.type != ePalm) {
      Serial.print(F("Warning: sensor reported result type "));
      Serial.println(user.type);
    }
    drawStatus("ENROLLMENT", "SUCCESS", uidText);
    delay(kResultScreenDurationMs);
    showReadyScreen();
    return;
  }

  char detail[32];
  if (user.result == eFailedTimeout) {
    snprintf(detail, sizeof(detail), "Timed out");
  } else if (user.result == eFailedMaxUser) {
    snprintf(detail, sizeof(detail), "Sensor full");
  } else if (user.result == eFailedCamera) {
    snprintf(detail, sizeof(detail), "Camera error");
  } else {
    snprintf(detail, sizeof(detail), "Failed: %u", user.result);
  }
  Serial.print(F("Enrollment failed: "));
  Serial.println(detail);
  drawStatus("ENROLLMENT", "FAILED", detail);
  delay(kResultScreenDurationMs);
  showReadyScreen();
}

void recognizePalm() {
  if (!requireSensorReady()) {
    return;
  }

  Serial.println(F("Recognition started. Present a registered palm."));
  drawStatus("RECOGNIZING", "PRESENT PALM", "Waiting for sensor");

  const sRecognitionData_t recognition = ai10.getRecognitionResult(kRecognitionTimeoutSeconds);
  if (recognition.result == eSuccess && recognition.type == ePalm) {
    char uidText[24];
    snprintf(uidText, sizeof(uidText), "UID: %u", recognition.userData.UID);
    Serial.print(F("Recognition success. "));
    Serial.println(uidText);
    drawStatus("RECOGNITION", "SUCCESS", uidText);
  } else {
    char detail[32];
    if (recognition.result == eFailedTimeout) {
      snprintf(detail, sizeof(detail), "Timed out");
    } else if (recognition.result == eFailedUnknowUser) {
      snprintf(detail, sizeof(detail), "Palm not enrolled");
    } else if (recognition.result == eSuccess) {
      snprintf(detail, sizeof(detail), "Not a palm: %u", recognition.type);
    } else {
      snprintf(detail, sizeof(detail), "Failed: %u", recognition.result);
    }
    Serial.print(F("Recognition failed: "));
    Serial.println(detail);
    drawStatus("RECOGNITION", "FAILED", detail);
  }

  delay(kResultScreenDurationMs);
  showReadyScreen();
}

bool parseUserId(const String &text, uint16_t *userId) {
  if (text.length() == 0) {
    return false;
  }

  for (size_t index = 0; index < text.length(); ++index) {
    if (!isDigit(text.charAt(index))) {
      return false;
    }
  }

  const unsigned long parsed = strtoul(text.c_str(), nullptr, 10);
  if (parsed == 0 || parsed > UINT16_MAX) {
    return false;
  }

  *userId = static_cast<uint16_t>(parsed);
  return true;
}

void deleteUser(const String &argument) {
  if (!requireSensorReady()) {
    return;
  }

  uint16_t userId = 0;
  if (!parseUserId(argument, &userId)) {
    Serial.println(F("Delete failed: use a valid numeric UID from 1 to 65535."));
    drawStatus("DELETE", "INVALID USER ID", "Use: d <uid>");
    return;
  }

  Serial.print(F("Deleting UID "));
  Serial.println(userId);
  if (ai10.deleteUser(userId)) {
    char uidText[24];
    snprintf(uidText, sizeof(uidText), "Deleted UID: %u", userId);
    Serial.println(F("Delete succeeded."));
    drawStatus("DELETE", "SUCCESS", uidText);
  } else {
    char detail[32];
    snprintf(detail, sizeof(detail), "UID %u failed", userId);
    Serial.println(F("Delete failed; check the UID and sensor connection."));
    drawStatus("DELETE", "FAILED", detail);
  }

  delay(kResultScreenDurationMs);
  showReadyScreen();
}

void factoryReset() {
  Serial.println(F("Factory reset: clearing network configuration…"));
  drawStatus("FACTORY RESET", "CLEARING CONFIG", "Please wait…");

  const guardian_x::CoreResult result = deviceCore.clearNetworkConfiguration();

  if (result == guardian_x::CoreResult::kNotReady ||
      result == guardian_x::CoreResult::kStorageUnavailable) {
    // The core cannot read or write storage, so no readable application data
    // exists to preserve.  Erase the slots (and reformat the filesystem if it
    // cannot be mounted); the next boot recreates a fresh unprovisioned
    // snapshot and enters setup mode.
    deviceStorage.eraseAllSlots();
    deviceStorage.recoverWithFormat();
    Serial.println(F("Factory reset: storage reinitialized. Restarting…"));
    drawStatus("FACTORY RESET", "COMPLETE", "Restarting…");
    delay(1000);
    ESP.restart();
    return;
  }

  if (result != guardian_x::CoreResult::kOk) {
    Serial.print(F("Factory reset failed: "));
    Serial.println(guardian_x::coreResultText(result));
    drawStatus("FACTORY RESET", "FAILED", guardian_x::coreResultText(result));
    delay(2000);
    return;
  }

  Serial.println(F("Factory reset complete. Restarting…"));
  drawStatus("FACTORY RESET", "COMPLETE", "Restarting…");
  delay(1000);
  ESP.restart();
}

void handleSerialCommand() {
  if (!Serial.available()) {
    return;
  }

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) {
    return;
  }

  const char command = tolower(line.charAt(0));
  String argument = line.substring(1);
  argument.trim();

  switch (command) {
    case 'l':
      listUsers();
      break;
    case 'e':
      enrollUser(argument);
      break;
    case 'r':
      recognizePalm();
      break;
    case 'd':
      deleteUser(argument);
      break;
    case 'f':
      factoryReset();
      break;
    case 'h':
    case '?':
      printHelp();
      break;
    default:
      Serial.println(F("Unknown command."));
      printHelp();
      break;
  }
}

// Wi-Fi reconnection is non-blocking so serial diagnostics (including the
// factory-reset command) and sensor operations stay responsive while a
// provisioned device waits out a router outage.
void startWifiConnectionAttempt() {
  const guardian_x::Snapshot &snapshot = deviceCore.snapshot();
  const char *ssid = snapshot.configuration.wifiSsid;
  const char *passphrase = snapshot.configuration.wifiPassphrase;

  if (ssid[0] == '\0') {
    return;
  }

  Serial.print(F("Connecting to Wi-Fi: "));
  Serial.println(ssid);

  WiFi.disconnect();
  WiFi.begin(ssid, passphrase);

  wifiAttemptStartMs = millis();
  wifiConnectionPhase = WifiConnectionPhase::kConnecting;

  if (!oledReady) {
    // Fall back to serial-only output when the OLED is unavailable.
    Serial.println(F("Attempting Wi-Fi connection…"));
  } else {
    drawStatus("CONNECTING", ssid, "Please wait…");
  }
}

void pollWifiConnection() {
  if (wifiConnectionPhase != WifiConnectionPhase::kConnecting) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnectionPhase = WifiConnectionPhase::kIdle;
    Serial.print(F("Wi-Fi connected. IP: "));
    Serial.println(WiFi.localIP());
    if (oledReady) {
      char ipText[24];
      snprintf(ipText, sizeof(ipText), "%s", WiFi.localIP().toString().c_str());
      drawStatus("CONNECTED", deviceCore.snapshot().configuration.wifiSsid,
                 ipText);
    }
    return;
  }

  if (millis() - wifiAttemptStartMs >= kWifiConnectTimeoutMs) {
    wifiConnectionPhase = WifiConnectionPhase::kIdle;
    Serial.println(F("Wi-Fi connection failed. Will retry…"));
    WiFi.disconnect();
    if (oledReady) {
      drawStatus("Wi-Fi FAILED", "Will retry", "Check credentials");
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("========================================");
  Serial.println("GUARDIAN X");
  Serial.println("PHASE 3 - DEVICE PROVISIONING");
  Serial.println("========================================");
  Serial.println(F("ESP32-S3 firmware booted."));

  // Keep the user's network credentials out of the Wi-Fi library's NVS
  // cache: the DeviceCore snapshot is the single source of truth for
  // provisioning state.
  WiFi.persistent(false);

  initializeOled();
  initializeSensor();

  Serial.println(F("Initializing device core…"));
  guardian_x::CoreResult coreResult = deviceCore.begin();

  if (coreResult == guardian_x::CoreResult::kStorageUnavailable) {
    // A blank factory-fresh filesystem (or one too damaged to mount) holds no
    // readable data, so reformatting is safe and is the only path back to a
    // working, provisionable device.
    if (deviceStorage.recoverWithFormat()) {
      Serial.println(F("Storage recovery: filesystem was blank or unreadable; reformatted."));
      coreResult = deviceCore.begin();
    }
  }

  if (coreResult == guardian_x::CoreResult::kRecoveryRequired) {
    // No slot passed CRC/schema validation.  Erase the invalid slots so the
    // core recreates a fresh unprovisioned snapshot; nothing readable is
    // lost, and the device re-enters setup mode instead of being stranded.
    deviceStorage.eraseAllSlots();
    Serial.println(F("Storage recovery: no valid snapshot; resetting to factory-unprovisioned state."));
    coreResult = deviceCore.begin();
  }

  Serial.print(F("Device core: "));
  Serial.println(guardian_x::coreResultText(coreResult));

  if (coreResult != guardian_x::CoreResult::kOk) {
    Serial.println(F("Local data changes are disabled; hardware diagnostics remain available."));
    if (oledReady) {
      drawStatus("STORAGE ERROR", guardian_x::coreResultText(coreResult), "See Serial Monitor");
    }
    printHelp();
    return;
  }

  if (!deviceCore.isProvisioned()) {
    Serial.println(F("Device is unprovisioned. Entering setup mode…"));
    if (oledReady) {
      drawStatus("SETUP MODE", "Connect to AP", "See Serial Monitor");
    }

    const bool provisioningStarted = provisioningService.begin();
    if (!provisioningStarted) {
      Serial.println(F("Failed to start provisioning mode."));
      if (oledReady) {
        drawStatus("SETUP FAILED", "AP could not start", "See Serial Monitor");
      }
    }
  } else {
    Serial.println(F("Device is provisioned. Attempting Wi-Fi connection…"));
    startWifiConnectionAttempt();
  }

  printHelp();
}

void loop() {
  handleSerialCommand();

  if (provisioningService.isActive()) {
    provisioningService.update();
    return;
  }

  pollWifiConnection();

  // In normal mode, retry Wi-Fi connection periodically if disconnected.
  // A temporary router outage never re-enters setup mode or clears
  // provisioning: the persisted configuration stays intact and is retried.
  if (deviceCore.isProvisioned() &&
      wifiConnectionPhase == WifiConnectionPhase::kIdle &&
      WiFi.status() != WL_CONNECTED) {
    const unsigned long now = millis();
    if (now - lastWifiReconnectAttempt >= kWifiReconnectDelayMs) {
      lastWifiReconnectAttempt = now;
      startWifiConnectionAttempt();
    }
  }
}
