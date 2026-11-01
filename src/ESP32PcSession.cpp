#include <ESP32PcSession.h>

namespace {
enum Mode { RECOVERY, READY, PC_SESSION };
Mode mode = RECOVERY;
ESP32PcSession *serverInstance = nullptr;
File uploadFile;
String uploadPath;
uint64_t uploadSize = 0, uploadReceived = 0;
uint32_t uploadExpectedCrc = 0;
bool uploadActive = false;
uint32_t lastQueueSync = 0;
String serialLine;
static constexpr size_t MAX_LINE = 4095;
static constexpr size_t CHUNK = 480;

uint32_t crcUpdate(uint32_t c,const uint8_t *p,size_t n) {
  while(n--){c^=*p++;for(uint8_t i=0;i<8;i++)c=(c&1)?(0xEDB88320UL^(c>>1)):(c>>1);}return c;
}
uint32_t crcFile(File &f){uint32_t c=0xFFFFFFFFUL;uint8_t b[512];while(f.available()){size_t n=f.read(b,sizeof(b));c=crcUpdate(c,b,n);}return c^0xFFFFFFFFUL;}
String hex32(uint32_t n){char b[9];snprintf(b,sizeof(b),"%08lx",(unsigned long)n);return String(b);}
bool parseCrc(JsonVariantConst v,uint32_t &out){if(!v.is<const char*>())return false;const char*s=v.as<const char*>();char*end=nullptr;unsigned long n=strtoul(s,&end,16);if(end==s||*end||strlen(s)!=8)return false;out=(uint32_t)n;return true;}
bool validPath(const String&p){if(p.length()<3||p.length()>240||!(p=="/sd"||p.startsWith("/sd/"))||p.indexOf("..")>=0||p.indexOf('\\')>=0)return false;for(size_t i=0;i<p.length();i++)if((uint8_t)p[i]<32)return false;return p!="/sd/.pc-tmp"&&!p.startsWith("/sd/.pc-tmp/");}
String cardPath(const String&p){String out=p.substring(3);return out.length()?out:"/";}
fs::FS &sdCard(){return StorageLibrary.SD.filesystem();}
bool makeParents(const String&p){int at=1;while((at=p.indexOf('/',at))>0){String d=p.substring(0,at);if(d!="/"&&!sdCard().exists(d))sdCard().mkdir(d);at++;}return true;}

void sendResponse(uint32_t id,const String&type,JsonDocument&d){d["v"]=1;d["id"]=id;d["type"]=type;serializeJson(d,Serial);Serial.write('\n');}
void sendNotification(const char *type){StaticJsonDocument<96>d;d["v"]=1;d["type"]=type;serializeJson(d,Serial);Serial.write('\n');}
void reply(uint32_t id,const String&request,bool ok,JsonDocument*body=nullptr,const String&message=""){
  StaticJsonDocument<128> local;JsonDocument&d=body?*body:static_cast<JsonDocument&>(local);String type=request+(ok?"_OK":"_ERR");if(!ok)d["message"]=message;sendResponse(id,type,d);
}
bool requireSession(uint32_t id,const String&cmd){if(mode==PC_SESSION&&serverInstance&&serverInstance->isActive())return true;reply(id,cmd,false,nullptr,"Begin a PC session first.");return false;}

void handle(JsonDocument&q){
  uint32_t id=q["id"]|0;String cmd=q["type"]|"";String path=q["path"]|"";
  if((q["v"]|0)!=1){reply(id,cmd,false,nullptr,"Unsupported protocol version.");return;}
  if(cmd=="HELLO"){StaticJsonDocument<160>d;d["device"]="ESP32-WROOM";d["build"]="serial-file-manager-v1";d["state"]=mode==READY?"READY":mode==PC_SESSION?"PC_SESSION":"RECOVERY";d["littlefsReady"]=StorageLibrary.isLittleFSReady();reply(id,cmd,true,&d);return;}
  if(cmd=="SESSION_BEGIN"){
    if(mode!=READY||!serverInstance){reply(id,cmd,false,nullptr,"Board is not ready; check SD and LittleFS status.");return;}
    if(!serverInstance->begin()){reply(id,cmd,false,nullptr,serverInstance->lastError());return;}
    mode=PC_SESSION;reply(id,cmd,true);return;
  }
  if(cmd=="SESSION_END"){
    if((mode!=PC_SESSION&&mode!=RECOVERY)||!serverInstance){reply(id,cmd,false,nullptr,"No active PC session.");return;}
    if(uploadActive){uploadFile.close();uploadActive=false;}
    if(!serverInstance->end()){mode=RECOVERY;reply(id,cmd,false,nullptr,serverInstance->lastError());return;}
    mode=READY;StaticJsonDocument<64>d;d["queueSynced"]=true;reply(id,cmd,true,&d);return;
  }
  if(!requireSession(id,cmd))return;
  if(!validPath(path)){reply(id,cmd,false,nullptr,"Invalid or protected SD path.");return;}
  String p=cardPath(path);
  if(cmd=="LIST"){
    File dir=sdCard().open(p);if(!dir||!dir.isDirectory()){reply(id,cmd,false,nullptr,"Folder not found.");return;}
    StaticJsonDocument<3600>d;JsonArray arr=d["entries"].to<JsonArray>();uint32_t skip=q["offset"]|0;File f=dir.openNextFile();uint32_t seen=0;
    while(f&&seen<skip){f.close();f=dir.openNextFile();seen++;}
    uint8_t count=0;while(f&&count<24){String name=String(f.name());int slash=name.lastIndexOf('/');if(slash>=0)name=name.substring(slash+1);if(name!=".pc-tmp"){JsonObject o=arr.add<JsonObject>();o["name"]=name;o["dir"]=f.isDirectory();o["size"]=f.isDirectory()?0:f.size();count++;}f.close();f=dir.openNextFile();seen++;}
    d["more"]=(bool)f;d["next"]=seen;if(f)f.close();dir.close();reply(id,cmd,true,&d);return;
  }
  if(cmd=="MKDIR"){if(path=="/sd"||sdCard().exists(p)||!makeParents(p.substring(0,p.lastIndexOf('/')))||!sdCard().mkdir(p)){reply(id,cmd,false,nullptr,"Could not create folder.");return;}reply(id,cmd,true);return;}
  if(cmd=="DELETE"){if(path=="/sd"){reply(id,cmd,false,nullptr,"Cannot delete the SD root.");return;}if(!sdCard().exists(p)){reply(id,cmd,false,nullptr,"Path not found.");return;}File f=sdCard().open(p);bool isDir=f&&f.isDirectory();f.close();bool ok=isDir?sdCard().rmdir(p):sdCard().remove(p);if(!ok){reply(id,cmd,false,nullptr,"Could not delete path (folder may not be empty).");return;}reply(id,cmd,true);return;}
  if(cmd=="GET_BEGIN"){File f=sdCard().open(p,"r");if(!f||f.isDirectory()){reply(id,cmd,false,nullptr,"File not found.");return;}uint64_t size=f.size();uint32_t crc=crcFile(f);f.close();StaticJsonDocument<96>d;d["size"]=size;d["crc32"]=hex32(crc);reply(id,cmd,true,&d);return;}
  if(cmd=="GET_CHUNK"){uint64_t off=q["offset"]|UINT64_MAX;size_t n=q["length"]|0;if(n==0||n>CHUNK||off==UINT64_MAX){reply(id,cmd,false,nullptr,"Invalid chunk request.");return;}File f=sdCard().open(p,"r");if(!f||f.isDirectory()||off>f.size()){reply(id,cmd,false,nullptr,"File or offset is invalid.");return;}f.seek((uint32_t)off);uint8_t b[CHUNK];size_t got=f.read(b,n);f.close();if(got!=n){reply(id,cmd,false,nullptr,"Short read.");return;}StaticJsonDocument<1100>d;char enc[CHUNK*2+8];size_t olen=0;if(mbedtls_base64_encode((unsigned char*)enc,sizeof(enc),&olen,b,got)!=0){reply(id,cmd,false,nullptr,"Base64 encoding failed.");return;}enc[olen]=0;d["offset"]=off;d["data"]=enc;d["crc32"]=hex32(crcUpdate(0xFFFFFFFFUL,b,got)^0xFFFFFFFFUL);reply(id,cmd,true,&d);return;}
  if(cmd=="PUT_BEGIN"){if(uploadActive){reply(id,cmd,false,nullptr,"Another upload is active.");return;}uint64_t size=q["size"]|UINT64_MAX;uint32_t crc;if(size==UINT64_MAX||size>1024ULL*1024*1024||!parseCrc(q["crc32"],crc)){reply(id,cmd,false,nullptr,"Invalid size or CRC.");return;}makeParents(p.substring(0,p.lastIndexOf('/')));if(!sdCard().exists("/.pc-tmp"))sdCard().mkdir("/.pc-tmp");sdCard().remove("/.pc-tmp/upload.bin");uploadPath=path;uploadSize=size;uploadExpectedCrc=crc;uploadReceived=0;uploadFile=sdCard().open("/.pc-tmp/upload.bin",FILE_WRITE);if(!uploadFile){reply(id,cmd,false,nullptr,"Could not create staging file.");return;}uploadActive=true;reply(id,cmd,true);return;}
  if(cmd=="PUT_CHUNK"){if(!uploadActive||path!=uploadPath){reply(id,cmd,false,nullptr,"No matching upload.");return;}uint64_t off=q["offset"]|UINT64_MAX;const char*encoded=q["data"]|"";uint32_t expected;if(off!=uploadReceived||!parseCrc(q["crc32"],expected)){reply(id,cmd,false,nullptr,"Unexpected offset or invalid CRC.");return;}size_t cap=strlen(encoded)*3/4+4;std::unique_ptr<uint8_t[]>b(new uint8_t[cap]);size_t n=0;if(mbedtls_base64_decode(b.get(),cap,&n,(const unsigned char*)encoded,strlen(encoded))!=0){reply(id,cmd,false,nullptr,"Invalid base64 payload.");return;}uint32_t actual=crcUpdate(0xFFFFFFFFUL,b.get(),n)^0xFFFFFFFFUL;if(!n||n>CHUNK||actual!=expected||uploadReceived+n>uploadSize){reply(id,cmd,false,nullptr,"Chunk CRC or length is invalid.");return;}if(uploadFile.write(b.get(),n)!=n){reply(id,cmd,false,nullptr,"Staging write failed.");return;}uploadReceived+=n;reply(id,cmd,true);return;}
  if(cmd=="PUT_COMMIT"){if(!uploadActive||path!=uploadPath){reply(id,cmd,false,nullptr,"No matching upload.");return;}uploadFile.flush();uploadFile.close();uploadActive=false;File f=sdCard().open("/.pc-tmp/upload.bin","r");if(!f||f.size()!=uploadSize||crcFile(f)!=uploadExpectedCrc){if(f)f.close();sdCard().remove("/.pc-tmp/upload.bin");reply(id,cmd,false,nullptr,"Whole-file verification failed.");return;}f.close();String backup="/.pc-tmp/previous.bin";bool had=sdCard().exists(p);if(had){sdCard().remove(backup);if(!sdCard().rename(p,backup)){reply(id,cmd,false,nullptr,"Could not preserve prior destination.");return;}}if(!sdCard().rename("/.pc-tmp/upload.bin",p)){if(had)sdCard().rename(backup,p);reply(id,cmd,false,nullptr,"Could not commit upload.");return;}if(had)sdCard().remove(backup);reply(id,cmd,true);return;}
  if(cmd=="PUT_ABORT"){if(uploadActive){uploadFile.close();uploadActive=false;}sdCard().remove("/.pc-tmp/upload.bin");reply(id,cmd,true);return;}
  reply(id,cmd,false,nullptr,"Unknown command.");
}
} // namespace

bool ESP32PcSession::beginServer(uint8_t sdCsPin,uint32_t baud,SPIClass *sdSPI){
  serverInstance=this;
  Serial.begin(baud);delay(300);
  bool mounted=StorageLibrary.beginStorage(sdCsPin,18,19,23,4000000,false,sdSPI);
  mode=(mounted&&StorageLibrary.isSDReady()&&StorageLibrary.isLittleFSReady()&&StorageLibrary.syncBackup())?READY:RECOVERY;
  StaticJsonDocument<128>g;g["v"]=1;g["type"]="BOOT";g["state"]=mode==READY?"READY":"RECOVERY";serializeJson(g,Serial);Serial.write('\n');
  return mode==READY;
}

void ESP32PcSession::poll(){
  while(Serial.available()){
    char c=(char)Serial.read();
    if(c=='\n'){
      if(serialLine.length()){DynamicJsonDocument q(8192);auto err=deserializeJson(q,serialLine);if(!err)handle(q);else{StaticJsonDocument<96>d;d["v"]=1;d["id"]=0;d["type"]="PROTOCOL_ERR";d["message"]="Malformed JSON";serializeJson(d,Serial);Serial.write('\n');}}
      serialLine="";
    }else if(serialLine.length()<MAX_LINE)serialLine+=c;else serialLine="";
  }
  if(mode==READY&&!_active&&millis()-lastQueueSync>30000){sync();lastQueueSync=millis();}
  delay(1);
}

bool ESP32PcSession::begin(){
  if(_active)return true;
  if(!StorageLibrary.isLittleFSReady()){_lastError="LittleFS is not ready; safe PC sessions require the fallback queue.";return false;}
  if(!StorageLibrary.beginHostSession()){_lastError=StorageLibrary.lastError();if(!_lastError.length())_lastError="StorageLibrary could not start a host session.";return false;}
  _active=true;_needsRecovery=false;_lastError="";return true;
}

bool ESP32PcSession::end(){
  if(_active){_active=false;if(StorageLibrary.endHostSession()){_needsRecovery=false;_lastError="";return true;}_needsRecovery=true;_lastError=StorageLibrary.lastError();if(!_lastError.length())_lastError="SD or queued-record sync failed.";return false;}
  if(!_needsRecovery)return StorageLibrary.syncBackup();
  if(StorageLibrary.syncBackup()){_needsRecovery=false;_lastError="";return true;}
  _lastError=StorageLibrary.lastError();if(!_lastError.length())_lastError="Queued-record sync is still pending.";return false;
}

bool ESP32PcSession::writeRecord(const String&record){if(!StorageLibrary.writeWithFallback(record)){_lastError=StorageLibrary.lastError();return false;}_lastError="";return true;}
bool ESP32PcSession::sync(){if(_active){_lastError="Cannot sync while the PC owns the SD card.";return false;}bool ok=StorageLibrary.syncBackup();_needsRecovery=!ok;_lastError=ok?"":StorageLibrary.lastError();return ok;}

void ESP32PcSession::notifySensorDataPending(){if(serverInstance!=this||!_active||_sensorDataNotificationPending)return;sendNotification("SENSOR_DATA_PENDING");_sensorDataNotificationPending=true;}
void ESP32PcSession::notifySensorDataSaved(){if(serverInstance!=this||!_sensorDataNotificationPending)return;sendNotification("SENSOR_DATA_SAVED");_sensorDataNotificationPending=false;}
