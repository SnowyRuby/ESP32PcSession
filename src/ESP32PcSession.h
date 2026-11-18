#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SPI.h>
#include <mbedtls/base64.h>
#include <ESP32StorageLibrary.h>

#define ESP32_PC_SESSION_VERSION "0.7.0"
#define ESP32_PC_SESSION_VERSION_MAJOR 0
#define ESP32_PC_SESSION_VERSION_MINOR 7
#define ESP32_PC_SESSION_VERSION_PATCH 0

// Runs the USB-to-UART file protocol and coordinates PC access to SD and LittleFS.
// The application sketch calls the sensor notification methods when needed.
class ESP32PcSession {
public:
  static constexpr const char *version() { return ESP32_PC_SESSION_VERSION; }
  // Starts the serial file server and initializes the existing storage manager.
  // The sketch's setup() can call beginServer(); loop() should call poll().
  bool beginServer(uint8_t sdCsPin = 5, uint32_t baud = 115200,
                   SPIClass *sdSPI = nullptr);
  // Optional friendly name shown by the PC app instead of just the COM port.
  // Leave unset to have the PC app show the COM port name (for example, COM3).
  void setDeviceName(const String &name);
  const String &deviceName() const { return _deviceName; }
  void poll();
  // True only during active file chunks/uploads or a resumable transfer hold.
  bool isTransferActive() const;

  // Ask the PC app to end its file session while keeping the COM port open.
  // Call notifySensorDataSaved() after the queued sensor records reach storage.
  void notifySensorDataPending();
  void notifySensorDataSaved();

  // Call after StorageLibrary.beginStorage(). This drains old queue data, then
  // pauses StorageLibrary's automatic record writes and queue replay. SD stays
  // mounted so the serial file server can service PC commands.
  bool begin();

  // Call only after the PC has finished and closed all file handles. Clears the
  // host gate and syncs queued records. If that fails, call end() again to retry.
  bool end();

  // Pauses the serial server while preserving a pending transfer's queue hold.
  bool suspendForResume();

  // Route one complete ESP32 record through the existing fallback manager.
  // During a host session this is queued in LittleFS by StorageLibrary.
  bool writeRecord(const String &record);

  // Retry queued data when no host session is active.
  bool sync();

  bool isActive() const { return _active; }
  bool needsRecovery() const { return _needsRecovery; }
  const String &lastError() const { return _lastError; }

private:
  bool _active = false;
  bool _needsRecovery = false;
  bool _sensorDataNotificationPending = false;
  bool _sensorDataPendingDeferred = false;
  bool _sensorDataSavedDeferred = false;
  String _deviceName;
  String _lastError;
};
