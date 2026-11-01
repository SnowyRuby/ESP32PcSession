#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SPI.h>
#include <mbedtls/base64.h>
#include <ESP32StorageLibrary.h>

// Session coordinator for a USB-to-UART file server. The PC receives exclusive
// SD access while the ESP32's existing StorageLibrary queues app records in
// LittleFS. This library does not implement the serial wire protocol itself.
class ESP32PcSession {
public:
  // Starts the serial file server and initializes the existing storage manager.
  // The sketch's setup() can call beginServer(); loop() should call poll().
  bool beginServer(uint8_t sdCsPin = 5, uint32_t baud = 115200);
  void poll();

  // Call after StorageLibrary.beginStorage(). This drains old queue data, then
  // pauses StorageLibrary's automatic record writes and queue replay. SD stays
  // mounted so the serial file server can service PC commands.
  bool begin();

  // Call only after the PC has finished and closed all file handles. Clears the
  // host gate and syncs queued records. If that fails, call end() again to retry.
  bool end();

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
  String _lastError;
};
