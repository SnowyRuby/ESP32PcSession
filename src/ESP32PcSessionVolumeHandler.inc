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
    d["build"] = "serial-file-manager-v1";
    d["state"] = mode == READY ? "READY" : mode == PC_SESSION ? "PC_SESSION" : "RECOVERY";
    d["sdReady"] = StorageLibrary.isSDReady();
    d["littlefsReady"] = StorageLibrary.isLittleFSReady();
    d["sdUsedBytes"] = StorageLibrary.isSDReady() ? ::SD.usedBytes() : 0;
    d["sdTotalBytes"] = StorageLibrary.isSDReady() ? ::SD.totalBytes() : 0;
    d["littlefsUsedBytes"] = StorageLibrary.isLittleFSReady() ? ::LittleFS.usedBytes() : 0;
    d["littlefsTotalBytes"] = StorageLibrary.isLittleFSReady() ? ::LittleFS.totalBytes() : 0;
    reply(id, cmd, true, &d);
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
    if (uploadActive) {
      uploadFile.close();
      if (uploadFilesystem) uploadFilesystem->remove(uploadStagePath);
      uploadActive = false;
      uploadFilesystem = nullptr;
      uploadPath = "";
      uploadStagePath = "";
    }
    if (!serverInstance->end()) {
      mode = RECOVERY;
      reply(id, cmd, false, nullptr, serverInstance->lastError());
      return;
    }
    const bool sdAvailable = StorageLibrary.isSDReady();
    mode = StorageLibrary.isLittleFSReady() ? READY : RECOVERY;
    StaticJsonDocument<96> d;
    d["queueSynced"] = sdAvailable;
    d["queuePending"] = !sdAvailable;
    reply(id, cmd, true, &d);
    return;
  }

  if (!requireSession(id, cmd)) return;
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
    d["offset"] = offset;
    d["data"] = encoded;
    d["crc32"] = hex32(crcUpdate(0xFFFFFFFFUL, buffer, got) ^ 0xFFFFFFFFUL);
    reply(id, cmd, true, &d);
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
    const int slash = localPath.lastIndexOf('/');
    if (slash > 0 && !makeParents(fs, localPath.substring(0, slash))) {
      reply(id, cmd, false, nullptr, "Could not create destination folder.");
      return;
    }
    uploadStagePath = "/.pc-tmp/upload.bin";
    if (!fs.exists("/.pc-tmp")) fs.mkdir("/.pc-tmp");
    fs.remove(uploadStagePath);
    uploadFile = fs.open(uploadStagePath, FILE_WRITE);
    if (!uploadFile) {
      reply(id, cmd, false, nullptr, "Could not create staging file.");
      return;
    }
    uploadFilesystem = &fs;
    uploadPath = path;
    uploadSize = size;
    uploadExpectedCrc = expectedCrc;
    uploadReceived = 0;
    uploadActive = true;
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
    uploadFilesystem = nullptr;
    uploadPath = "";
    uploadStagePath = "";
    reply(id, cmd, true);
    return;
  }

  if (cmd == "PUT_ABORT") {
    if (uploadActive && path == uploadPath) {
      uploadFile.close();
      if (uploadFilesystem) uploadFilesystem->remove(uploadStagePath);
      uploadActive = false;
      uploadFilesystem = nullptr;
      uploadPath = "";
      uploadStagePath = "";
    }
    reply(id, cmd, true);
    return;
  }

  reply(id, cmd, false, nullptr, "Unknown command.");
}
