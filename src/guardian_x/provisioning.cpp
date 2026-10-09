#include "guardian_x/provisioning.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

#include "guardian_x/device_core.h"

namespace guardian_x {

namespace {

// AP SSID prefix.  The device MAC suffix is appended for uniqueness.
constexpr char kApSsidPrefix[] = "GUARDIAN-X-";

// Minimal responsive setup page.  Embedded to avoid data/ file dependencies.
const char *kSetupPageHtml = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>GUARDIAN X Setup</title>
  <style>
    :root {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      background: #f5f5f5;
      color: #222;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      padding: 16px;
      display: flex;
      justify-content: center;
      align-items: center;
      min-height: 100vh;
      background: #f5f5f5;
    }
    .card {
      background: #fff;
      border-radius: 8px;
      box-shadow: 0 2px 8px rgba(0,0,0,0.08);
      padding: 24px;
      width: 100%;
      max-width: 400px;
    }
    h1 {
      margin: 0 0 4px 0;
      font-size: 20px;
      text-align: center;
    }
    .subtitle {
      text-align: center;
      color: #666;
      font-size: 13px;
      margin-bottom: 20px;
    }
    label {
      display: block;
      font-size: 13px;
      font-weight: 600;
      margin: 12px 0 6px;
    }
    input[type="text"], input[type="password"] {
      width: 100%;
      padding: 10px;
      border: 1px solid #ccc;
      border-radius: 6px;
      font-size: 15px;
    }
    button {
      width: 100%;
      margin-top: 18px;
      padding: 12px;
      background: #0066cc;
      color: #fff;
      border: none;
      border-radius: 6px;
      font-size: 16px;
      cursor: pointer;
    }
    button:disabled {
      background: #999;
      cursor: not-allowed;
    }
    .status {
      margin-top: 14px;
      padding: 10px;
      border-radius: 6px;
      font-size: 13px;
      display: none;
      text-align: center;
    }
    .status.error { background: #fde8e8; color: #c00; display: block; }
    .status.success { background: #e6f4ea; color: #1a7f37; display: block; }
    .status.info { background: #e8f0fe; color: #0066cc; display: block; }
    .spinner {
      display: inline-block;
      width: 14px;
      height: 14px;
      border: 2px solid #0066cc;
      border-top-color: transparent;
      border-radius: 50%;
      animation: spin 1s linear infinite;
      vertical-align: middle;
      margin-right: 6px;
    }
    @keyframes spin { to { transform: rotate(360deg); } }
  </style>
</head>
<body>
  <div class="card">
    <h1>GUARDIAN X</h1>
    <div class="subtitle">Device Setup</div>

    <form id="setupForm">
      <label for="ssid">Wi-Fi Network (SSID)</label>
      <input type="text" id="ssid" name="ssid" autocomplete="off" required>

      <label for="passphrase">Password</label>
      <input type="password" id="passphrase" name="passphrase" autocomplete="off">

      <button type="submit" id="submitBtn">Connect & Save</button>
    </form>

    <div id="status" class="status"></div>
  </div>

  <script>
    const form = document.getElementById('setupForm');
    const status = document.getElementById('status');
    const submitBtn = document.getElementById('submitBtn');

    function showStatus(message, type) {
      status.className = 'status ' + type;
      status.innerHTML = type === 'info' ? '<span class="spinner"></span>' + message : message;
    }

    form.addEventListener('submit', function(e) {
      e.preventDefault();
      const ssid = document.getElementById('ssid').value.trim();
      const passphrase = document.getElementById('passphrase').value;

      if (!ssid) {
        showStatus('Please enter a network name (SSID).', 'error');
        return;
      }

      submitBtn.disabled = true;
      showStatus('Connecting to Wi-Fi…', 'info');

      fetch('/configure', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'ssid=' + encodeURIComponent(ssid) + '&passphrase=' + encodeURIComponent(passphrase)
      })
      .then(function(res) { return res.json(); })
      .then(function(data) {
        if (data.success) {
          showStatus('Wi-Fi configured. Device will restart and connect.', 'success');
          submitBtn.textContent = 'Saved';
        } else {
          showStatus(data.error || 'Configuration failed. Please try again.', 'error');
          submitBtn.disabled = false;
        }
      })
      .catch(function() {
        showStatus('Network error. Please try again.', 'error');
        submitBtn.disabled = false;
      });
    });
  </script>
</body>
</html>
)rawliteral";

const char *wifiFailureDetail(wl_status_t status) {
  switch (status) {
    case WL_NO_SSID_AVAIL:
      return "network not found";
    case WL_CONNECT_FAILED:
      return "connection failed (wrong password or network problem)";
    case WL_CONNECTION_LOST:
      return "connection lost";
    default:
      return "could not reach the network";
  }
}

bool buildSsid(char *buffer, size_t capacity) {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  // Use last 4 hex chars of MAC for deterministic per-device uniqueness.
  const size_t prefixLength = strlen(kApSsidPrefix);
  if (prefixLength + 5 >= capacity) {  // 4 hex + null
    return false;
  }
  memcpy(buffer, kApSsidPrefix, prefixLength);
  snprintf(buffer + prefixLength, capacity - prefixLength, "%02X%02X", mac[4], mac[5]);
  return true;
}

}  // namespace

ProvisioningService::ProvisioningService(DeviceCore &deviceCore)
    : deviceCore_(deviceCore), active_(false), apStarted_(false), webServer_(nullptr) {}

bool ProvisioningService::begin() {
  stop();
  if (!deviceCore_.ready() || deviceCore_.isProvisioned()) {
    return false;
  }

  if (!startAccessPoint()) {
    return false;
  }

  webServer_ = new WebServer(kWebServerPort);
  webServer_->on("/", [this]() { return handleRoot(); });
  webServer_->on("/configure", HTTP_POST, [this]() { return handleConfigure(); });
  webServer_->on("/status", [this]() { return handleStatus(); });
  webServer_->begin();

  active_ = true;
  return true;
}

void ProvisioningService::update() {
  if (!active_ || webServer_ == nullptr) {
    return;
  }
  webServer_->handleClient();
}

void ProvisioningService::stop() {
  if (webServer_ != nullptr) {
    delete webServer_;
    webServer_ = nullptr;
  }
  stopAccessPoint();
  active_ = false;
}

bool ProvisioningService::handleRoot() {
  webServer_->setContentLength(strlen(kSetupPageHtml));
  webServer_->send(200, "text/html", kSetupPageHtml);
  return true;
}

bool ProvisioningService::handleStatus() {
  // Minimal JSON status for potential future polling extensions.
  webServer_->send(200, "application/json", "{\"active\":true}");
  return true;
}

bool ProvisioningService::handleConfigure() {
  if (!webServer_->hasArg("ssid")) {
    webServer_->send(400, "application/json", "{\"success\":false,\"error\":\"Missing SSID\"}");
    return false;
  }

  String ssid = webServer_->arg("ssid");
  String passphrase = webServer_->arg("passphrase");

  // Trim whitespace.
  ssid.trim();
  passphrase.trim();

  if (ssid.length() == 0) {
    webServer_->send(400, "application/json", "{\"success\":false,\"error\":\"SSID cannot be empty\"}");
    return false;
  }

  if (!DeviceCore::validateNetworkConfiguration(ssid.c_str(), passphrase.c_str())) {
    webServer_->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid network configuration\"}");
    return false;
  }

  // Attempt real Wi-Fi connection before persisting.  The device must confirm
  // a successful connection before marking itself provisioned.  The station
  // interface runs alongside the setup AP (AP+STA), so the client keeps its
  // connection to this page while the credentials are being tested.
  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), passphrase.c_str());

  const uint32_t startMs = millis();
  const uint32_t timeoutMs = 20000;  // 20-second connection timeout.

  while (WiFi.status() != WL_CONNECTED && millis() - startMs < timeoutMs) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    const wl_status_t status = WiFi.status();
    // Disconnect only the station interface.  Stopping the radio here would
    // take the setup AP down with it, stranding the client without an error
    // response or a way to retry.
    WiFi.disconnect();
    char error[128];
    snprintf(error, sizeof(error),
             "{\"success\":false,\"error\":\"Connection failed: %s\"}",
             wifiFailureDetail(status));
    webServer_->send(200, "application/json", error);
    return false;
  }

  // Connection confirmed.  Persist via DeviceCore and mark provisioned.
  const CoreResult result = deviceCore_.configureNetwork(ssid.c_str(), passphrase.c_str());
  if (result != CoreResult::kOk) {
    WiFi.disconnect();
    const String error = "{\"success\":false,\"error\":\"Failed to save configuration\"}";
    webServer_->send(200, "application/json", error.c_str());
    return false;
  }

  const String response = "{\"success\":true}";
  webServer_->send(200, "application/json", response.c_str());

  // Allow the response to reach the client before restarting.
  delay(500);
  ESP.restart();
  return true;
}

bool ProvisioningService::startAccessPoint() {
  char ssid[32];
  if (!buildSsid(ssid, sizeof(ssid))) {
    return false;
  }

  // Open AP (no password) for local setup.  The setup page is not authenticated
  // and this AP exists only during initial provisioning.
  const bool started = WiFi.softAP(ssid, nullptr, kApChannel, 0, kApMaxConnections);
  if (started) {
    apStarted_ = true;
    Serial.print(F("Setup AP started: "));
    Serial.println(ssid);
    Serial.print(F("Setup page: http://"));
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println(F("Failed to start setup Access Point"));
  }

  return started;
}

void ProvisioningService::stopAccessPoint() {
  if (apStarted_) {
    WiFi.softAPdisconnect(true);
    apStarted_ = false;
  }
}

}  // namespace guardian_x
