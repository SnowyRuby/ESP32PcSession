static const char *pcResumeStagePath = "/.pc-tmp/upload.bin";
static const char *pcResumeMetaPath = "/.pc-tmp/upload.json";

bool pcSaveUploadMetadata(fs::FS &fs, const String &path, uint64_t size, uint32_t crc) {
  if (!fs.exists("/.pc-tmp") && !fs.mkdir("/.pc-tmp")) return false;
  fs.remove(pcResumeMetaPath);
  File meta = fs.open(pcResumeMetaPath, FILE_WRITE);
  if (!meta) return false;
  StaticJsonDocument<320> document;
  document["path"] = path;
  document["size"] = size;
  document["crc32"] = hex32(crc);
  const bool ok = serializeJson(document, meta) > 0;
  meta.flush();
  meta.close();
  return ok;
}

bool pcLoadUploadMetadata(fs::FS &fs, String &path, uint64_t &size, uint32_t &crc) {
  File meta = fs.open(pcResumeMetaPath, FILE_READ);
  if (!meta) return false;
  StaticJsonDocument<320> document;
  const DeserializationError error = deserializeJson(document, meta);
  meta.close();
  if (error || !document["path"].is<const char *>() || !document["crc32"].is<const char *>()) return false;
  path = document["path"].as<String>();
  size = document["size"] | UINT64_MAX;
  return size != UINT64_MAX && parseCrc(document["crc32"], crc);
}

uint32_t pcCrcFilePrefix(File &file, uint64_t length, bool &ok) {
  uint32_t crc = 0xFFFFFFFFUL;
  uint8_t buffer[512];
  uint64_t processed = 0;
  ok = file && file.seek(0);
  while (ok && processed < length) {
    const size_t wanted = (size_t)min((uint64_t)sizeof(buffer), length - processed);
    const size_t got = file.read(buffer, wanted);
    if (got != wanted) { ok = false; break; }
    crc = crcUpdate(crc, buffer, got);
    processed += got;
  }
  return crc ^ 0xFFFFFFFFUL;
}

void pcCloseUploadForResume() {
  if (uploadActive) {
    uploadFile.flush();
    uploadFile.close();
    uploadActive = false;
  }
  uploadFilesystem = nullptr;
  uploadPath = "";
  uploadStagePath = "";
}

bool pcStageUpload(fs::FS &fs, const String &path, uint64_t size, uint32_t crc, bool append, uint64_t offset) {
  if (!fs.exists("/.pc-tmp") && !fs.mkdir("/.pc-tmp")) return false;
  uploadStagePath = pcResumeStagePath;
  uploadFile = fs.open(uploadStagePath, append ? FILE_APPEND : FILE_WRITE);
  if (!uploadFile) return false;
  uploadFilesystem = &fs;
  uploadPath = path;
  uploadSize = size;
  uploadExpectedCrc = crc;
  uploadReceived = offset;
  uploadActive = true;
  return true;
}
void addStorageEntry(JsonArray entries, const char *name, const char *volume,
                    uint64_t usedBytes, uint64_t totalBytes) {
  JsonObject entry = entries.add<JsonObject>();
  entry["name"] = name;
  entry["dir"] = true;
  entry["size"] = usedBytes;
  entry["totalBytes"] = totalBytes;
  entry["volume"] = volume;
}

void handleVolumes(JsonDocument &q) {
  const uint32_t id = q["id"] | 0;
  const String cmd = q["type"] | "";
  const String path = q["path"] | "";
  if ((q["v"] | 0) != 1) {
    reply(id, cmd, false, nullptr, "Unsupported protocol version.");
    return;
  }

  if (cmd == "HELLO") {
    StaticJsonDocument<512> d;
    d["device"] = "ESP32-WROOM";
    if (serverInstance && serverInstance->deviceName().length())
      d["name"] = serverInstance->deviceName();
    d["build"] = "serial-file-manager-v2";
    d["pcSessionVersion"] = ESP32PcSession::version();
    d["storageLibraryVersion"] = StorageLibrary.version();
    d["state"] = mode == READY ? "READY" : mode == PC_SESSION ? "PC_SESSION" : "RECOVERY";
    d["sdReady"] = StorageLibrary.isSDReady();
    d["littlefsReady"] = StorageLibrary.isLittleFSReady();
    d["sdUsedBytes"] = StorageLibrary.isSDReady() ? ::SD.usedBytes() : 0;
    d["sdTotalBytes"] = StorageLibrary.isSDReady() ? ::SD.totalBytes() : 0;
    d["littlefsUsedBytes"] = StorageLibrary.isLittleFSReady() ? ::LittleFS.usedBytes() : 0;
    d["littlefsTotalBytes"] = StorageLibrary.isLittleFSReady() ? ::LittleFS.totalBytes() : 0;
    d["baud"] = pendingSerialBaud ? pendingSerialBaud : activeSerialBaud;
    d["baudPending"] = pendingSerialBaud != 0;
    d["binaryTransfer"] = 2;
    d["downloadBatch"] = 1;
    d["binaryMaxPayload"] = FAST_MAX_PAYLOAD;
    d["binaryMaxWindow"] = FAST_MAX_WINDOW;
    JsonArray rates = d["baudRates"].to<JsonArray>();
    for (uint32_t supported : SUPPORTED_BAUD_RATES) rates.add(supported);
    reply(id, cmd, true, &d);
    return;
  }

  if (cmd == "SET_BAUD") {
    const uint32_t baud = q["baud"] | 0;
    if (!isSupportedBaud(baud)) {
      reply(id, cmd, false, nullptr, "Unsupported serial baud rate.");
      return;
    }
    if (pendingSerialBaud) {
      reply(id, cmd, false, nullptr, "A baud-rate change is already pending.");
      return;
    }
    StaticJsonDocument<96> d;
    d["baud"] = baud;
    d["switchDelayMs"] = BAUD_SWITCH_DELAY_MS;
    d["fallbackDelayMs"] = BAUD_FALLBACK_DELAY_MS;
    reply(id, cmd, true, &d);
    Serial.flush();
    delay(BAUD_SWITCH_DELAY_MS);
    Serial.updateBaudRate(baud);
    pendingSerialBaud = baud;
    pendingBaudDeadline = millis() + BAUD_FALLBACK_DELAY_MS;
    return;
  }

  if (cmd == "BAUD_CONFIRM") {
    const uint32_t baud = q["baud"] | 0;
    if (pendingSerialBaud == baud) {
      activeSerialBaud = baud;
      pendingSerialBaud = 0;
      pendingBaudDeadline = 0;
      reply(id, cmd, true);
      return;
    }
    if (!pendingSerialBaud && activeSerialBaud == baud) {
      reply(id, cmd, true);
      return;
    }
    reply(id, cmd, false, nullptr, "Baud-rate confirmation does not match the pending rate.");
    return;
  }
  if (cmd == "SESSION_BEGIN") {
    if ((mode != READY && mode != PC_SESSION) || !serverInstance) {
      reply(id, cmd, false, nullptr, "Storage is not ready for a PC session.");
      return;
    }
    if (!serverInstance->begin()) {
      reply(id, cmd, false, nullptr, serverInstance->lastError());
      return;
    }
    mode = PC_SESSION;
    reply(id, cmd, true);
    return;
  }

  if (cmd == "SESSION_END") {
    if ((mode != PC_SESSION && mode != RECOVERY) || !serverInstance) {
      reply(id, cmd, false, nullptr, "No active PC session.");
      return;
    }
    if (uploadActive) pcCloseUploadForResume();
    if (pcTransferHoldActive()) {
      serverInstance->suspendForResume();
      mode = PC_SESSION;
      StaticJsonDocument<96> pending;
      pending["transferPending"] = true;
      pending["queuePending"] = true;
      reply(id, cmd, true, &pending);
      return;
    }
    if (!serverInstance->end()) {
      mode = RECOVERY;
      reply(id, cmd, false, nullptr, serverInstance->lastError());
      return;
    }
    mode = StorageLibrary.isLittleFSReady() ? READY : RECOVERY;
    StaticJsonDocument<96> d;
    d["queueSynced"] = true;
    reply(id, cmd, true, &d);
    if (activeSerialBaud != defaultSerialBaud) restoreDefaultSerialBaud();
    return;
  }

  if (!requireSession(id, cmd)) return;
  if (cmd == "TRANSFER_STATUS") {
    StaticJsonDocument<64> result;
    result["pending"] = pcTransferHoldActive();
    reply(id, cmd, true, &result);
    return;
  }
  if (cmd == "TRANSFER_HOLD") {
    if (!StorageLibrary.isLittleFSReady()) {
      reply(id, cmd, false, nullptr, "LittleFS is unavailable for the persistent transfer hold.");
      return;
    }
    fs::FS &littleFs = StorageLibrary.LittleFS.filesystem();
    if (!littleFs.exists("/.pc-tmp") && !littleFs.mkdir("/.pc-tmp")) {
      reply(id, cmd, false, nullptr, "Could not create the transfer hold folder.");
      return;
    }
    File hold = littleFs.open(pcTransferHoldPath, FILE_WRITE);
    if (!hold) {
      reply(id, cmd, false, nullptr, "Could not save the transfer hold on LittleFS.");
      return;
    }
    hold.print("transfer pending");
    hold.flush();
    hold.close();
    reply(id, cmd, true);
    return;
  }
  if (cmd == "TRANSFER_RELEASE") {
    const bool discard = q["discard"] | false;
    if (uploadActive) pcCloseUploadForResume();
    if (!discard) {
      bool pendingUpload = false;
      fs::FS *candidates[] = { &StorageLibrary.SD.filesystem(), &StorageLibrary.LittleFS.filesystem() };
      for (fs::FS *candidate : candidates) {
        String storedPath;
        uint64_t storedSize = 0;
        uint32_t storedCrc = 0;
        if (pcLoadUploadMetadata(*candidate, storedPath, storedSize, storedCrc)) pendingUpload = true;
      }
      if (pendingUpload) {
        reply(id, cmd, false, nullptr, "An unfinished upload still needs to be resumed or discarded.");
        return;
      }
    } else {
      fs::FS *candidates[] = { &StorageLibrary.SD.filesystem(), &StorageLibrary.LittleFS.filesystem() };
      for (fs::FS *candidate : candidates) {
        candidate->remove(pcResumeStagePath);
        candidate->remove(pcResumeMetaPath);
      }
    }
    if (StorageLibrary.isLittleFSReady()) StorageLibrary.LittleFS.filesystem().remove(pcTransferHoldPath);
    clearDownloadTransfer();
    reply(id, cmd, true);
    return;
  }
  if (!validPath(path)) {
    reply(id, cmd, false, nullptr, "Invalid, reserved, or protected storage path.");
    return;
  }
  if (path == "/" && cmd != "LIST") {
    reply(id, cmd, false, nullptr, "Choose a storage volume first.");
    return;
  }

  if (cmd == "LIST" && path == "/") {
    StaticJsonDocument<512> d;
    JsonArray entries = d["entries"].to<JsonArray>();
    if (StorageLibrary.isSDReady())
      addStorageEntry(entries, "SD card", "/sd", ::SD.usedBytes(), ::SD.totalBytes());
    if (StorageLibrary.isLittleFSReady())
      addStorageEntry(entries, "LittleFS", "/littlefs", ::LittleFS.usedBytes(), ::LittleFS.totalBytes());
    d["more"] = false;
    d["next"] = entries.size();
    reply(id, cmd, true, &d);
    return;
  }

  const String localPath = filesystemPath(path);
  fs::FS &fs = filesystemFor(path);
  ESP32StorageLibrary &storage = storageFor(path);

  if (cmd == "UNARCHIVE") {
    const String destination = q["destination"] | "";
    if (!validPath(destination) || destination == "/" || isVolumeRoot(destination) ||
        volumeForPath(destination) != volumeForPath(path)) {
      reply(id, cmd, false, nullptr, "The ZIP and destination must be on the same storage volume.");
      return;
    }
    File archive = fs.open(localPath, FILE_READ);
    if (!archive || archive.isDirectory()) {
      if (archive) archive.close();
      reply(id, cmd, false, nullptr, "ZIP archive not found.");
      return;
    }
    archive.close();
    const String destinationLocalPath = filesystemPath(destination);
    File destinationFolder = fs.open(destinationLocalPath, FILE_READ);
    const bool destinationIsDirectory = destinationFolder && destinationFolder.isDirectory();
    if (destinationFolder) destinationFolder.close();
    if (!destinationIsDirectory) {
      reply(id, cmd, false, nullptr, "Destination folder not found.");
      return;
    }
    if (!storage.unarchiveFolder(localPath, destinationLocalPath)) {
      reply(id, cmd, false, nullptr, storage.lastError());
      return;
    }
    reply(id, cmd, true);
    return;
  }

  if (cmd == "FAST_PUT_SWITCH") {
    const uint64_t offset = q["offset"] | UINT64_MAX;
    const uint16_t requestedPayload = q["requestedPayload"] | FAST_MAX_PAYLOAD;
    const uint8_t requestedWindow = q["requestedWindow"] | FAST_MAX_WINDOW;
    if (offset == UINT64_MAX) {
      reply(id, cmd, false, nullptr, "Invalid upload offset.");
      return;
    }
    startFastUpload(id, cmd, path, offset, requestedPayload, requestedWindow);
    return;
  }

  if (cmd == "FAST_GET_SWITCH") {
    const uint64_t offset = q["offset"] | 0;
    const uint16_t requestedPayload = q["requestedPayload"] | FAST_MAX_PAYLOAD;
    const uint8_t requestedWindow = q["requestedWindow"] | FAST_MAX_WINDOW;
    uint32_t expectedCrc = 0;
    if (!parseCrc(q["crc32"], expectedCrc)) {
      reply(id, cmd, false, nullptr, "Invalid download CRC.");
      return;
    }
    startFastDownload(id, cmd, path, offset, requestedPayload, requestedWindow, expectedCrc);
    return;
  }

  if (cmd == "LIST") {
    File dir = fs.open(localPath, FILE_READ);
    if (!dir || !dir.isDirectory()) {
      reply(id, cmd, false, nullptr, "Folder not found.");
      return;
    }
    StaticJsonDocument<3600> d;
    JsonArray entries = d["entries"].to<JsonArray>();
    uint32_t skip = q["offset"] | 0;
    File file = dir.openNextFile();
    uint32_t seen = 0;
    while (file && seen < skip) {
      file.close();
      file = dir.openNextFile();
      seen++;
    }
    uint8_t count = 0;
    while (file && count < 24) {
      String name = String(file.name());
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      const bool reservedQueue = volumeForPath(path) == "/littlefs" && localPath == "/" && name.equalsIgnoreCase("StorageLibraryQueue");
      const bool reservedBackup = volumeForPath(path) == "/sd" && localPath == "/" && name.equalsIgnoreCase("StorageLibraryBackup");
      const bool reservedStage = localPath == "/" && name.equalsIgnoreCase(".pc-tmp");
      if (name.length() && !reservedQueue && !reservedBackup && !reservedStage) {
        JsonObject entry = entries.add<JsonObject>();
        entry["name"] = name;
        entry["dir"] = file.isDirectory();
        entry["size"] = file.isDirectory() ? 0 : file.size();
        const time_t modified = file.getLastWrite();
        if (modified >= 315532800) {
          entry["modified"] = static_cast<int64_t>(modified);
        }
        count++;
      }
      file.close();
      file = dir.openNextFile();
      seen++;
    }
    d["more"] = (bool)file;
    d["next"] = seen;
    if (file) file.close();
    dir.close();
    reply(id, cmd, true, &d);
    return;
  }

  if (cmd == "MKDIR") {
    const int slash = localPath.lastIndexOf('/');
    if (isVolumeRoot(path) || fs.exists(localPath) ||
        (slash > 0 && !makeParents(fs, localPath.substring(0, slash))) ||
        !storage.createFolder(localPath)) {
      reply(id, cmd, false, nullptr, "Could not create folder.");
      return;
    }
    reply(id, cmd, true);
    return;
  }

  if (cmd == "DELETE") {
    if (isVolumeRoot(path)) {
      reply(id, cmd, false, nullptr, "Cannot delete a storage root.");
      return;
    }
    File entry = fs.open(localPath, FILE_READ);
    if (!entry) {
      reply(id, cmd, false, nullptr, "Path not found.");
      return;
    }
    const bool isDirectory = entry.isDirectory();
    entry.close();
    const bool ok = isDirectory ? storage.deleteFolder(localPath) : storage.deleteFile(localPath);
    if (!ok) {
      reply(id, cmd, false, nullptr, storage.lastError());
      return;
    }
    reply(id, cmd, true);
    return;
  }

  if (cmd == "RENAME" || cmd == "MOVE" || cmd == "COPY") {
    const String destination = q["destination"] | "";
    if (!validPath(destination) || destination == "/" || isVolumeRoot(destination) || isVolumeRoot(path)) {
      reply(id, cmd, false, nullptr, "Invalid or protected destination path.");
      return;
    }
    const String sourceVolume = volumeForPath(path);
    const String destinationVolume = volumeForPath(destination);
    const String destinationLocalPath = filesystemPath(destination);
    fs::FS &destinationFs = filesystemFor(destination);
    ESP32StorageLibrary &destinationStorage = storageFor(destination);
    if (cmd == "RENAME" && sourceVolume != destinationVolume) {
      reply(id, cmd, false, nullptr, "RENAME must stay in the same storage volume.");
      return;
    }
    if (cmd == "RENAME") {
      const int sourceSlash = path.lastIndexOf('/');
      const int destinationSlash = destination.lastIndexOf('/');
      if (sourceSlash < 0 || destinationSlash < 0 ||
          path.substring(0, sourceSlash) != destination.substring(0, destinationSlash)) {
        reply(id, cmd, false, nullptr, "RENAME must keep the item in its current folder; use MOVE to change folders.");
        return;
      }
    }
    if (destinationFs.exists(destinationLocalPath)) {
      reply(id, cmd, false, nullptr, "The destination already exists.");
      return;
    }
    const int destinationSlash = destinationLocalPath.lastIndexOf('/');
    if (destinationSlash > 0 && !makeParents(destinationFs, destinationLocalPath.substring(0, destinationSlash))) {
      reply(id, cmd, false, nullptr, "Could not create destination folder.");
      return;
    }

    bool ok = false;
    if (sourceVolume == destinationVolume) {
      ok = cmd == "COPY"
          ? storage.copyPath(localPath, destinationLocalPath)
          : cmd == "MOVE"
              ? storage.movePath(localPath, destinationLocalPath)
              : storage.renamePath(localPath, destinationLocalPath);
    } else if (cmd == "COPY" || cmd == "MOVE") {
      ok = copyTree(fs, localPath, destinationFs, destinationLocalPath);
      if (ok && cmd == "MOVE") {
        File sourceEntry = fs.open(localPath, FILE_READ);
        const bool sourceIsDirectory = sourceEntry && sourceEntry.isDirectory();
        if (sourceEntry) sourceEntry.close();
        ok = sourceIsDirectory ? storage.deleteFolder(localPath) : storage.deleteFile(localPath);
      }
    }
    if (!ok) {
      const String error = storage.lastError().length() ? storage.lastError() : "The storage operation could not be completed.";
      reply(id, cmd, false, nullptr, error);
      return;
    }
    reply(id, cmd, true);
    return;
  }

  if (cmd == "DOWNLOAD_BATCH") {
    const String destination = q["destination"] | "";
    JsonArrayConst items = q["items"].as<JsonArrayConst>();
    const String volume = volumeForPath(path);
    if (items.size() < 1 || items.size() > 64 || !volume.length() ||
        !validPath(destination) || volumeForPath(destination) != volume || fs.exists(filesystemPath(destination))) {
      reply(id, cmd, false, nullptr, "Invalid batch selection or archive destination.");
      return;
    }

    if (items.size() == 1) {
      const String source = items[0].as<String>();
      if (!validPath(source) || volumeForPath(source) != volume || source == path) {
        reply(id, cmd, false, nullptr, "A single-item batch must be a folder in the current storage volume.");
        return;
      }
      File folder = fs.open(filesystemPath(source), FILE_READ);
      if (!folder || !folder.isDirectory()) {
        if (folder) folder.close();
        reply(id, cmd, false, nullptr, "A single-item batch must be a folder.");
        return;
      }
      File child = folder.openNextFile();
      const bool hasChildren = (bool)child;
      if (child) child.close();
      folder.close();
      if (!hasChildren) {
        reply(id, cmd, false, nullptr, "The selected folder is empty.");
        return;
      }
    }

    const String stageLocal = "/.pc-tmp/download-batch-" + String((uint32_t)esp_random(), HEX);
    if (!fs.exists("/.pc-tmp") && !fs.mkdir("/.pc-tmp")) {
      reply(id, cmd, false, nullptr, "Could not create the protected batch staging folder.");
      return;
    }
    if (!fs.mkdir(stageLocal)) {
      reply(id, cmd, false, nullptr, "Could not create a batch staging folder.");
      return;
    }

    bool ok = true;
    String error;
    std::vector<String> copiedNames;
    for (JsonVariantConst item : items) {
      const String source = item.as<String>();
      if (!validPath(source) || volumeForPath(source) != volume || source == path) {
        ok = false;
        error = "A selected item is invalid or belongs to another storage volume.";
        break;
      }
      const int slash = source.lastIndexOf('/');
      const String name = source.substring(slash + 1);
      if (!name.length()) {
        ok = false;
        error = "A selected item has an invalid name.";
        break;
      }
      for (const String &previous : copiedNames) {
        if (previous.equalsIgnoreCase(name)) {
          ok = false;
          error = "Selected items contain duplicate names.";
          break;
        }
      }
      if (!ok) break;
      copiedNames.push_back(name);
      const String target = stageLocal + "/" + name;
      if (!storage.copyPath(filesystemPath(source), target)) {
        ok = false;
        error = storage.lastError().length() ? storage.lastError() : "Could not stage a selected item.";
        break;
      }
    }

    const String destinationLocal = filesystemPath(destination);
    if (ok && !storage.archiveFolder(stageLocal, destinationLocal)) {
      ok = false;
      error = storage.lastError().length() ? storage.lastError() : "Could not create the download batch archive.";
    }
    removeTree(fs, stageLocal);
    if (!ok) {
      if (fs.exists(destinationLocal)) fs.remove(destinationLocal);
    }
    if (!ok) {
      reply(id, cmd, false, nullptr, error);
      return;
    }

    File archive = fs.open(destinationLocal, FILE_READ);
    if (!archive || archive.isDirectory()) {
      if (archive) archive.close();
      fs.remove(destinationLocal);
      reply(id, cmd, false, nullptr, "The batch archive could not be reopened for download.");
      return;
    }
    const uint64_t size = archive.size();
    const uint32_t crc = crcFile(archive);
    archive.close();
    updateParentModifiedTime(destination);
    StaticJsonDocument<128> d;
    d["size"] = size;
    d["crc32"] = hex32(crc);
    reply(id, cmd, true, &d);
    return;
  }

  if (cmd == "GET_BEGIN") {
    File file = fs.open(localPath, FILE_READ);
    if (!file || file.isDirectory()) {
      if (file) file.close();
      reply(id, cmd, false, nullptr, "File not found.");
      return;
    }
    const uint64_t size = file.size();
    const uint32_t crc = crcFile(file);
    file.close();
    prepareDownload(path, size);
    StaticJsonDocument<96> d;
    d["size"] = size;
    d["crc32"] = hex32(crc);
    reply(id, cmd, true, &d);
    return;
  }

  if (cmd == "GET_CHUNK") {
    const uint64_t offset = q["offset"] | UINT64_MAX;
    const size_t length = q["length"] | 0;
    if (!length || length > CHUNK || offset == UINT64_MAX) {
      reply(id, cmd, false, nullptr, "Invalid chunk request.");
      return;
    }
    File file = fs.open(localPath, FILE_READ);
    if (!file || file.isDirectory() || offset > file.size()) {
      if (file) file.close();
      reply(id, cmd, false, nullptr, "File or offset is invalid.");
      return;
    }
    file.seek((uint32_t)offset);
    uint8_t buffer[CHUNK];
    const size_t got = file.read(buffer, length);
    file.close();
    if (got != length) {
      reply(id, cmd, false, nullptr, "Short read.");
      return;
    }
    StaticJsonDocument<1100> d;
    char encoded[CHUNK * 2 + 8];
    size_t encodedLength = 0;
    if (mbedtls_base64_encode((unsigned char *)encoded, sizeof(encoded), &encodedLength, buffer, got) != 0) {
      reply(id, cmd, false, nullptr, "Base64 encoding failed.");
      return;
    }
    encoded[encodedLength] = 0;
    noteDownloadChunk(path, offset, got);
    d["offset"] = offset;
    d["data"] = encoded;
    d["crc32"] = hex32(crcUpdate(0xFFFFFFFFUL, buffer, got) ^ 0xFFFFFFFFUL);
    reply(id, cmd, true, &d);
    return;
  }

  if (cmd == "PUT_STATUS") {
    if (uploadActive) pcCloseUploadForResume();
    const uint64_t size = q["size"] | UINT64_MAX;
    uint32_t expectedCrc = 0;
    const bool requestValid = size != UINT64_MAX && parseCrc(q["crc32"], expectedCrc);
    fs::FS &fs = filesystemFor(path);
    String storedPath;
    uint64_t storedSize = 0;
    uint32_t storedCrc = 0;
    const bool metadataMatches = requestValid && pcLoadUploadMetadata(fs, storedPath, storedSize, storedCrc) &&
      storedPath == path && storedSize == size && storedCrc == expectedCrc && fs.exists(pcResumeStagePath);
    uint64_t received = 0;
    uint32_t prefixCrc = 0;
    if (metadataMatches) {
      File staged = fs.open(pcResumeStagePath, FILE_READ);
      if (staged && staged.size() <= size) {
        received = staged.size();
        bool crcOk = false;
        prefixCrc = pcCrcFilePrefix(staged, received, crcOk);
        if (!crcOk) received = 0;
      }
      if (staged) staged.close();
    }
    StaticJsonDocument<160> result;
    result["matches"] = metadataMatches && received <= size;
    result["received"] = received;
    result["prefixCrc"] = hex32(prefixCrc);
    reply(id, cmd, true, &result);
    return;
  }

  if (cmd == "PUT_RESUME") {
    if (uploadActive) pcCloseUploadForResume();
    const uint64_t size = q["size"] | UINT64_MAX;
    const uint64_t requestedOffset = q["offset"] | UINT64_MAX;
    uint32_t expectedCrc = 0, expectedPrefixCrc = 0;
    if (isVolumeRoot(path) || size == UINT64_MAX || size > 1024ULL * 1024 * 1024 ||
        requestedOffset == UINT64_MAX || requestedOffset > size ||
        !parseCrc(q["crc32"], expectedCrc) || !parseCrc(q["prefixCrc"], expectedPrefixCrc)) {
      reply(id, cmd, false, nullptr, "Invalid resume metadata.");
      return;
    }
    fs::FS &fs = filesystemFor(path);
    String storedPath;
    uint64_t storedSize = 0;
    uint32_t storedCrc = 0;
    const bool metadataMatches = pcLoadUploadMetadata(fs, storedPath, storedSize, storedCrc) &&
      storedPath == path && storedSize == size && storedCrc == expectedCrc && fs.exists(pcResumeStagePath);
    bool prefixMatches = false;
    if (metadataMatches) {
      File staged = fs.open(pcResumeStagePath, FILE_READ);
      if (staged && staged.size() == requestedOffset) {
        bool crcOk = false;
        const uint32_t stagedCrc = pcCrcFilePrefix(staged, requestedOffset, crcOk);
        prefixMatches = crcOk && stagedCrc == expectedPrefixCrc;
      }
      if (staged) staged.close();
    }

    uint64_t acceptedOffset = prefixMatches ? requestedOffset : 0;
    if (!prefixMatches) {
      fs.remove(pcResumeStagePath);
      if (!pcSaveUploadMetadata(fs, path, size, expectedCrc)) {
        reply(id, cmd, false, nullptr, "Could not prepare upload recovery metadata.");
        return;
      }
    }
    if (!pcStageUpload(fs, path, size, expectedCrc, prefixMatches, acceptedOffset)) {
      reply(id, cmd, false, nullptr, "Could not reopen the upload staging file.");
      return;
    }
    StaticJsonDocument<64> result;
    result["offset"] = acceptedOffset;
    reply(id, cmd, true, &result);
    return;
  }

  if (cmd == "PUT_BEGIN") {
    if (uploadActive) {
      reply(id, cmd, false, nullptr, "Another upload is active.");
      return;
    }
    const uint64_t size = q["size"] | UINT64_MAX;
    uint32_t expectedCrc;
    if (isVolumeRoot(path) || size == UINT64_MAX || size > 1024ULL * 1024 * 1024 || !parseCrc(q["crc32"], expectedCrc)) {
      reply(id, cmd, false, nullptr, "Invalid size or CRC.");
      return;
    }
    fs::FS &fs = filesystemFor(path);
    const String localPath = filesystemPath(path);
    const int slash = localPath.lastIndexOf('/');
    if (slash > 0 && !makeParents(fs, localPath.substring(0, slash))) {
      reply(id, cmd, false, nullptr, "Could not create destination folder.");
      return;
    }
    if (!fs.exists("/.pc-tmp") && !fs.mkdir("/.pc-tmp")) {
      reply(id, cmd, false, nullptr, "Could not create protected staging folder.");
      return;
    }
    fs.remove(pcResumeStagePath);
    if (!pcSaveUploadMetadata(fs, path, size, expectedCrc) || !pcStageUpload(fs, path, size, expectedCrc, false, 0)) {
      fs.remove(pcResumeStagePath);
      fs.remove(pcResumeMetaPath);
      reply(id, cmd, false, nullptr, "Could not create upload staging file.");
      return;
    }
    reply(id, cmd, true);
    return;
  }

  if (cmd == "PUT_CHUNK") {
    if (!uploadActive || path != uploadPath) {
      reply(id, cmd, false, nullptr, "No matching upload.");
      return;
    }
    const uint64_t offset = q["offset"] | UINT64_MAX;
    const char *encoded = q["data"] | "";
    uint32_t expectedCrc;
    if (offset != uploadReceived || !parseCrc(q["crc32"], expectedCrc)) {
      reply(id, cmd, false, nullptr, "Unexpected offset or invalid CRC.");
      return;
    }
    uint8_t buffer[CHUNK];
    size_t decodedLength = 0, badIndex = 0;
    uint8_t badByte = 0;
    const size_t encodedLength = strlen(encoded);
    if (!decodeBase64(encoded, encodedLength, buffer, sizeof(buffer), decodedLength, badIndex, badByte)) {
      char message[128];
      snprintf(message, sizeof(message), "Invalid base64 at offset %lu (length %u, index %u, byte 0x%02X).",
               (unsigned long)offset, (unsigned)encodedLength, (unsigned)badIndex, (unsigned)badByte);
      reply(id, cmd, false, nullptr, String(message));
      return;
    }
    const uint32_t actualCrc = crcUpdate(0xFFFFFFFFUL, buffer, decodedLength) ^ 0xFFFFFFFFUL;
    if (!decodedLength || decodedLength > CHUNK || actualCrc != expectedCrc ||
        uploadReceived + decodedLength > uploadSize || uploadFile.write(buffer, decodedLength) != decodedLength) {
      reply(id, cmd, false, nullptr, "Chunk CRC, length, or staging write is invalid.");
      return;
    }
    // Avoid syncing SD metadata for every tiny serial chunk. The file is flushed
    // when the upload commits or when it is closed for resume.
    uploadReceived += decodedLength;
    reply(id, cmd, true);
    return;
  }

  if (cmd == "PUT_COMMIT") {
    if (!uploadActive || path != uploadPath || !uploadFilesystem) {
      reply(id, cmd, false, nullptr, "No matching upload.");
      return;
    }
    uploadFile.flush();
    uploadFile.close();
    uploadActive = false;
    fs::FS &uploadFs = *uploadFilesystem;
    const String destination = filesystemPath(path);
    File staged = uploadFs.open(uploadStagePath, FILE_READ);
    const bool verified = staged && staged.size() == uploadSize && crcFile(staged) == uploadExpectedCrc;
    if (staged) staged.close();
    if (!verified) {
      uploadFs.remove(uploadStagePath);
      uploadFs.remove(pcResumeMetaPath);
      uploadFilesystem = nullptr;
      uploadPath = "";
      uploadStagePath = "";
      reply(id, cmd, false, nullptr, "Whole-file verification failed.");
      return;
    }
    File existing = uploadFs.open(destination, FILE_READ);
    if (existing && existing.isDirectory()) {
      existing.close();
      uploadFs.remove(uploadStagePath);
      uploadFs.remove(pcResumeMetaPath);
      uploadFilesystem = nullptr;
      uploadPath = "";
      uploadStagePath = "";
      reply(id, cmd, false, nullptr, "A folder already exists at the upload destination.");
      return;
    }
    if (existing) existing.close();
    const String backupPath = "/.pc-tmp/previous.bin";
    const bool hadPrevious = uploadFs.exists(destination);
    if (hadPrevious) {
      uploadFs.remove(backupPath);
      if (!uploadFs.rename(destination, backupPath)) {
        uploadFs.remove(uploadStagePath);
        uploadFs.remove(pcResumeMetaPath);
        uploadFilesystem = nullptr;
        uploadPath = "";
        uploadStagePath = "";
        reply(id, cmd, false, nullptr, "Could not preserve the previous destination.");
        return;
      }
    }
    if (!uploadFs.rename(uploadStagePath, destination)) {
      if (hadPrevious) uploadFs.rename(backupPath, destination);
      uploadFilesystem = nullptr;
      uploadPath = "";
      uploadStagePath = "";
      reply(id, cmd, false, nullptr, "Could not commit upload.");
      return;
    }
    if (hadPrevious) uploadFs.remove(backupPath);
    uploadFs.remove(pcResumeMetaPath);
    uploadFilesystem = nullptr;
    uploadPath = "";
    uploadStagePath = "";
    updateParentModifiedTime(path);
    reply(id, cmd, true);
    return;
  }

  if (cmd == "PUT_ABORT") {
    fs::FS &fs = filesystemFor(path);
    if (uploadActive && path == uploadPath) pcCloseUploadForResume();
    String storedPath;
    uint64_t storedSize = 0;
    uint32_t storedCrc = 0;
    if (pcLoadUploadMetadata(fs, storedPath, storedSize, storedCrc) && storedPath == path) {
      fs.remove(pcResumeStagePath);
      fs.remove(pcResumeMetaPath);
    }
    reply(id, cmd, true);
    return;
  }
  reply(id, cmd, false, nullptr, "Unknown command.");
}
