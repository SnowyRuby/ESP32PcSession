# ESP32PcSession

An Arduino library that runs the ESP32 side of the serial file manager. It serves SD and LittleFS folder listings and file transfers over the board's USB-to-UART COM port. The PC uses its companion Windows app; no Wi-Fi or USB mass-storage mode is involved.

## Dependency

Install [**ESP32StorageLibrary 0.4.1 or newer**](https://github.com/SnowyRuby/ESP32StorageLibrary) first. ESP32PcSession uses its `copyPath()`, `renamePath()`, and `movePath()` helpers along with `StorageLibrary.beginStorage()`, `beginHostSession()`, `writeWithFallback()`, and `endHostSession()`. It also requires ArduinoJson 7 and the ESP32 Arduino core 3.x.

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

Existing `beginServer(csPin, baud)` calls remain source-compatible. To share an SPI bus already initialized by a TFT, pass its `SPIClass` pointer as the third argument, for example `server.beginServer(5, 115200, Screen.getSPIinstance())`. This forwards the bus to `StorageLibrary.beginStorage(...)`; leave it omitted or pass `nullptr` to use the default pin-based SPI initialization.

To show a friendly device name in the PC app's port list, call `server.setDeviceName("Workshop ESP32")` before `beginServer()`. If you leave it unset, the app shows the COM port name.

The app starts at a virtual **This PC** view. The server lists every mounted volume with used and total capacity, then supports browsing and file operations under `/sd` and `/littlefs`. Same-volume copy, move, and rename use StorageLibrary helpers; cross-volume copy and move transfer files between filesystems. LittleFS-only boards can still be browsed. The reserved `/StorageLibraryQueue` (LittleFS) and `/StorageLibraryBackup` (SD) folders are hidden and protected, and `.pc-tmp` is reserved for verified upload staging.

While the PC owns the transfer session, send new ESP32 records through `StorageLibrary.writeWithFallback(record)` or `server.writeRecord(record)`. The StorageLibrary host-session gate queues them in its protected LittleFS queue until the session ends; when SD is unavailable, queued records remain there for later replay. Avoid direct application writes to SD or to the protected LittleFS queue during a PC session.

## Sensor-save pause and resume

The library exposes `notifySensorDataPending()` and `notifySensorDataSaved()` for applications that must pause PC access before writing sensor data to SD. Call `notifySensorDataPending()` when the sketch has sensor records waiting to be committed. The companion app receives `SENSOR_DATA_PENDING`, ends the SD file session, and keeps the COM port open. After the application confirms that the waiting records are saved, call `notifySensorDataSaved()`. The app receives `SENSOR_DATA_SAVED` and begins a new file session automatically.

These methods only send protocol notifications. The sketch remains responsible for detecting pending sensor data, coordinating its storage task, and calling the saved notification only after the write is complete. The methods do nothing unless a PC session is active and a pending notification has been sent.

## Hardware and wiring

The library is for the classic ESP32 Dev Module / ESP32-WROOM. For the default SPI wiring, connect microSD SCK to GPIO18, MISO to GPIO19, MOSI to GPIO23, and CS to GPIO5. The SD card should be FAT32. Change the CS pin in `beginServer()` if your wiring differs.

This project has only been tested on the **ESP32 Dev Module**. Other boards have not been tested.

## Companion PC app

Use the Windows app in the separate **ESP32 Serial File Manager** project. Select the board's COM port, connect, and browse or transfer files. Close Arduino Serial Monitor before connecting the app.
