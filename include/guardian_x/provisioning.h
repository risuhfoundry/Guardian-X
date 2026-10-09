#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <WebServer.h>

#include "guardian_x/device_core.h"

namespace guardian_x {

// Provisioning service manages the setup Access Point and local configuration
// web server for unprovisioned devices. It is deliberately independent of
// sensor I/O and does not authenticate callers (Phase 4 responsibility).
class ProvisioningService {
 public:
  explicit ProvisioningService(DeviceCore &deviceCore);

  // Begin provisioning if the device is unprovisioned.  Returns true when
  // provisioning mode is active (AP running, web server listening).  Returns
  // false if the device is already provisioned or if the AP failed to start.
  bool begin();

  // Service web server requests.  Call from loop() while provisioning is
  // active.
  void update();

  // True while the setup AP and web server are running.
  bool isActive() const { return active_; }

  // Stop the AP and web server.  Does not alter persisted state.
  void stop();

 private:
  static constexpr uint16_t kWebServerPort = 80;
  static constexpr uint8_t kApChannel = 1;
  static constexpr uint8_t kApMaxConnections = 1;

  bool handleRoot();
  bool handleConfigure();
  bool handleStatus();

  bool startAccessPoint();
  void stopAccessPoint();

  DeviceCore &deviceCore_;
  bool active_;
  bool apStarted_;
  WebServer *webServer_;
};

}  // namespace guardian_x
