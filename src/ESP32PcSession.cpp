#include <ESP32PcSession.h>
#include <LittleFS.h>
#include <time.h>
#include <utime.h>
#include <vector>

namespace {
enum Mode { RECOVERY, READY, PC_SESSION };
Mode mode = RECOVERY;
ESP32PcSession *serverInstance = nullptr;
static const char *pcTransferHoldPath = "/.pc-tmp/transfer-hold";
bool pcTransferHoldActive() {
  return StorageLibrary.isLittleFSReady() && LittleFS.exists(pcTransferHoldPath);
}
File uploadFile;
String uploadPath;
String uploadStagePath;
fs::FS *uploadFilesystem = nullptr;
uint64_t uploadSize = 0, uploadReceived = 0;
uint32_t uploadExpectedCrc = 0;
bool uploadActive = false;
String downloadPath;
uint64_t downloadSize = 0;
bool downloadPrepared = false;
bool downloadActive = false;
uint32_t downloadLastActivity = 0;
static constexpr uint32_t DOWNLOAD_IDLE_TIMEOUT_MS = 30000;

void prepareDownload(const String &path, uint64_t size) {
  if (downloadActive && downloadPath == path) {
    downloadLastActivity = millis();
    return;
  }
  downloadPath = path;
  downloadSize = size;
  downloadPrepared = size > 0;
  downloadActive = false;
  downloadLastActivity = 0;
}

void noteDownloadChunk(const String &path, uint64_t offset, size_t length) {
  if (!downloadPrepared || downloadPath != path) return;
  downloadActive = true;
  downloadLastActivity = millis();
  if (offset + length >= downloadSize) {
    downloadActive = false;
    downloadPrepared = false;
    downloadPath = "";
    downloadSize = 0;
  }
}

void clearDownloadTransfer() {
  downloadPrepared = false;
  downloadActive = false;
  downloadPath = "";
  downloadSize = 0;
  downloadLastActivity = 0;
}

void expireDownloadTransfer() {
  if (downloadActive &&
      (uint32_t)(millis() - downloadLastActivity) >= DOWNLOAD_IDLE_TIMEOUT_MS) {
    clearDownloadTransfer();
  }
}
uint32_t lastQueueSync = 0;
static constexpr size_t MAX_LINE = 4095;
char serialLine[MAX_LINE + 1];
size_t serialLineLength = 0;
bool serialLineOverflow = false;
static constexpr size_t CHUNK = 480;
static constexpr uint8_t FAST_FRAME_DATA = 1;
static constexpr uint8_t FAST_FRAME_ACK = 2;
static constexpr uint8_t FAST_FRAME_NAK = 3;
static constexpr uint8_t FAST_FRAME_FINISH = 4;
static constexpr uint8_t FAST_FRAME_FINISH_ACK = 5;
static constexpr uint8_t FAST_FRAME_ABORT = 6;
static constexpr uint8_t FAST_FRAME_ERROR = 7;
static constexpr uint8_t FAST_PROTOCOL_VERSION = 2;
static constexpr size_t FAST_HEADER_SIZE = 22;
static constexpr size_t FAST_MAX_PAYLOAD = 1024;
static constexpr uint8_t FAST_MAX_WINDOW = 4;
static constexpr size_t FAST_MAX_ENCODED = FAST_HEADER_SIZE + FAST_MAX_PAYLOAD + 20;
enum FastDirection : uint8_t { FAST_NONE, FAST_UPLOAD, FAST_DOWNLOAD };
FastDirection fastDirection = FAST_NONE;
bool binaryMode = false;
uint8_t fastEncoded[FAST_MAX_ENCODED];
size_t fastEncodedLength = 0;
bool fastJsonProbeActive = false;
bool fastJsonPrefixMatched = false;
size_t fastJsonProbeStart = 0;
uint8_t fastJsonProbePosition = 0;
uint32_t nextFastTransferId = 1;
uint32_t fastTransferId = 0;
uint32_t fastSequence = 0;
uint64_t fastOffset = 0;
uint64_t fastSize = 0;
uint32_t fastFileCrc = 0;
uint16_t fastPayloadSize = FAST_MAX_PAYLOAD;
uint8_t fastWindow = FAST_MAX_WINDOW;
uint8_t fastFramesInWindow = 0;
uint32_t fastLastActivity = 0;
static constexpr uint32_t FAST_IDLE_TIMEOUT_MS = 30000;
File fastDownloadFile;
uint32_t fastSendSequence = 0;
uint64_t fastSendOffset = 0;
uint32_t fastWindowEndSequence = 0;
uint64_t fastWindowEndOffset = 0;
bool fastWaitingForAck = false;
bool fastWaitingForFinishAck = false;
bool fastNakOutstanding = false;
bool fastDuplicateAckSent = false;
static constexpr uint32_t DEFAULT_SERIAL_BAUD = 115200;
static constexpr uint32_t BAUD_SWITCH_DELAY_MS = 150;
static constexpr uint32_t BAUD_FALLBACK_DELAY_MS = 3000;
static constexpr uint32_t SUPPORTED_BAUD_RATES[] = {921600, 460800, 230400};
uint32_t defaultSerialBaud = DEFAULT_SERIAL_BAUD;
uint32_t activeSerialBaud = DEFAULT_SERIAL_BAUD;
uint32_t pendingSerialBaud = 0;
uint32_t pendingBaudDeadline = 0;

bool isSupportedBaud(uint32_t baud) {
  for (uint32_t supported : SUPPORTED_BAUD_RATES)
    if (baud == supported) return true;
  return false;
}

void restoreDefaultSerialBaud() {
  Serial.flush();
  Serial.updateBaudRate(defaultSerialBaud);
  activeSerialBaud = defaultSerialBaud;
  pendingSerialBaud = 0;
  pendingBaudDeadline = 0;
  serialLineLength = 0;
  serialLineOverflow = false;
}

int base64Value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

bool decodeBase64(const char *src, size_t srcLen, uint8_t *dst,
                  size_t dstCap, size_t &dstLen, size_t &errorIndex,
                  uint8_t &errorByte) {
  dstLen = 0;
  errorIndex = 0;
  errorByte = 0;
  auto failAt = [&](size_t at) {
    errorIndex = at;
    errorByte = src && at < srcLen ? (uint8_t)src[at] : 0;
    return false;
  };
  if (!src || srcLen == 0) return failAt(0);
  if ((srcLen % 4) != 0) return failAt(srcLen - (srcLen % 4));

  for (size_t i = 0; i < srcLen; i += 4) {
    const bool last = (i + 4 == srcLen);
    int a = base64Value((unsigned char)src[i]);
    int b = base64Value((unsigned char)src[i + 1]);
    int c = src[i + 2] == '=' ? -2 : base64Value((unsigned char)src[i + 2]);
    int d = src[i + 3] == '=' ? -2 : base64Value((unsigned char)src[i + 3]);
    if (a < 0) return failAt(i);
    if (b < 0) return failAt(i + 1);
    if (c == -1) return failAt(i + 2);
    if (d == -1) return failAt(i + 3);
    if (c == -2 && d != -2) return failAt(i + 3);
    if ((c == -2 || d == -2) && !last) return failAt(c == -2 ? i + 2 : i + 3);
    if (c == -2 && (b & 0x0F) != 0) return failAt(i + 1);
    if (d == -2 && c >= 0 && (c & 0x03) != 0) return failAt(i + 2);

    const size_t bytes = c == -2 ? 1 : (d == -2 ? 2 : 3);
    if (dstLen + bytes > dstCap) return failAt(i);
    dst[dstLen++] = (uint8_t)((a << 2) | (b >> 4));
    if (bytes > 1) dst[dstLen++] = (uint8_t)((b << 4) | (c >> 2));
    if (bytes > 2) dst[dstLen++] = (uint8_t)((c << 6) | d);
  }
  return true;
}
uint32_t crcUpdate(uint32_t c,const uint8_t *p,size_t n) {
  while(n--){c^=*p++;for(uint8_t i=0;i<8;i++)c=(c&1)?(0xEDB88320UL^(c>>1)):(c>>1);}return c;
}
uint32_t crcFile(File &f){uint32_t c=0xFFFFFFFFUL;uint8_t b[512];while(f.available()){size_t n=f.read(b,sizeof(b));c=crcUpdate(c,b,n);}return c^0xFFFFFFFFUL;}
String hex32(uint32_t n){char b[9];snprintf(b,sizeof(b),"%08lx",(unsigned long)n);return String(b);}
bool parseCrc(JsonVariantConst v,uint32_t &out){if(!v.is<const char*>())return false;const char*s=v.as<const char*>();char*end=nullptr;unsigned long n=strtoul(s,&end,16);if(end==s||*end||strlen(s)!=8)return false;out=(uint32_t)n;return true;}
bool isVolumeRoot(const String &path){return path=="/sd"||path=="/littlefs";}
String volumeForPath(const String &path){if(path=="/sd"||path.startsWith("/sd/"))return "/sd";if(path=="/littlefs"||path.startsWith("/littlefs/"))return "/littlefs";return "";}
String filesystemPath(const String &path){String volume=volumeForPath(path);if(!volume.length())return "/";String out=path.substring(volume.length());return out.length()?out:"/";}
bool updateParentModifiedTime(const String &virtualPath){
  const time_t now=time(nullptr);
  if(now<315532800)return false;
  struct utimbuf times;times.actime=now;times.modtime=now;
  bool updated=false;int slash=virtualPath.lastIndexOf('/');
  while(slash>0){const String parent=virtualPath.substring(0,slash);if(utime(parent.c_str(),&times)==0)updated=true;const int parentSlash=parent.lastIndexOf('/');if(parentSlash<=0||parent.substring(0,parentSlash)=="/sd"||parent.substring(0,parentSlash)=="/littlefs")break;slash=parentSlash;}
  return updated;
}
bool matchesProtectedPath(const String &path,const char *reserved){String root=reserved;if(path.equalsIgnoreCase(root))return true;return path.length()>root.length()&&path.substring(0,root.length()).equalsIgnoreCase(root)&&path[root.length()]=='/';}
bool validPath(const String&p){if(p=="/")return true;if(p.length()<3||p.length()>240||!volumeForPath(p).length()||p.indexOf("..")>=0||p.indexOf('\\')>=0)return false;for(size_t i=0;i<p.length();i++)if((uint8_t)p[i]<32)return false;String local=filesystemPath(p);if(matchesProtectedPath(local,"/.pc-tmp"))return false;if(volumeForPath(p)=="/sd"&&matchesProtectedPath(local,"/StorageLibraryBackup"))return false;if(volumeForPath(p)=="/littlefs"&&matchesProtectedPath(local,"/StorageLibraryQueue"))return false;return true;}
fs::FS &filesystemFor(const String &path){return volumeForPath(path)=="/littlefs"?StorageLibrary.LittleFS.filesystem():StorageLibrary.SD.filesystem();}
ESP32StorageLibrary &storageFor(const String &path){return volumeForPath(path)=="/littlefs"?StorageLibrary.LittleFS:StorageLibrary.SD;}
bool makeParents(fs::FS &fs,const String&p){int at=1;while((at=p.indexOf('/',at))>0){String d=p.substring(0,at);if(d!="/"&&!fs.exists(d))fs.mkdir(d);at++;}return true;}
bool removeTree(fs::FS &fs,const String &path){File node=fs.open(path,FILE_READ);if(!node)return false;bool directory=node.isDirectory();if(directory){while(true){File child=node.openNextFile();if(!child)break;String childName=String(child.name());int slash=childName.lastIndexOf('/');if(slash>=0)childName=childName.substring(slash+1);child.close();if(childName.length()&&!removeTree(fs,path=="/"?"/"+childName:path+"/"+childName)){node.close();return false;}}}node.close();return directory?fs.rmdir(path):fs.remove(path);}
bool copyTree(fs::FS &sourceFs,const String &source,fs::FS &destinationFs,const String &destination){File input=sourceFs.open(source,FILE_READ);if(!input||destinationFs.exists(destination)){if(input)input.close();return false;}if(input.isDirectory()){input.close();if(!destinationFs.mkdir(destination))return false;File directory=sourceFs.open(source,FILE_READ);while(true){File child=directory.openNextFile();if(!child)break;String childName=String(child.name());int slash=childName.lastIndexOf('/');if(slash>=0)childName=childName.substring(slash+1);bool isDirectory=child.isDirectory();child.close();String sourceChild=source=="/"?"/"+childName:source+"/"+childName;String destinationChild=destination=="/"?"/"+childName:destination+"/"+childName;if(!copyTree(sourceFs,sourceChild,destinationFs,destinationChild)){directory.close();removeTree(destinationFs,destination);return false;}}directory.close();return true;}File output=destinationFs.open(destination,FILE_WRITE);if(!output){input.close();return false;}uint8_t buffer[512];bool ok=true;while(input.available()){int count=input.read(buffer,sizeof(buffer));if(count<=0||output.write(buffer,(size_t)count)!=(size_t)count){ok=false;break;}}input.close();output.flush();output.close();if(!ok){destinationFs.remove(destination);return false;}File copied=destinationFs.open(destination,FILE_READ);File original=sourceFs.open(source,FILE_READ);bool same=copied&&original&&copied.size()==original.size()&&crcFile(copied)==crcFile(original);if(copied)copied.close();if(original)original.close();if(!same)destinationFs.remove(destination);return same;}
String cardPath(const String &path){return filesystemPath(path);}
fs::FS &sdCard(){return StorageLibrary.SD.filesystem();}
bool makeParents(const String &path){return makeParents(sdCard(),path);}

void sendResponse(uint32_t id,const String&type,JsonDocument&d){d["v"]=1;d["id"]=id;d["type"]=type;serializeJson(d,Serial);Serial.write('\n');}
void sendNotification(const char *type){StaticJsonDocument<96>d;d["v"]=1;d["type"]=type;serializeJson(d,Serial);Serial.write('\n');}
void reply(uint32_t id,const String&request,bool ok,JsonDocument*body=nullptr,const String&message=""){
  StaticJsonDocument<128> local;JsonDocument&d=body?*body:static_cast<JsonDocument&>(local);String type=request+(ok?"_OK":"_ERR");if(!ok)d["message"]=message;sendResponse(id,type,d);
}
bool requireSession(uint32_t id,const String&cmd){if(mode==PC_SESSION&&serverInstance&&serverInstance->isActive())return true;reply(id,cmd,false,nullptr,"Begin a PC session first.");return false;}

void startFastUpload(uint32_t id, const String &cmd, const String &path,
                     uint64_t offset, uint16_t requestedPayload, uint8_t requestedWindow);
void startFastDownload(uint32_t id, const String &cmd, const String &path,
                       uint64_t offset, uint16_t requestedPayload, uint8_t requestedWindow,
                       uint32_t expectedCrc);
void handleFastEncodedFrame(const uint8_t *encoded, size_t encodedLength);
void sendFastDownloadWindow();

#include "ESP32PcSessionVolumeHandler.inc"

uint16_t fastRead16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
uint32_t fastRead32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint64_t fastRead64(const uint8_t *p) { return (uint64_t)fastRead32(p) | ((uint64_t)fastRead32(p + 4) << 32); }
void fastWrite16(uint8_t *p, uint16_t value) { p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); }
void fastWrite32(uint8_t *p, uint32_t value) { for (uint8_t i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i)); }
void fastWrite64(uint8_t *p, uint64_t value) { fastWrite32(p, (uint32_t)value); fastWrite32(p + 4, (uint32_t)(value >> 32)); }

bool fastCobsDecode(const uint8_t *input, size_t inputLength, uint8_t *output, size_t outputCapacity, size_t &outputLength) {
  outputLength = 0;
  size_t read = 0;
  while (read < inputLength) {
    const uint8_t code = input[read++];
    if (!code) return false;
    const size_t count = code - 1;
    if (read + count > inputLength || outputLength + count + ((code != 0xFF && read + count < inputLength) ? 1 : 0) > outputCapacity) return false;
    memcpy(output + outputLength, input + read, count);
    read += count;
    outputLength += count;
    if (code != 0xFF && read < inputLength) output[outputLength++] = 0;
  }
  return true;
}

void sendFastFrame(uint8_t type, uint32_t sequence, uint64_t offset, const uint8_t *payload = nullptr, size_t payloadLength = 0) {
  if (payloadLength > fastPayloadSize || payloadLength > FAST_MAX_PAYLOAD) return;
  uint8_t raw[FAST_HEADER_SIZE + FAST_MAX_PAYLOAD + 4];
  raw[0] = FAST_PROTOCOL_VERSION; raw[1] = type; raw[2] = 0; raw[3] = FAST_HEADER_SIZE;
  fastWrite32(raw + 4, fastTransferId); fastWrite32(raw + 8, sequence); fastWrite64(raw + 12, offset);
  fastWrite16(raw + 20, (uint16_t)payloadLength);
  if (payloadLength) memcpy(raw + FAST_HEADER_SIZE, payload, payloadLength);
  fastWrite32(raw + FAST_HEADER_SIZE + payloadLength,
              crcUpdate(0xFFFFFFFFUL, raw, FAST_HEADER_SIZE + payloadLength) ^ 0xFFFFFFFFUL);

  uint8_t encoded[FAST_MAX_ENCODED];
  size_t write = 1, codeIndex = 0;
  uint8_t code = 1;
  const size_t rawLength = FAST_HEADER_SIZE + payloadLength + 4;
  for (size_t read = 0; read < rawLength; ++read) {
    const uint8_t value = raw[read];
    if (!value) { encoded[codeIndex] = code; codeIndex = write++; code = 1; }
    else { encoded[write++] = value; if (++code == 0xFF) { encoded[codeIndex] = code; codeIndex = write++; code = 1; } }
  }
  encoded[codeIndex] = code;
  Serial.write(encoded, write);
  Serial.write((uint8_t)0);
  fastLastActivity = millis();
}

void fastSetBinaryMode(FastDirection direction, uint32_t transferId, uint16_t payload, uint8_t window) {
  fastDirection = direction;
  fastTransferId = transferId;
  fastPayloadSize = payload;
  fastWindow = window;
  fastSequence = 0;
  fastOffset = 0;
  fastFramesInWindow = 0;
  fastSendSequence = 0;
  fastSendOffset = 0;
  fastWindowEndSequence = 0;
  fastWindowEndOffset = 0;
  fastWaitingForAck = false;
  fastWaitingForFinishAck = false;
  fastNakOutstanding = false;
  fastDuplicateAckSent = false;
  fastEncodedLength = 0;
  fastJsonProbeActive = false;
  fastJsonPrefixMatched = false;
  fastJsonProbeStart = 0;
  fastJsonProbePosition = 0;
  fastLastActivity = millis();
  binaryMode = true;
}

void startFastUpload(uint32_t id, const String &cmd, const String &path,
                     uint64_t offset, uint16_t requestedPayload, uint8_t requestedWindow) {
  if (!uploadActive || path != uploadPath || !uploadFilesystem || offset != uploadReceived) {
    reply(id, cmd, false, nullptr, "No matching staged upload or offset.");
    return;
  }
  const uint16_t payload = min((uint16_t)FAST_MAX_PAYLOAD, requestedPayload ? requestedPayload : (uint16_t)FAST_MAX_PAYLOAD);
  const uint8_t window = min((uint8_t)FAST_MAX_WINDOW, requestedWindow ? requestedWindow : (uint8_t)FAST_MAX_WINDOW);
  const uint32_t transferId = nextFastTransferId++;
  fastSize = uploadSize;
  fastFileCrc = uploadExpectedCrc;
  fastSetBinaryMode(FAST_UPLOAD, transferId, payload, window);
  fastOffset = offset;
  fastSequence = (uint32_t)(offset / payload);
  StaticJsonDocument<128> result;
  result["transferId"] = transferId; result["offset"] = offset;
  result["chunkSize"] = payload; result["window"] = window;
  reply(id, cmd, true, &result);
}

void startFastDownload(uint32_t id, const String &cmd, const String &path,
                       uint64_t offset, uint16_t requestedPayload, uint8_t requestedWindow,
                       uint32_t expectedCrc) {
  if (isVolumeRoot(path) || offset > 0) {
    reply(id, cmd, false, nullptr, "Fast download must start at the beginning of a file.");
    return;
  }
  fs::FS &fs = filesystemFor(path);
  const String local = filesystemPath(path);
  File file = fs.open(local, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    reply(id, cmd, false, nullptr, "File not found for binary download.");
    return;
  }
  fastSize = file.size();
  fastFileCrc = expectedCrc;
  const uint16_t payload = min((uint16_t)FAST_MAX_PAYLOAD, requestedPayload ? requestedPayload : (uint16_t)FAST_MAX_PAYLOAD);
  const uint8_t window = min((uint8_t)FAST_MAX_WINDOW, requestedWindow ? requestedWindow : (uint8_t)FAST_MAX_WINDOW);
  const uint32_t transferId = nextFastTransferId++;
  fastDownloadFile = file;
  fastSetBinaryMode(FAST_DOWNLOAD, transferId, payload, window);
  StaticJsonDocument<192> result;
  result["transferId"] = transferId; result["offset"] = 0;
  result["size"] = fastSize; result["crc32"] = hex32(expectedCrc);
  result["chunkSize"] = payload; result["window"] = window;
  reply(id, cmd, true, &result);
  sendFastDownloadWindow();
}

void sendFastDownloadWindow() {
  if (fastDirection != FAST_DOWNLOAD || !fastDownloadFile || fastWaitingForAck || fastWaitingForFinishAck) return;
  if (fastSendOffset >= fastSize) {
    uint8_t crc[4]; fastWrite32(crc, fastFileCrc);
    sendFastFrame(FAST_FRAME_FINISH, fastSendSequence, fastSize, crc, sizeof(crc));
    fastWaitingForFinishAck = true;
    return;
  }
  uint8_t payload[FAST_MAX_PAYLOAD];
  uint8_t sent = 0;
  while (sent < fastWindow && fastSendOffset < fastSize) {
    const size_t wanted = (size_t)min((uint64_t)fastPayloadSize, fastSize - fastSendOffset);
    if (!fastDownloadFile.seek((uint32_t)fastSendOffset)) break;
    const size_t got = fastDownloadFile.read(payload, wanted);
    if (got != wanted) { sendFastFrame(FAST_FRAME_ERROR, fastSendSequence, fastSendOffset); return; }
    sendFastFrame(FAST_FRAME_DATA, fastSendSequence, fastSendOffset, payload, got);
    fastSendOffset += got;
    fastSendSequence++;
    sent++;
  }
  fastWindowEndOffset = fastSendOffset;
  fastWindowEndSequence = fastSendSequence;
  fastWaitingForAck = sent > 0;
}

void commitFastUpload() {
  if (!uploadActive || !uploadFilesystem) { sendFastFrame(FAST_FRAME_ERROR, fastSequence, fastOffset); return; }
  uploadFile.flush();
  uploadFile.close();
  uploadActive = false;
  fs::FS &fs = *uploadFilesystem;
  const String path = uploadPath;
  const String destination = filesystemPath(path);
  File staged = fs.open(uploadStagePath, FILE_READ);
  const bool verified = staged && staged.size() == uploadSize && crcFile(staged) == uploadExpectedCrc;
  if (staged) staged.close();
  if (!verified) {
    fs.remove(uploadStagePath); fs.remove(pcResumeMetaPath);
    uploadFilesystem = nullptr; uploadPath = ""; uploadStagePath = "";
    sendFastFrame(FAST_FRAME_ERROR, fastSequence, fastOffset);
    return;
  }
  File existing = fs.open(destination, FILE_READ);
  if (existing && existing.isDirectory()) {
    existing.close(); fs.remove(uploadStagePath); fs.remove(pcResumeMetaPath);
    uploadFilesystem = nullptr; uploadPath = ""; uploadStagePath = "";
    sendFastFrame(FAST_FRAME_ERROR, fastSequence, fastOffset);
    return;
  }
  if (existing) existing.close();
  const String backupPath = "/.pc-tmp/previous.bin";
  const bool hadPrevious = fs.exists(destination);
  if (hadPrevious) {
    fs.remove(backupPath);
    if (!fs.rename(destination, backupPath)) { sendFastFrame(FAST_FRAME_ERROR, fastSequence, fastOffset); return; }
  }
  if (!fs.rename(uploadStagePath, destination)) {
    if (hadPrevious) fs.rename(backupPath, destination);
    sendFastFrame(FAST_FRAME_ERROR, fastSequence, fastOffset);
    return;
  }
  if (hadPrevious) fs.remove(backupPath);
  fs.remove(pcResumeMetaPath);
  uploadFilesystem = nullptr; uploadPath = ""; uploadStagePath = "";
  sendFastFrame(FAST_FRAME_FINISH_ACK, fastSequence, fastSize);
  fastDirection = FAST_NONE; binaryMode = false; fastEncodedLength = 0;
}

void handleFastEncodedFrame(const uint8_t *encoded, size_t encodedLength) {
  uint8_t raw[FAST_HEADER_SIZE + FAST_MAX_PAYLOAD + 4];
  size_t rawLength = 0;
  if (!fastCobsDecode(encoded, encodedLength, raw, sizeof(raw), rawLength) || rawLength < FAST_HEADER_SIZE + 4) {
    if (fastDirection == FAST_UPLOAD && !fastNakOutstanding) { fastFramesInWindow = 0; sendFastFrame(FAST_FRAME_NAK, fastSequence, fastOffset); fastNakOutstanding = true; }
    return;
  }
  const uint16_t payloadLength = fastRead16(raw + 20);
  if (raw[0] != FAST_PROTOCOL_VERSION || raw[2] != 0 || raw[3] != FAST_HEADER_SIZE ||
      payloadLength > fastPayloadSize || rawLength != FAST_HEADER_SIZE + payloadLength + 4 ||
      fastRead32(raw + FAST_HEADER_SIZE + payloadLength) != (crcUpdate(0xFFFFFFFFUL, raw, FAST_HEADER_SIZE + payloadLength) ^ 0xFFFFFFFFUL) ||
      fastRead32(raw + 4) != fastTransferId) {
    if (fastDirection == FAST_UPLOAD && !fastNakOutstanding) { fastFramesInWindow = 0; sendFastFrame(FAST_FRAME_NAK, fastSequence, fastOffset); fastNakOutstanding = true; }
    return;
  }
  fastLastActivity = millis();
  const uint8_t type = raw[1];
  const uint32_t sequence = fastRead32(raw + 8);
  const uint64_t offset = fastRead64(raw + 12);
  const uint8_t *payload = raw + FAST_HEADER_SIZE;

  if (type == FAST_FRAME_ABORT) {
    if (fastDirection == FAST_UPLOAD) pcCloseUploadForResume();
    if (fastDownloadFile) fastDownloadFile.close();
    clearDownloadTransfer();
    fastDirection = FAST_NONE; binaryMode = false; fastEncodedLength = 0;
    return;
  }

  if (fastDirection == FAST_UPLOAD && type == FAST_FRAME_DATA) {
    if (sequence < fastSequence && offset < fastOffset) {
      if (!fastDuplicateAckSent) { sendFastFrame(FAST_FRAME_ACK, fastSequence, fastOffset); fastDuplicateAckSent = true; }
      return;
    }
    if (sequence != fastSequence || offset != fastOffset || !payloadLength || fastOffset + payloadLength > fastSize ||
        uploadFile.write(payload, payloadLength) != payloadLength) {
      if (!fastNakOutstanding) { fastFramesInWindow = 0; sendFastFrame(FAST_FRAME_NAK, fastSequence, fastOffset); fastNakOutstanding = true; }
      return;
    }
    fastNakOutstanding = false; fastDuplicateAckSent = false;
    fastOffset += payloadLength; uploadReceived = fastOffset; fastSequence++; fastFramesInWindow++;
    if (fastFramesInWindow >= fastWindow || fastOffset == fastSize) {
      uploadFile.flush();
      sendFastFrame(FAST_FRAME_ACK, fastSequence, fastOffset);
      fastFramesInWindow = 0;
    }
    return;
  }

  if (fastDirection == FAST_UPLOAD && type == FAST_FRAME_FINISH) {
    if (sequence != fastSequence || offset != fastSize || fastOffset != fastSize || payloadLength != 4 || fastRead32(payload) != fastFileCrc) {
      sendFastFrame(FAST_FRAME_ERROR, fastSequence, fastOffset);
      return;
    }
    commitFastUpload();
    return;
  }

  if (fastDirection == FAST_DOWNLOAD && (type == FAST_FRAME_ACK || type == FAST_FRAME_NAK)) {
    if (offset > fastSize || sequence > fastWindowEndSequence || offset > fastWindowEndOffset) {
      sendFastFrame(FAST_FRAME_ERROR, fastSendSequence, fastSendOffset);
      return;
    }
    if (type == FAST_FRAME_NAK || sequence != fastWindowEndSequence || offset != fastWindowEndOffset) {
      if (offset > fastSize || !fastDownloadFile.seek((uint32_t)offset)) return;
      fastSendSequence = sequence; fastSendOffset = offset;
      fastWaitingForAck = false; fastWaitingForFinishAck = false;
      sendFastDownloadWindow();
      return;
    }
    fastWaitingForAck = false;
    sendFastDownloadWindow();
    return;
  }

  if (fastDirection == FAST_DOWNLOAD && type == FAST_FRAME_FINISH_ACK && fastWaitingForFinishAck && offset == fastSize) {
    if (fastDownloadFile) fastDownloadFile.close();
    clearDownloadTransfer();
    fastDirection = FAST_NONE; binaryMode = false; fastEncodedLength = 0;
    return;
  }
  sendFastFrame(FAST_FRAME_ERROR, fastSequence, fastOffset);
}

void handle(JsonDocument&q){
  uint32_t id=q["id"]|0;String cmd=q["type"]|"";String path=q["path"]|"";
  if((q["v"]|0)!=1){reply(id,cmd,false,nullptr,"Unsupported protocol version.");return;}
  if(cmd=="HELLO"){StaticJsonDocument<384>d;d["device"]="ESP32-WROOM";if(serverInstance&&serverInstance->deviceName().length())d["name"]=serverInstance->deviceName();d["build"]="serial-file-manager-v2";d["state"]=mode==READY?"READY":mode==PC_SESSION?"PC_SESSION":"RECOVERY";d["littlefsReady"]=StorageLibrary.isLittleFSReady();d["baud"]=pendingSerialBaud?pendingSerialBaud:activeSerialBaud;d["baudPending"]=pendingSerialBaud!=0;JsonArray rates=d["baudRates"].to<JsonArray>();for(uint32_t supported:SUPPORTED_BAUD_RATES)rates.add(supported);reply(id,cmd,true,&d);return;}
  if(cmd=="SET_BAUD"){
    uint32_t baud=q["baud"]|0;
    if(!isSupportedBaud(baud)){reply(id,cmd,false,nullptr,"Unsupported serial baud rate.");return;}
    if(pendingSerialBaud){reply(id,cmd,false,nullptr,"A baud-rate change is already pending.");return;}
    StaticJsonDocument<96>d;d["baud"]=baud;d["switchDelayMs"]=BAUD_SWITCH_DELAY_MS;d["fallbackDelayMs"]=BAUD_FALLBACK_DELAY_MS;reply(id,cmd,true,&d);
    Serial.flush();delay(BAUD_SWITCH_DELAY_MS);Serial.updateBaudRate(baud);pendingSerialBaud=baud;pendingBaudDeadline=millis()+BAUD_FALLBACK_DELAY_MS;return;
  }
  if(cmd=="BAUD_CONFIRM"){
    uint32_t baud=q["baud"]|0;
    if(pendingSerialBaud==baud){activeSerialBaud=baud;pendingSerialBaud=0;pendingBaudDeadline=0;reply(id,cmd,true);return;}
    if(!pendingSerialBaud&&activeSerialBaud==baud){reply(id,cmd,true);return;}
    reply(id,cmd,false,nullptr,"Baud-rate confirmation does not match the pending rate.");return;
  }
  if(cmd=="SESSION_BEGIN"){
    if(mode!=READY||!serverInstance){reply(id,cmd,false,nullptr,"Board is not ready; check SD and LittleFS status.");return;}
    if(!serverInstance->begin()){reply(id,cmd,false,nullptr,serverInstance->lastError());return;}
    mode=PC_SESSION;reply(id,cmd,true);return;
  }
  if(cmd=="SESSION_END"){
    if((mode!=PC_SESSION&&mode!=RECOVERY)||!serverInstance){reply(id,cmd,false,nullptr,"No active PC session.");return;}
    if(uploadActive){uploadFile.close();uploadActive=false;}
    if(!serverInstance->end()){mode=RECOVERY;reply(id,cmd,false,nullptr,serverInstance->lastError());return;}
    mode=READY;StaticJsonDocument<64>d;d["queueSynced"]=true;reply(id,cmd,true,&d);if(activeSerialBaud!=defaultSerialBaud)restoreDefaultSerialBaud();return;
  }
  if(!requireSession(id,cmd))return;
  if(!validPath(path)){reply(id,cmd,false,nullptr,"Invalid or protected SD path.");return;}
  String p=cardPath(path);
  if(cmd=="LIST"){
    File dir=sdCard().open(p);if(!dir||!dir.isDirectory()){reply(id,cmd,false,nullptr,"Folder not found.");return;}
    StaticJsonDocument<3600>d;JsonArray arr=d["entries"].to<JsonArray>();uint32_t skip=q["offset"]|0;File f=dir.openNextFile();uint32_t seen=0;
    while(f&&seen<skip){f.close();f=dir.openNextFile();seen++;}
    uint8_t count=0;while(f&&count<24){String name=String(f.name());int slash=name.lastIndexOf('/');if(slash>=0)name=name.substring(slash+1);if(name!=".pc-tmp"){JsonObject o=arr.add<JsonObject>();o["name"]=name;o["dir"]=f.isDirectory();o["size"]=f.isDirectory()?0:f.size();time_t modified=f.getLastWrite();if(modified>=315532800)o["modified"]=(int64_t)modified;count++;}f.close();f=dir.openNextFile();seen++;}
    d["more"]=(bool)f;d["next"]=seen;if(f)f.close();dir.close();reply(id,cmd,true,&d);return;
  }
  if(cmd=="MKDIR"){if(path=="/sd"||sdCard().exists(p)||!makeParents(p.substring(0,p.lastIndexOf('/')))||!sdCard().mkdir(p)){reply(id,cmd,false,nullptr,"Could not create folder.");return;}reply(id,cmd,true);return;}
  if(cmd=="DELETE"){if(path=="/sd"){reply(id,cmd,false,nullptr,"Cannot delete the SD root.");return;}if(!sdCard().exists(p)){reply(id,cmd,false,nullptr,"Path not found.");return;}File f=sdCard().open(p);bool isDir=f&&f.isDirectory();f.close();bool ok=isDir?StorageLibrary.SD.deleteFolder(p):StorageLibrary.SD.deleteFile(p);if(!ok){reply(id,cmd,false,nullptr,StorageLibrary.SD.lastError());return;}reply(id,cmd,true);return;}
  if(cmd=="RENAME"||cmd=="MOVE"||cmd=="COPY"){
    String destination=q["destination"]|"";
    if(!validPath(destination)||destination=="/sd"){
      reply(id,cmd,false,nullptr,"Invalid or protected destination path.");return;
    }
    if(cmd=="RENAME"){
      int sourceSlash=path.lastIndexOf('/');
      int destinationSlash=destination.lastIndexOf('/');
      if(sourceSlash<0||destinationSlash<0||path.substring(0,sourceSlash)!=destination.substring(0,destinationSlash)){
        reply(id,cmd,false,nullptr,"RENAME must keep the item in its current folder; use MOVE to change folders.");return;
      }
    }
    String destinationCardPath=cardPath(destination);
    int slash=destinationCardPath.lastIndexOf('/');
    if(slash>0&&!makeParents(destinationCardPath.substring(0,slash))){
      reply(id,cmd,false,nullptr,"Could not create destination folder.");return;
    }
    bool ok=cmd=="COPY"
      ? StorageLibrary.SD.copyPath(p,destinationCardPath)
      : cmd=="MOVE"
        ? StorageLibrary.SD.movePath(p,destinationCardPath)
        : StorageLibrary.SD.renamePath(p,destinationCardPath);
    if(!ok){reply(id,cmd,false,nullptr,StorageLibrary.SD.lastError());return;}
    reply(id,cmd,true);return;
  }
  if(cmd=="GET_BEGIN"){File f=sdCard().open(p,"r");if(!f||f.isDirectory()){reply(id,cmd,false,nullptr,"File not found.");return;}uint64_t size=f.size();uint32_t crc=crcFile(f);f.close();StaticJsonDocument<96>d;d["size"]=size;d["crc32"]=hex32(crc);reply(id,cmd,true,&d);return;}
  if(cmd=="GET_CHUNK"){uint64_t off=q["offset"]|UINT64_MAX;size_t n=q["length"]|0;if(n==0||n>CHUNK||off==UINT64_MAX){reply(id,cmd,false,nullptr,"Invalid chunk request.");return;}File f=sdCard().open(p,"r");if(!f||f.isDirectory()||off>f.size()){reply(id,cmd,false,nullptr,"File or offset is invalid.");return;}f.seek((uint32_t)off);uint8_t b[CHUNK];size_t got=f.read(b,n);f.close();if(got!=n){reply(id,cmd,false,nullptr,"Short read.");return;}StaticJsonDocument<1100>d;char enc[CHUNK*2+8];size_t olen=0;if(mbedtls_base64_encode((unsigned char*)enc,sizeof(enc),&olen,b,got)!=0){reply(id,cmd,false,nullptr,"Base64 encoding failed.");return;}enc[olen]=0;d["offset"]=off;d["data"]=enc;d["crc32"]=hex32(crcUpdate(0xFFFFFFFFUL,b,got)^0xFFFFFFFFUL);reply(id,cmd,true,&d);return;}
  if(cmd=="PUT_BEGIN"){if(uploadActive){reply(id,cmd,false,nullptr,"Another upload is active.");return;}uint64_t size=q["size"]|UINT64_MAX;uint32_t crc;if(size==UINT64_MAX||size>1024ULL*1024*1024||!parseCrc(q["crc32"],crc)){reply(id,cmd,false,nullptr,"Invalid size or CRC.");return;}makeParents(p.substring(0,p.lastIndexOf('/')));if(!sdCard().exists("/.pc-tmp"))sdCard().mkdir("/.pc-tmp");sdCard().remove("/.pc-tmp/upload.bin");uploadPath=path;uploadSize=size;uploadExpectedCrc=crc;uploadReceived=0;uploadFile=sdCard().open("/.pc-tmp/upload.bin",FILE_WRITE);if(!uploadFile){reply(id,cmd,false,nullptr,"Could not create staging file.");return;}uploadActive=true;reply(id,cmd,true);return;}
  if(cmd=="PUT_CHUNK"){if(!uploadActive||path!=uploadPath){reply(id,cmd,false,nullptr,"No matching upload.");return;}uint64_t off=q["offset"]|UINT64_MAX;const char*encoded=q["data"]|"";uint32_t expected;if(off!=uploadReceived||!parseCrc(q["crc32"],expected)){reply(id,cmd,false,nullptr,"Unexpected offset or invalid CRC.");return;}uint8_t b[CHUNK];size_t n=0,badIndex=0;uint8_t badByte=0;size_t encodedLen=strlen(encoded);if(!decodeBase64(encoded,encodedLen,b,sizeof(b),n,badIndex,badByte)){char message[128];snprintf(message,sizeof(message),"Invalid base64 at offset %lu (length %u, index %u, byte 0x%02X).",(unsigned long)off,(unsigned)encodedLen,(unsigned)badIndex,(unsigned)badByte);reply(id,cmd,false,nullptr,String(message));return;}uint32_t actual=crcUpdate(0xFFFFFFFFUL,b,n)^0xFFFFFFFFUL;if(!n||n>CHUNK||actual!=expected||uploadReceived+n>uploadSize){reply(id,cmd,false,nullptr,"Chunk CRC or length is invalid.");return;}if(uploadFile.write(b,n)!=n){reply(id,cmd,false,nullptr,"Staging write failed.");return;}uploadReceived+=n;reply(id,cmd,true);return;}
  if(cmd=="PUT_COMMIT"){if(!uploadActive||path!=uploadPath){reply(id,cmd,false,nullptr,"No matching upload.");return;}uploadFile.flush();uploadFile.close();uploadActive=false;File f=sdCard().open("/.pc-tmp/upload.bin","r");if(!f||f.size()!=uploadSize||crcFile(f)!=uploadExpectedCrc){if(f)f.close();sdCard().remove("/.pc-tmp/upload.bin");reply(id,cmd,false,nullptr,"Whole-file verification failed.");return;}f.close();String backup="/.pc-tmp/previous.bin";bool had=sdCard().exists(p);if(had){sdCard().remove(backup);if(!sdCard().rename(p,backup)){reply(id,cmd,false,nullptr,"Could not preserve prior destination.");return;}}if(!sdCard().rename("/.pc-tmp/upload.bin",p)){if(had)sdCard().rename(backup,p);reply(id,cmd,false,nullptr,"Could not commit upload.");return;}if(had)sdCard().remove(backup);reply(id,cmd,true);return;}
  if(cmd=="PUT_ABORT"){if(uploadActive){uploadFile.close();uploadActive=false;}sdCard().remove("/.pc-tmp/upload.bin");reply(id,cmd,true);return;}
  reply(id,cmd,false,nullptr,"Unknown command.");
}
} // namespace

bool ESP32PcSession::beginServer(uint8_t sdCsPin,uint32_t baud,SPIClass *sdSPI){
  serverInstance=this;
  defaultSerialBaud=baud;activeSerialBaud=baud;pendingSerialBaud=0;pendingBaudDeadline=0;
  Serial.setRxBufferSize(8192);Serial.begin(baud);delay(300);
  bool mounted=StorageLibrary.beginStorage(sdCsPin,18,19,23,4000000,false,sdSPI);
  const bool transferPending = pcTransferHoldActive();
  StorageLibrary.deferQueueReplay(transferPending);
  bool queueReady = transferPending || !StorageLibrary.isSDReady() || StorageLibrary.syncBackup();
  mode=(mounted&&StorageLibrary.isLittleFSReady()&&queueReady)?READY:RECOVERY;
  StaticJsonDocument<128>g;g["v"]=1;g["type"]="BOOT";g["state"]=mode==READY?"READY":"RECOVERY";serializeJson(g,Serial);Serial.write('\n');
  return mode==READY;
}

bool ESP32PcSession::isTransferActive() const {
  return uploadActive || downloadActive || pcTransferHoldActive();
}

void ESP32PcSession::setDeviceName(const String &name){
  _deviceName=name;
  _deviceName.trim();
  if(_deviceName.length()>64)_deviceName.remove(64);
}

void ESP32PcSession::poll(){
  expireDownloadTransfer();
  if(pendingSerialBaud&&(int32_t)(millis()-pendingBaudDeadline)>=0){Serial.updateBaudRate(defaultSerialBaud);activeSerialBaud=defaultSerialBaud;pendingSerialBaud=0;pendingBaudDeadline=0;serialLineLength=0;serialLineOverflow=false;while(Serial.available())Serial.read();}
  if(binaryMode&&(uint32_t)(millis()-fastLastActivity)>=FAST_IDLE_TIMEOUT_MS){
    if(fastDirection==FAST_UPLOAD)pcCloseUploadForResume();
    if(fastDownloadFile)fastDownloadFile.close();
    clearDownloadTransfer();
    fastDirection=FAST_NONE;binaryMode=false;fastEncodedLength=0;
  }
  while(Serial.available()){
    const uint8_t c=(uint8_t)Serial.read();
    if(binaryMode){
      if(c==0){if(fastEncodedLength)handleFastEncodedFrame(fastEncoded,fastEncodedLength);fastEncodedLength=0;fastJsonProbeActive=false;fastJsonPrefixMatched=false;fastJsonProbePosition=0;}
      else if(fastEncodedLength<sizeof(fastEncoded)){
        fastEncoded[fastEncodedLength++]=c;
        static const char helloPrefix[]="{\"v\":1,\"id\":";
        if(!fastJsonProbeActive&&c=='{'){
          fastJsonProbeActive=true;fastJsonPrefixMatched=false;fastJsonProbeStart=fastEncodedLength-1;fastJsonProbePosition=1;
        }else if(fastJsonProbeActive&&!fastJsonPrefixMatched){
          if(fastJsonProbePosition<sizeof(helloPrefix)-1&&c==(uint8_t)helloPrefix[fastJsonProbePosition])fastJsonProbePosition++;
          else fastJsonProbeActive=false;
          if(fastJsonProbePosition==sizeof(helloPrefix)-1)fastJsonPrefixMatched=true;
        }
        if(fastJsonProbeActive&&fastJsonPrefixMatched&&c=='\n'){
          fastEncoded[fastEncodedLength]='\0';
          const char *candidate=(const char*)fastEncoded+fastJsonProbeStart;
          if(strstr(candidate,"\"type\":\"HELLO\"")!=nullptr){
            DynamicJsonDocument q(512);auto err=deserializeJson(q,candidate);
            if(!err){
              if(fastDirection==FAST_UPLOAD)pcCloseUploadForResume();
              if(fastDownloadFile)fastDownloadFile.close();
              clearDownloadTransfer();
              fastDirection=FAST_NONE;binaryMode=false;fastEncodedLength=0;fastJsonProbeActive=false;fastJsonPrefixMatched=false;
              handleVolumes(q);continue;
            }
          }
          fastJsonProbeActive=false;fastJsonPrefixMatched=false;
        }
      }
      else fastEncodedLength=0;
      continue;
    }
    if(c=='\n'){
      if(serialLineOverflow){
        StaticJsonDocument<96>d;d["v"]=1;d["id"]=0;d["type"]="PROTOCOL_ERR";d["message"]="Serial request line too long";serializeJson(d,Serial);Serial.write('\n');
      }else if(serialLineLength){
        serialLine[serialLineLength]='\0';
        DynamicJsonDocument q(8192);auto err=deserializeJson(q,serialLine);
        if(!err)handleVolumes(q);else{StaticJsonDocument<96>d;d["v"]=1;d["id"]=0;d["type"]="PROTOCOL_ERR";d["message"]="Malformed JSON";serializeJson(d,Serial);Serial.write('\n');}
      }
      serialLineLength=0;
      serialLineOverflow=false;
    }else if(!serialLineOverflow){
      if(serialLineLength<MAX_LINE)serialLine[serialLineLength++]=c;
      else serialLineOverflow=true;
    }
  }
  if(!binaryMode&&!pcTransferHoldActive()&&serverInstance==this&&_sensorDataPendingDeferred){
    _sensorDataPendingDeferred=false;
    notifySensorDataPending();
  }
  if(!binaryMode&&mode!=PC_SESSION&&serverInstance==this&&_sensorDataSavedDeferred){
    _sensorDataSavedDeferred=false;
    notifySensorDataSaved();
  }
  if(mode==READY&&!_active&&millis()-lastQueueSync>30000){sync();lastQueueSync=millis();}
  delay(1);
}
bool ESP32PcSession::begin(){
  if(_active)return true;
  if(!StorageLibrary.isLittleFSReady()){_lastError="LittleFS is not ready; safe PC sessions require the fallback queue.";return false;}
  // Keep queued sensor records in LittleFS for the entire PC session. Replaying
  // them to SD here would block serial transfer commands before they can start.
  if(!StorageLibrary.beginHostSession(pcTransferHoldActive())){_lastError=StorageLibrary.lastError();if(!_lastError.length())_lastError="StorageLibrary could not start a host session.";return false;}
  _active=true;_needsRecovery=false;_lastError="";return true;
}

bool ESP32PcSession::suspendForResume(){if(!_active)return true;_active=false;_lastError="";return true;}
bool ESP32PcSession::end(){if(pcTransferHoldActive())return suspendForResume();
  if(_active){_active=false;if(StorageLibrary.endHostSession()){_needsRecovery=false;_lastError="";return true;}_needsRecovery=true;_lastError=StorageLibrary.lastError();if(!_lastError.length())_lastError="Could not release the StorageLibrary host session.";return false;}
  if(!_needsRecovery)return StorageLibrary.syncBackup();
  if(StorageLibrary.syncBackup()){_needsRecovery=false;_lastError="";return true;}
  _lastError=StorageLibrary.lastError();if(!_lastError.length())_lastError="Queued-record sync is still pending.";return false;
}

bool ESP32PcSession::writeRecord(const String&record){if(!StorageLibrary.writeWithFallback(record)){_lastError=StorageLibrary.lastError();return false;}_lastError="";return true;}
bool ESP32PcSession::sync(){if(_active){_lastError="Cannot sync while the PC owns the storage.";return false;}if(!StorageLibrary.isSDReady()){StorageLibrary.syncBackup();_lastError="SD card unavailable; queued sensor records remain safely in LittleFS.";_needsRecovery=false;return true;}bool ok=StorageLibrary.syncBackup();_needsRecovery=!ok;_lastError=ok?"":StorageLibrary.lastError();return ok;}

void ESP32PcSession::notifySensorDataPending(){if(serverInstance!=this||!_active||_sensorDataNotificationPending)return;if(binaryMode){_sensorDataPendingDeferred=true;return;}sendNotification("SENSOR_DATA_PENDING");_sensorDataNotificationPending=true;}
void ESP32PcSession::notifySensorDataSaved(){if(serverInstance!=this||(!_sensorDataNotificationPending&&!_sensorDataPendingDeferred))return;if(binaryMode){_sensorDataSavedDeferred=true;return;}sendNotification("SENSOR_DATA_SAVED");_sensorDataNotificationPending=false;_sensorDataPendingDeferred=false;_sensorDataSavedDeferred=false;}
