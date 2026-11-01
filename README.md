# ESP32PcSession

An Arduino library that runs the ESP32 side of the serial SD-card file manager. It serves folder listings and file transfers over the board's USB-to-UART COM port. The PC uses its companion Windows app; no Wi-Fi or USB mass-storage mode is involved.

## Dependency

Install the separate **ESP32StorageLibrary** first. ESP32PcSession calls its `StorageLibrary.beginStorage()`, `beginHostSession()`, `writeWithFallback()`, and `endHostSession()` APIs. It also requires ArduinoJson 7 and the ESP32 Arduino core 3.x.

## Example

Open **File → Examples → ESP32PcSession → SerialFileManager** in Arduino IDE:

```cpp
#include <ESP32StorageLibrary.h>
#include <ESP32PcSession.h>

ESP32PcSession server;

void setup() {
  server.beginServer(5, 115200);  // SD CS pin, serial baud rate
}

void loop() {
  server.poll();
}
```

The library handles the serial protocol, browsing, uploads, downloads, transfer CRC checks, and session begin/end. SD remains mounted so the firmware can perform the PC's file requests. While the PC owns the transfer session, send new ESP32 records through `StorageLibrary.writeWithFallback(record)` or `server.writeRecord(record)`; the StorageLibrary host-session gate queues them in LittleFS until the session ends. Avoid direct application writes to SD during a PC session.

## Sensor-save pause and resume

The library exposes `notifySensorDataPending()` and `notifySensorDataSaved()` for applications that must pause PC access before writing sensor data to SD. Call `notifySensorDataPending()` when the sketch has sensor records waiting to be committed. The companion app receives `SENSOR_DATA_PENDING`, ends the SD file session, and keeps the COM port open. After the application confirms that the waiting records are saved, call `notifySensorDataSaved()`. The app receives `SENSOR_DATA_SAVED` and begins a new file session automatically.

These methods only send protocol notifications. The sketch remains responsible for detecting pending sensor data, coordinating its storage task, and calling the saved notification only after the write is complete. The methods do nothing unless a PC session is active and a pending notification has been sent.
## Hardware and wiring

The library is for the classic ESP32 Dev Module / ESP32-WROOM. For the default SPI wiring, connect microSD SCK to GPIO18, MISO to GPIO19, MOSI to GPIO23, and CS to GPIO5. The SD card should be FAT32. Change the CS pin in `beginServer()` if your wiring differs.

This project has only been tested on the **ESP32 Dev Module**. Other boards have not been tested.

## Companion PC app

Use the Windows app in the separate **ESP32 Serial File Manager** project. Select the board's COM port, connect, and browse or transfer files. Close Arduino Serial Monitor before connecting the app.
