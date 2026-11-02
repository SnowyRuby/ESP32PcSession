// Example: run the ESP32 serial file server with the ESP32PcSession library.
//
// The library handles the serial protocol, SD and LittleFS browsing/transfers, and the
// LittleFS queue session. This sketch only starts it and keeps it polling.
// Connect the PC over the board's USB-to-UART COM port at 115200 baud.

#include <ESP32StorageLibrary.h>
#include <ESP32PcSession.h>

ESP32PcSession server;

void setup() {
  // Optional: set a friendly name shown in the PC app's device dropdown.
  // Leave this unset to show the COM port name instead.
  // server.setDeviceName("Workshop ESP32");

  // Use SD CS GPIO 5. The library initializes the ESP32StorageLibrary backends
  // and reports READY when LittleFS is available (SD is optional).
  // Keep the existing two-argument call, or pass a shared bus as argument 3:
  // server.beginServer(5, 115200, Screen.getSPIinstance());
  if (!server.beginServer(5, 115200)) {
    Serial.println("File server is in recovery mode; check LittleFS.");
  }
}

// In your sensor/storage task, call server.notifySensorDataPending()
// when readings need SD access. After the CSV write completes, call
// server.notifySensorDataSaved(); the PC app ends/resumes its file session
// while keeping this COM connection open.

void loop() {
  // Reads serial requests and services browse/upload/download commands.
  // During a PC session, route application records through
  // StorageLibrary.writeWithFallback(...) or server.writeRecord(...).
  server.poll();
}
