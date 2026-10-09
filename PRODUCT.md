# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

## Users

The first-run setup page is for the person physically commissioning a
GUARDIAN X device, such as a school technician or designated site operator.

## Product Purpose

GUARDIAN X is a local-first ESP32-S3 palm-based guardian/student dismissal
device. Phase 3 lets a new or recovered device join the site's Wi-Fi without
introducing the later management application.

## Operating Context

The operator joins the device's temporary local setup access point from a
phone or laptop, enters Wi-Fi details, waits for the device to verify them,
and then lets it restart into normal network mode.

## Capabilities and Constraints

- Firmware is Arduino C++ for an ESP32-S3 DevKitC-1.
- The setup page is a local, temporary provisioning surface; it is not a
  dashboard, management UI, API, or authentication system.
- Device configuration must remain inside the versioned, CRC-validated,
  dual-slot DeviceCore/LittleFS architecture.
- Existing SEN0677 and SSD1309 diagnostics remain available.
- No production credentials or guardian, student, or biometric data are
  exposed by provisioning.

## Evidence on Hand

The repository contains the firmware architecture, roadmap, Phase 1 sensor
diagnostics, Phase 2 core/storage implementation, and host unit tests. It has
no visual brand assets or existing web interface to inherit.

## Product Principles

- Verify a Wi-Fi connection before treating a device as provisioned.
- Keep recovery local, deterministic, and non-destructive to application data.
- Keep first-run setup narrowly focused on connectivity.
- Preserve hardware diagnostics and separate hardware from application state.
