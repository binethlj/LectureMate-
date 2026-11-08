#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_system.h>
#include <time.h>

// =====================================================
// LECTURE MATE
// ESP32 + INMP441 + SD CARD + WIFI SETUP
// =====================================================


// =====================================================
// EXISTING LEDS
// KEEP THESE FROM YOUR ORIGINAL CODE
// =====================================================

#define GREEN_LED_PIN  4
#define RED_LED_PIN    14
#define ORANGE_LED_PIN 13


// =====================================================
// NEW WIFI HARDWARE
// =====================================================

#define WIFI_BUTTON_PIN 15
#define WIFI_RED_LED    2
#define WIFI_GREEN_LED 21


// =====================================================
// EXISTING BUTTONS
// =====================================================

#define START_STOP_BUTTON_PIN 27
#define PAUSE_BUTTON_PIN      32


// =====================================================
// SD CARD
// =====================================================

#define SD_CS   5
#define SD_SCK  18
#define SD_MISO 19
#define SD_MOSI 23


// =====================================================
// INMP441
// =====================================================

#define I2S_SCK 26
#define I2S_WS  25
#define I2S_SD  33

#define I2S_PORT I2S_NUM_0

#define SAMPLE_RATE 16000


// =====================================================
// WIFI - MULTI NETWORK + CAPTIVE PORTAL
// =====================================================

#define MAX_SAVED_WIFI 5
#define SETUP_AP_NAME "LectureMate-Setup"
#define AP_PASSWORD   "LectureMate"
#define SETUP_AP_TIMEOUT 300000UL
#define WIFI_CONNECT_TIMEOUT 20000UL
#define WIFI_BLINK_INTERVAL 350UL
#define WIFI_BUTTON_DEBOUNCE 50UL

// CLOUD DEPLOYMENT: LectureMate hosted website.
// ESP32 communicates with the live backend using HTTPS on port 443.
const char* LECTURE_MATE_BASE_URL = "https://lecture-mate.ai.studio";

// Replace this placeholder with the NEW ESP32_DEVICE_SECRET saved in AI Studio Secrets.
// Do not share the secret or commit it to a public repository.
const char* ESP32_DEVICE_SECRET = "NON DISCLOSABLE"; //CANT ADD HERE 

// HTTPS trust is handled by Arduino-ESP32 3.3.12's built-in Mozilla CA bundle.
// This avoids pinning LectureMate to only one root CA certificate.

const char* ESP32_DEVICE_ID = "lecture-mate-esp32";
const char* ESP32_DEVICE_NAME = "Lecture Mate ESP32";

#define WEBSITE_CHECK_INTERVAL 15000UL
#define WEBSITE_CHECK_TIMEOUT 5000UL
#define CHUNK_SIZE 262144UL              // Backend expects 256 KB chunks
#define UPLOAD_RESPONSE_TIMEOUT 900000UL // Allow up to 15 minutes for Gemini processing

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

struct SavedWiFi {
  String ssid;
  String password;
  String bssid;
  bool used;
};

SavedWiFi savedNetworks[MAX_SAVED_WIFI];
int activeNetworkIndex = -1;

// =====================================================
// RECORDING STATE
// =====================================================

File audioFile;
bool recording = false;
bool paused = false;
uint32_t audioDataSize = 0;
int fileNumber = 1;
String currentFileName = "";

enum WiFiState {
  WIFI_OFF_STATE,
  WIFI_CONNECTING_STATE,
  WIFI_CONNECTED_STATE,
  WIFI_FAILED_STATE,
  WIFI_SETUP_STATE
};

WiFiState wifiState = WIFI_OFF_STATE;
bool websiteConnected = false;
String websiteStatusMessage = "Backend URL not configured";

// =====================================================
// SECURE MULTI-ACCOUNT DEVICE BINDING / COMMAND QUEUE
// =====================================================
#define MAX_LINKED_ACCOUNTS 5
#define COMMAND_POLL_INTERVAL 5000UL
#define HEARTBEAT_SYNC_INTERVAL 15000UL

bool multiAccountEnabled = false;
bool bindingCapacityFull = false;
int linkedMemberCount = 1;
int maxLinkedMembers = MAX_LINKED_ACCOUNTS;
int remainingAccountSlots = MAX_LINKED_ACCOUNTS - 1;
String sharedDeviceName = "Lecture Mate ESP32";
String bindingStatus = "unknown";
String activePairingCode = "";
String pairingStatusMessage = "Connect the ESP32 to Wi-Fi, then generate a pairing code.";
bool pairingErrorState = false;
unsigned long pairingCodeIssuedAt = 0;
unsigned long pairingCodeTtlMs = 300000UL;
unsigned long lastCommandPoll = 0;
unsigned long lastBindingSync = 0;
bool queueUploadInProgress = false;
String activeQueueCommandId = "";
String activeQueueFileName = "";
int activeQueueProgress = 0;


// HTTPS certificate validation requires a real system clock. ESP32 clocks
// reset on power loss, so synchronize time after joining each Wi-Fi network.
bool lectureMateClockValid = false;
unsigned long lastClockSyncAttempt = 0;

bool ensureLectureMateClock() {
  time_t now = time(nullptr);
  if (now >= 1735689600) { // 2025-01-01; safely beyond certificate-era epoch
    lectureMateClockValid = true;
    return true;
  }

  if (lastClockSyncAttempt != 0 && millis() - lastClockSyncAttempt < 60000UL) {
    return false;
  }
  lastClockSyncAttempt = millis();

  Serial.println("Synchronizing system time before HTTPS...");
  configTime(0, 0, "time.google.com", "pool.ntp.org", "time.cloudflare.com");

  const unsigned long started = millis();
  while (millis() - started < 15000UL) {
    now = time(nullptr);
    if (now >= 1735689600) {
      lectureMateClockValid = true;
      struct tm timeInfo;
      if (getLocalTime(&timeInfo, 1000)) {
        Serial.printf("Clock synchronized: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                      timeInfo.tm_year + 1900, timeInfo.tm_mon + 1,
                      timeInfo.tm_mday, timeInfo.tm_hour,
                      timeInfo.tm_min, timeInfo.tm_sec);
      } else {
        Serial.println("Clock synchronized (epoch valid).");
      }
      return true;
    }
    delay(250);
    yield();
  }

  Serial.println("ERROR: NTP time synchronization failed; HTTPS certificate checks cannot safely run yet.");
  return false;
}

// Arduino-ESP32 3.3.12 supports the built-in CA bundle. Keep certificate
// validation enabled while trusting the full maintained root-CA bundle.
void configureLectureMateTLS(WiFiClientSecure &client, uint32_t timeoutMs) {
  client.useBuiltinCACertBundle();
  client.setTimeout(timeoutMs);
}

void printLectureMateTLSError(WiFiClientSecure &client, const char *operation) {
  char errorText[160] = {0};
  int errorCode = client.lastError(errorText, sizeof(errorText));
  Serial.printf("[TLS] %s failed. Error %d: %s\n", operation, errorCode, errorText);
}

unsigned long lastWebsiteCheck = 0;

unsigned long wifiConnectStart = 0;
unsigned long wifiBlinkTimer = 0;
unsigned long apStartTime = 0;
bool wifiBlinkState = false;
bool portalActive = false;
bool wifiButtonLastReading = HIGH;
bool wifiButtonStableState = HIGH;
unsigned long wifiButtonDebounceTime = 0;

IPAddress AP_IP(192,168,4,1);
IPAddress AP_NETMASK(255,255,255,0);

int countSDRecordings();
String jsonEscape(const String &value);
void saveErrorLED();
void saveSuccessLED();

void wifiLEDsOff() {
  digitalWrite(WIFI_RED_LED, LOW);
  digitalWrite(WIFI_GREEN_LED, LOW);
}

void wifiBlinkBoth() {
  if (millis() - wifiBlinkTimer >= WIFI_BLINK_INTERVAL) {
    wifiBlinkTimer = millis();
    wifiBlinkState = !wifiBlinkState;
    digitalWrite(WIFI_RED_LED, wifiBlinkState);
    digitalWrite(WIFI_GREEN_LED, wifiBlinkState);
  }
}

void wifiConnectedLED() {
  digitalWrite(WIFI_RED_LED, LOW);
  digitalWrite(WIFI_GREEN_LED, HIGH);
}

void wifiFailedLED() {
  digitalWrite(WIFI_RED_LED, HIGH);
  digitalWrite(WIFI_GREEN_LED, LOW);
}

String bssidToString(const uint8_t *bssid) {
  char b[18];
  sprintf(b,"%02X:%02X:%02X:%02X:%02X:%02X",
          bssid[0],bssid[1],bssid[2],
          bssid[3],bssid[4],bssid[5]);
  return String(b);
}

void clearSavedNetworks() {
  for (int i=0;i<MAX_SAVED_WIFI;i++) {
    savedNetworks[i].ssid="";
    savedNetworks[i].password="";
    savedNetworks[i].bssid="";
    savedNetworks[i].used=false;
  }
}

void saveNetwork(int i) {
  String k="n"+String(i);
  preferences.begin("wifi-list",false);
  preferences.putString((k+"_s").c_str(),savedNetworks[i].ssid);
  preferences.putString((k+"_p").c_str(),savedNetworks[i].password);
  preferences.putString((k+"_b").c_str(),savedNetworks[i].bssid);
  preferences.putBool((k+"_u").c_str(),savedNetworks[i].used);
  preferences.end();
}

void loadSavedNetworks() {
  clearSavedNetworks();
  preferences.begin("wifi-list",true);
  for (int i=0;i<MAX_SAVED_WIFI;i++) {
    String k="n"+String(i);
    savedNetworks[i].ssid=preferences.getString((k+"_s").c_str(),"");
    savedNetworks[i].password=preferences.getString((k+"_p").c_str(),"");
    savedNetworks[i].bssid=preferences.getString((k+"_b").c_str(),"");
    savedNetworks[i].used=preferences.getBool((k+"_u").c_str(),false);
  }
  preferences.end();
}

int findSavedNetwork(String ssid,String bssid) {
  for(int i=0;i<MAX_SAVED_WIFI;i++)
    if(savedNetworks[i].used &&
       savedNetworks[i].ssid==ssid &&
       savedNetworks[i].bssid==bssid) return i;
  return -1;
}

int findFreeSlot() {
  for(int i=0;i<MAX_SAVED_WIFI;i++)
    if(!savedNetworks[i].used) return i;
  return -1;
}

bool addSavedNetwork(String ssid,String password,String bssid) {
  int i=findSavedNetwork(ssid,bssid);
  if(i<0) i=findFreeSlot();
  if(i<0) return false;

  savedNetworks[i].ssid=ssid;
  savedNetworks[i].password=password;
  savedNetworks[i].bssid=bssid;
  savedNetworks[i].used=true;
  activeNetworkIndex=i;
  saveNetwork(i);
  return true;
}

void deleteSavedNetwork(int i) {
  if(i<0 || i>=MAX_SAVED_WIFI) return;
  savedNetworks[i].ssid="";
  savedNetworks[i].password="";
  savedNetworks[i].bssid="";
  savedNetworks[i].used=false;
  saveNetwork(i);
  if(activeNetworkIndex==i) activeNetworkIndex=-1;
}

String pageStart(String title) {
  String p="<!DOCTYPE html><html><head>";
  p+="<meta name='viewport' content='width=device-width,initial-scale=1'>";
  p+="<meta name='theme-color' content='#0b1020'>";
  p+="<title>"+title+"</title><style>";
  p+="*{box-sizing:border-box}body{margin:0;min-height:100vh;font-family:Inter,ui-sans-serif,system-ui,-apple-system,'Segoe UI',sans-serif;background:radial-gradient(900px 420px at 50% -160px,#26356b 0%,transparent 70%),#0b1020;color:#eef2ff;padding:24px 15px 40px} .box{max-width:620px;margin:0 auto;background:rgba(17,24,43,.94);border:1px solid #293653;border-radius:24px;padding:24px;box-shadow:0 24px 70px rgba(0,0,0,.38)}";
  p+=".brand{display:flex;align-items:center;gap:12px;margin-bottom:24px}.brand-mark{width:42px;height:42px;border-radius:14px;display:grid;place-items:center;background:linear-gradient(145deg,#6366f1,#8b5cf6);font-weight:800;font-size:19px;box-shadow:0 8px 24px #6366f133}.brand-name{font-size:18px;font-weight:750;letter-spacing:-.4px}.brand-sub{font-size:12px;color:#93a2c4;margin-top:2px}h1{font-size:25px;letter-spacing:-.7px;margin:0 0 5px}h2{font-size:15px;letter-spacing:-.2px;margin:22px 0 10px;color:#dbe5ff}p{line-height:1.55}.small{color:#9aa8c4;font-size:13px;line-height:1.55}.card{background:#10182b;border:1px solid #293653;border-radius:16px;padding:16px;margin:12px 0}.card-title{font-size:13px;font-weight:700;color:#dce6ff;margin-bottom:12px}.status-row{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:10px 0;border-bottom:1px solid #202c45}.status-row:last-child{border-bottom:0;padding-bottom:0}.status-label{display:flex;align-items:center;gap:9px;font-size:13px;color:#c4cee4}.status-icon{width:30px;height:30px;border-radius:10px;background:#1c2944;display:grid;place-items:center;color:#a5b4fc}.pill{display:inline-flex;align-items:center;gap:6px;padding:5px 9px;border-radius:999px;font-size:11px;font-weight:750;letter-spacing:.2px;white-space:nowrap}.pill:before{content:'';width:6px;height:6px;border-radius:50%;background:currentColor}.pill.good{background:#123629;color:#6ee7b7}.pill.bad{background:#3b202b;color:#fda4af}.pill.pending{background:#332d1c;color:#fcd34d}button,.btn{display:block;width:100%;padding:13px 15px;margin-top:10px;background:linear-gradient(135deg,#6366f1,#7c3aed);color:white;border:1px solid #818cf8;border-radius:12px;text-align:center;text-decoration:none;font-weight:700;font-size:13px;cursor:pointer;transition:.18s}button:hover,.btn:hover{filter:brightness(1.1);transform:translateY(-1px)}button:disabled{opacity:.45;cursor:not-allowed}.btn.secondary{background:#1a2742;border-color:#344566;color:#dce6ff}.btn.danger,.danger{background:#7f2638;border-color:#a43a50}input{width:100%;padding:13px 14px;margin:8px 0 12px;border-radius:11px;background:#0b1222;color:white;border:1px solid #34415e;font-size:14px}input:focus{outline:none;border-color:#818cf8;box-shadow:0 0 0 3px #6366f122}form{margin-top:10px}.network-meta{font-size:12px;color:#93a2c4;margin-top:4px}.divider{height:1px;background:#25314b;margin:18px 0}.footnote{font-size:11px;color:#7f8eac;text-align:center;margin:20px 0 0}a{color:#a5b4fc} .section-intro{margin:0 0 16px;color:#9aa8c4;font-size:13px;line-height:1.5}";
  p+="@media(max-width:480px){body{padding:12px 10px 28px}.box{padding:18px;border-radius:19px}h1{font-size:22px}}";
  p+="</style></head><body><main class='box'><div class='brand'><div class='brand-mark'>L</div><div><div class='brand-name'>Lecture Mate</div><div class='brand-sub'>Personal lecture recording system</div></div></div>";
  return p;
}
String pageEnd(){return "</main></body></html>";}

void handleCaptivePortal() {
  server.sendHeader("Location","http://192.168.4.1/",true);
  server.send(302,"text/html","");
}

// Forward declarations for Phase 4 integration helpers.
bool validRecordingName(const String &name);
bool parseBackendHost(String &host, uint16_t &port);
bool writeClientAll(WiFiClientSecure &client, const uint8_t *data, size_t length);
int readHttpResponse(WiFiClientSecure &client, String &bodyPreview);

// ---------------- Phase 4 backend integration ----------------
String extractJsonString(const String &json, const char* key) {
  String pattern = "\"" + String(key) + "\"";
  int keyPos = json.indexOf(pattern);
  if (keyPos < 0) return "";
  int colonPos = json.indexOf(':', keyPos + pattern.length());
  if (colonPos < 0) return "";
  int firstQuote = json.indexOf('"', colonPos + 1);
  if (firstQuote < 0) return "";
  int secondQuote = json.indexOf('"', firstQuote + 1);
  while (secondQuote > 0 && json.charAt(secondQuote - 1) == '\\') secondQuote = json.indexOf('"', secondQuote + 1);
  if (secondQuote < 0) return "";
  return json.substring(firstQuote + 1, secondQuote);
}
int extractJsonInt(const String &json, const char* key, int fallback) {
  String pattern = "\"" + String(key) + "\"";
  int pos = json.indexOf(pattern); if (pos < 0) return fallback;
  int colon = json.indexOf(':', pos + pattern.length()); if (colon < 0) return fallback;
  int start = colon + 1; while (start < (int)json.length() && (json[start]==' ' || json[start]=='\t')) start++;
  int end = start; while (end < (int)json.length() && (isDigit(json[end]) || json[end]=='-')) end++;
  return end == start ? fallback : json.substring(start,end).toInt();
}
bool extractJsonBool(const String &json, const char* key, bool fallback) {
  String pattern = "\"" + String(key) + "\"";
  int pos=json.indexOf(pattern); if(pos<0) return fallback;
  int colon=json.indexOf(':',pos+pattern.length()); if(colon<0) return fallback;
  String tail=json.substring(colon+1,min((int)json.length(),colon+18)); tail.trim();
  if(tail.startsWith("true")) return true; if(tail.startsWith("false")) return false; return fallback;
}
String getPairingRemaining() {
  if (!activePairingCode.length()) return "0";
  unsigned long elapsed = millis() - pairingCodeIssuedAt;
  if (elapsed >= pairingCodeTtlMs) { activePairingCode=""; return "0"; }
  return String((pairingCodeTtlMs-elapsed)/1000UL);
}
bool backendRequestReady() {
  return WiFi.status()==WL_CONNECTED && ensureLectureMateClock() &&
         String(ESP32_DEVICE_SECRET).length() >= 32;
}
void parseBindingJson(const String &body) {
  String n=extractJsonString(body,"deviceName"); if(n.length()) sharedDeviceName=n;
  String b=extractJsonString(body,"bindingStatus"); if(b.length()) bindingStatus=b;
  linkedMemberCount=extractJsonInt(body,"memberCount",linkedMemberCount);
  maxLinkedMembers=extractJsonInt(body,"maxMembers",MAX_LINKED_ACCOUNTS);
  remainingAccountSlots=extractJsonInt(body,"remainingSlots",max(0,maxLinkedMembers-linkedMemberCount));
  bindingCapacityFull=extractJsonBool(body,"capacityFull",linkedMemberCount>=maxLinkedMembers);
  multiAccountEnabled=extractJsonBool(body,"multiAccountEnabled",multiAccountEnabled);
}
bool refreshDeviceBinding() {
  if (!backendRequestReady()) return false;
  WiFiClientSecure client; configureLectureMateTLS(client,10000);
  HTTPClient http; http.setTimeout(10000);
  String url=String(LECTURE_MATE_BASE_URL)+"/api/esp32/device-binding";
  if(!http.begin(client,url)) return false;
  http.addHeader("x-device-token",ESP32_DEVICE_SECRET); http.addHeader("x-device-id",ESP32_DEVICE_ID);
  int code=http.GET(); String body=code>0?http.getString():""; http.end();
  if(code==200){parseBindingJson(body);return true;} return false;
}
bool generatePairingCode() {
  if(!backendRequestReady()) { pairingErrorState=true; pairingStatusMessage="Connect the ESP32 to your router and verify its device secret first."; return false; }
  refreshDeviceBinding();
  if(bindingCapacityFull || linkedMemberCount>=maxLinkedMembers){activePairingCode="";pairingErrorState=true;pairingStatusMessage="All five account slots are occupied. Unlink an account in LectureMate first.";return false;}
  WiFiClientSecure client; configureLectureMateTLS(client,15000);
  HTTPClient http; http.setTimeout(15000);
  String url=String(LECTURE_MATE_BASE_URL)+"/api/esp32/pairing/generate";
  if(!http.begin(client,url)){pairingErrorState=true;pairingStatusMessage="Could not connect securely to LectureMate.";return false;}
  http.addHeader("Content-Type","application/json");
  http.addHeader("x-device-token",ESP32_DEVICE_SECRET); http.addHeader("x-device-id",ESP32_DEVICE_ID);
  String body="{\"deviceId\":\""+String(ESP32_DEVICE_ID)+"\",\"deviceName\":\""+jsonEscape(sharedDeviceName)+"\"}";
  int code=http.POST(body); String response=code>0?http.getString():""; http.end();
  if(code>=200 && code<300){String codeText=extractJsonString(response,"pairingCode"); int ttl=extractJsonInt(response,"ttlSeconds",300); parseBindingJson(response);
    if(codeText.length()){activePairingCode=codeText;pairingCodeIssuedAt=millis();pairingCodeTtlMs=(unsigned long)ttl*1000UL;pairingErrorState=false;pairingStatusMessage="Enter this code in LectureMate → ESP Settings → Linked Devices.";return true;}}
  activePairingCode=""; pairingErrorState=true;
  String err=extractJsonString(response,"errorCode"); if(!err.length()) err=extractJsonString(response,"error");
  pairingStatusMessage=(err=="pairing_rate_limit_exceeded")?"Too many requests. Please wait before generating another code.":"Pairing code could not be generated (HTTP "+String(code)+").";
  return false;
}
void reportQueueStatus(const String &commandId,const String &status,int pct,const String &errorCode="",const String &errorMessage="") {
  if(!backendRequestReady() || !commandId.length()) return;
  WiFiClientSecure client; configureLectureMateTLS(client,15000); HTTPClient http; http.setTimeout(15000);
  String url=String(LECTURE_MATE_BASE_URL)+"/api/esp32/commands/status";
  if(!http.begin(client,url)) return;
  http.addHeader("Content-Type","application/json"); http.addHeader("x-device-token",ESP32_DEVICE_SECRET); http.addHeader("x-device-id",ESP32_DEVICE_ID);
  String body="{\"commandId\":\""+jsonEscape(commandId)+"\",\"deviceId\":\""+String(ESP32_DEVICE_ID)+"\",\"status\":\""+status+"\",\"uploadProgressPercent\":"+String(pct);
  if(errorCode.length()) body+=",\"errorCode\":\""+jsonEscape(errorCode)+"\"";
  if(errorMessage.length()) body+=",\"error\":\""+jsonEscape(errorMessage)+"\"";
  body+="}"; http.POST(body); http.end();
}
int postTicketChunk(File &audio,const String &fileName,uint32_t fileSize,uint32_t chunkIndex,uint32_t totalChunks,const String &uploadId,const String &jobId,const String &ticket,uint32_t offset,uint32_t chunkLength) {
  String host; uint16_t port=443;
  if(!parseBackendHost(host,port) || !ensureLectureMateClock()) return -1;
  WiFiClientSecure client; configureLectureMateTLS(client,UPLOAD_RESPONSE_TIMEOUT);
  if(!client.connect(host.c_str(),port)){printLectureMateTLSError(client,"ticket upload TLS");return -1;}

  String boundary="----LectureMate"+String((uint32_t)esp_random(),HEX);
  String prefix;
  auto addField=[&](const String &name,const String &value){
    prefix+="--"+boundary+"\r\n";
    prefix+="Content-Disposition: form-data; name=\""+name+"\"\r\n\r\n";
    prefix+=value+"\r\n";
  };
  addField("uploadId",uploadId);
  addField("chunkIndex",String(chunkIndex));
  addField("totalChunks",String(totalChunks));
  addField("fileName",fileName);
  addField("fileSize",String(fileSize));
  addField("mimeType","audio/wav");
  addField("jobId",jobId);
  prefix+="--"+boundary+"\r\n";
  prefix+="Content-Disposition: form-data; name=\"file\"; filename=\""+fileName+"\"\r\n";
  prefix+="Content-Type: audio/wav\r\n\r\n";
  String suffix="\r\n--"+boundary+"--\r\n";
  uint32_t contentLength=prefix.length()+chunkLength+suffix.length();

  String headers="POST /api/esp32/upload HTTP/1.1\r\n";
  headers+="Host: "+host+"\r\nConnection: close\r\n";
  headers+="Content-Type: multipart/form-data; boundary="+boundary+"\r\n";
  headers+="Content-Length: "+String(contentLength)+"\r\n";
  headers+="x-device-token: "+String(ESP32_DEVICE_SECRET)+"\r\n";
  headers+="x-device-id: "+String(ESP32_DEVICE_ID)+"\r\n";
  headers+="x-upload-ticket: "+ticket+"\r\n";
  headers+="x-upload-id: "+uploadId+"\r\n\r\n";

  if(!writeClientAll(client,(const uint8_t*)headers.c_str(),headers.length()) ||
     !writeClientAll(client,(const uint8_t*)prefix.c_str(),prefix.length()) || !audio.seek(offset)){
    client.stop(); return -1;
  }
  uint8_t buf[2048]; uint32_t remain=chunkLength;
  while(remain){
    size_t want=min((uint32_t)sizeof(buf),remain); size_t got=audio.read(buf,want);
    if(!got || !writeClientAll(client,buf,got)){client.stop();return -1;}
    remain-=got; yield();
  }
  if(!writeClientAll(client,(const uint8_t*)suffix.c_str(),suffix.length())){client.stop();return -1;}
  client.flush(); String preview; int status=readHttpResponse(client,preview); client.stop();
  if(status<200||status>=300) Serial.println("[QUEUE] Chunk response: "+preview.substring(0,300));
  return status;
}

bool uploadQueuedRecording(const String &commandId,const String &fileName,const String &uploadId,const String &jobId,const String &ticket) {
  if(recording || !validRecordingName(fileName) || !SD.exists("/"+fileName)){reportQueueStatus(commandId,"failed",0,"wav_file_not_found_on_sd","Requested WAV file is not available on the SD card.");return false;}
  File audio=SD.open("/"+fileName,FILE_READ); if(!audio){reportQueueStatus(commandId,"failed",0,"wav_file_open_failed","Could not open WAV file.");return false;}
  uint32_t size=audio.size(); if(size<=44){audio.close();reportQueueStatus(commandId,"failed",0,"invalid_wav_file_size","WAV file is empty or invalid.");return false;}
  uint32_t chunks=(size+CHUNK_SIZE-1)/CHUNK_SIZE; queueUploadInProgress=true; activeQueueCommandId=commandId;activeQueueFileName=fileName;activeQueueProgress=0;
  reportQueueStatus(commandId,"uploading",0);
  bool ok=true;
  for(uint32_t i=0;i<chunks;i++){uint32_t offset=i*CHUNK_SIZE;uint32_t len=min((uint32_t)CHUNK_SIZE,size-offset);
    int code=postTicketChunk(audio,fileName,size,i,chunks,uploadId,jobId,ticket,offset,len);
    if(code<200||code>=300){ok=false;break;}
    activeQueueProgress=(int)(((i+1)*100UL)/chunks); reportQueueStatus(commandId,"uploading",activeQueueProgress);
  }
  audio.close();queueUploadInProgress=false;
  if(!ok){reportQueueStatus(commandId,"failed",activeQueueProgress,"upload_interrupted_recoverable","A chunk could not be uploaded. Retry from LectureMate to issue a fresh secure ticket.");return false;}
  reportQueueStatus(commandId,"processing",100);return true;
}
void pollUploadQueue() {
  if(!multiAccountEnabled || queueUploadInProgress || recording || WiFi.status()!=WL_CONNECTED || !backendRequestReady()) return;
  WiFiClientSecure client; configureLectureMateTLS(client,15000); HTTPClient http;http.setTimeout(15000);
  String url=String(LECTURE_MATE_BASE_URL)+"/api/esp32/commands/poll";if(!http.begin(client,url))return;
  http.addHeader("Content-Type","application/json");http.addHeader("x-device-token",ESP32_DEVICE_SECRET);http.addHeader("x-device-id",ESP32_DEVICE_ID);
  int code=http.POST(String("{\"deviceId\":\"")+ESP32_DEVICE_ID+"\"}");String response=code>0?http.getString():"";http.end();if(code!=200)return;
  int cp=response.indexOf("\"command\":{");if(cp<0)return;
  int start=response.indexOf('{',cp);if(start<0)return;int depth=0,end=-1;bool inString=false,escape=false;
  for(int i=start;i<(int)response.length();i++){char c=response[i];if(inString){if(escape)escape=false;else if(c=='\\')escape=true;else if(c=='"')inString=false;}else{if(c=='"')inString=true;else if(c=='{')depth++;else if(c=='}'&&--depth==0){end=i+1;break;}}}
  if(end<0)return;String cmd=response.substring(start,end);
  String commandId=extractJsonString(cmd,"commandId"),fileName=extractJsonString(cmd,"fileName"),uploadId=extractJsonString(cmd,"uploadId"),jobId=extractJsonString(cmd,"jobId"),ticket=extractJsonString(cmd,"uploadTicket");
  if(!commandId.length())return;
  if(!fileName.length()||!uploadId.length()||!ticket.length()){reportQueueStatus(commandId,"failed",0,"invalid_command_payload","Command did not contain required upload information.");return;}
  uploadQueuedRecording(commandId,fileName,uploadId,jobId,ticket);
}
String createBindingPortalCard() {
  String html="<div class='card'><div class='card-title'>DEVICE PAIRING</div>";
  html+="<p class='small'>Link up to five Google accounts to this ESP32. Generate a code here, then enter it in LectureMate → ESP Settings → Linked Devices.</p>";
  html+="<div class='status-row'><span class='small'>Device ID</span><b>"+String(ESP32_DEVICE_ID)+"</b></div>";
  html+="<div class='status-row'><span class='small'>Linked accounts</span><b>"+String(linkedMemberCount)+" / "+String(maxLinkedMembers)+"</b></div>";
  html+="<div class='status-row'><span class='small'>Available slots</span><b>"+String(remainingAccountSlots)+"</b></div>";
  html+="<div style='font-size:25px;letter-spacing:4px;text-align:center;padding:15px;background:#0b1222;border:1px solid #6366f1;border-radius:12px;margin:12px 0'>"+(activePairingCode.length()?activePairingCode:"--------")+"</div>";
  html += "<p class='small' style='color:";
  html += (pairingErrorState ? "#fda4af" : "#6ee7b7");
  html += "'>";
  html += pairingStatusMessage;
  html += "</p>";
  html+="<p class='small'>Code expires in <b>"+getPairingRemaining()+" seconds</b>.</p>";
  html+="<form method='POST' action='/pairing/generate'><button type='submit' "+String((bindingCapacityFull||remainingAccountSlots<=0)?"disabled":"")+">Generate Pairing Code</button></form>";
  html+="<form method='POST' action='/pairing/refresh'><button class='secondary' type='submit'>Refresh Device Status</button></form></div>";
  return html;
}
void handleGeneratePairingRoute(){generatePairingCode();server.sendHeader("Location","/",true);server.send(303,"text/plain","");}
void handleRefreshBindingRoute(){refreshDeviceBinding();server.sendHeader("Location","/",true);server.send(303,"text/plain","");}

String createSetupPage() {
  String p=pageStart("Lecture Mate Wi-Fi Manager");
  p+="<h1>Wi-Fi & Cloud Connection</h1><p class='section-intro'>Manage your network connection and check whether Lecture Mate can reach its online services.</p>";
  p+="<div class='card'><div class='card-title'>CONNECTION OVERVIEW</div>";
  p+="<div class='status-row'><div class='status-label'><span class='status-icon'>⌁</span><span><b>Wi-Fi network</b><br><span class='small' id='wifiDetail'>Waiting for connection</span></span></div><span id='wifiBadge' class='pill pending'>Checking</span></div>";
  p+="<div class='status-row'><div class='status-label'><span class='status-icon'>◎</span><span><b>Lecture Mate website</b><br><span class='small' id='websiteDetail'>Checking backend availability</span></span></div><span id='websiteBadge' class='pill pending'>Checking</span></div>";
  p+="<p class='small' id='ip' style='margin:12px 0 0'>Router IP: not connected</p></div>";
  p+="<div class='card'><div class='card-title'>RECORDING LIBRARY</div><p class='small'>Browse, listen to and manage WAV recordings stored safely on the SD card.</p><a class='btn' href='/recordings'>Open Recording Manager</a></div>";
  p+=createBindingPortalCard();
  p+="<h2>Saved networks</h2><p class='section-intro'>Choose a saved network to connect. Up to five networks can be stored.</p>";
  bool any=false;
  for(int i=0;i<MAX_SAVED_WIFI;i++) {
    if(!savedNetworks[i].used) continue;
    any=true;
    p+="<div class='card'><b>"+savedNetworks[i].ssid+"</b><br>";
    p+="<a class='btn' href='/use?index="+String(i)+"'>Connect to this network</a>";
    p+="<a class='btn danger' href='/delete?index="+String(i)+"'>Delete</a></div>";
  }
  if(!any) p+="<div class='card'>No saved networks yet.</div>";
  p+="<h2>Available networks</h2><a class='btn secondary' href='/scan'>Scan / Refresh networks</a>";
  int n=WiFi.scanNetworks(false,true);
  if(n<=0) p+="<div class='card'>No networks found. Tap Scan / Refresh networks.</div>";
  else for(int i=0;i<n;i++) {
    String ssid=WiFi.SSID(i);
    if(!ssid.length()) continue;
    String bssid=bssidToString(WiFi.BSSID(i));
    p+="<div class='card'><b>"+ssid+"</b><br><span class='small'>Signal: "+String(WiFi.RSSI(i))+" dBm</span>";
    p+="<form action='/save' method='POST'><input type='hidden' name='ssid' value='"+ssid+"'>";
    p+="<input type='hidden' name='bssid' value='"+bssid+"'>";
    p+="<input type='password' name='password' placeholder='Wi-Fi password (blank for open network)'>";
    p+="<button type='submit'>Save & Connect</button></form></div>";
  }
  WiFi.scanDelete();
  p+="<div class='divider'></div><p class='footnote'>Setup portal closes after 5 minutes. Press the Wi-Fi button (D15) to turn Wi-Fi off at any time.</p>";
  p+="<script>function badge(id,kind,label){let e=document.getElementById(id);e.className='pill '+kind;e.textContent=label;}async function upd(){try{let r=await fetch('/status',{cache:'no-store'});let j=await r.json();badge('wifiBadge',j.wifiConnected?'good':(j.wifiConnecting?'pending':'bad'),j.wifiConnected?'Connected':(j.wifiConnecting?'Connecting':'Disconnected'));document.getElementById('wifiDetail').textContent=j.wifiConnected?j.ssid:'Router connection';badge('websiteBadge',j.websiteConnected?'good':(j.wifiConnected?'bad':'pending'),j.websiteConnected?'Connected':(j.wifiConnected?'Unavailable':'Waiting'));document.getElementById('websiteDetail').textContent=j.websiteStatus||'Backend status unavailable';document.getElementById('ip').textContent=j.ip;}catch(e){badge('wifiBadge','pending','Checking');badge('websiteBadge','pending','Checking');}}setInterval(upd,3000);upd();</script>";
  p+=pageEnd(); return p;
}

void stopPortal() {
  dnsServer.stop();
  server.stop();
  WiFi.softAPdisconnect(true);
}

void startConnection(int index);

// =====================================================
// SD CARD RECORDING MANAGER
// =====================================================

String formatFileSize(uint32_t bytes) {
  if (bytes < 1024) return String(bytes) + " B";
  if (bytes < 1024UL * 1024UL) return String(bytes / 1024.0, 1) + " KB";
  return String(bytes / (1024.0 * 1024.0), 2) + " MB";
}

String formatDuration(uint32_t fileSize) {
  // Lecture Mate WAV format: 16 kHz, mono, 16-bit PCM = 32,000 bytes/sec.
  uint32_t audioBytes = fileSize > 44 ? fileSize - 44 : 0;
  uint32_t seconds = audioBytes / 32000UL;
  char value[20];
  snprintf(value, sizeof(value), "%02lu:%02lu:%02lu",
           (unsigned long)(seconds / 3600),
           (unsigned long)((seconds / 60) % 60),
           (unsigned long)(seconds % 60));
  return String(value);
}

bool validRecordingName(const String &name) {
  // Only permit files created by Lecture Mate; reject paths and traversal.
  if (!name.startsWith("lecture_") || !name.endsWith(".wav")) return false;
  if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf("..") >= 0) return false;
  for (size_t i = 0; i < name.length(); i++) {
    char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
  }
  return true;
}

int countSDRecordings() {
  int count = 0;
  File root = SD.open("/");
  if (!root || !root.isDirectory()) return 0;
  File file = root.openNextFile();
  while (file) {
    String name = String(file.name());
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (!file.isDirectory() && name.startsWith("lecture_") && name.endsWith(".wav")) count++;
    file.close();
    file = root.openNextFile();
  }
  root.close();
  return count;
}

String jsonEscape(const String &value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '\\' || c == '"') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if ((uint8_t)c >= 0x20) out += c;
  }
  return out;
}

bool isCurrentRecording(const String &name) {
  return recording && name == currentFileName;
}

String createRecordingsPage() {
  String p = pageStart("Lecture Mate - My Recordings");
  p += "<a class='btn' href='/'>Back to Wi-Fi Manager</a>";
  p += "<h1>My Recordings</h1>";
  p += "<p class='small'>WAV files saved on the Lecture Mate SD card.</p>";
  if (recording) {
    p += "<div class='card'><b class='green'>Recording in progress</b><br>";
    p += currentFileName + "<p class='small'>This file is still being written. Playback and deletion are disabled until recording stops.</p></div>";
  }

  File root = SD.open("/");
  bool found = false;
  if (root && root.isDirectory()) {
    File file = root.openNextFile();
    while (file) {
      String name = String(file.name());
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      if (!file.isDirectory() && name.startsWith("lecture_") && name.endsWith(".wav")) {
        found = true;
        uint32_t size = file.size();
        bool busy = recording;
        p += "<div class='card'><b>" + name + "</b><br>";
        p += "<span class='small'>Size: " + formatFileSize(size) + " · Duration: " + formatDuration(size) + "</span>";
        if (busy) {
          p += "<p class='green'>Recording is active — file management is temporarily paused to protect audio capture.</p>";
          p += "<button disabled>Playback unavailable while recording</button>";
          p += "<button class='danger' disabled>Delete unavailable while recording</button>";
        } else {
          p += "<a class='btn' href='/listen?name=" + name + "'>Play recording</a>";
          p += "<form method='POST' action='/upload'><input type='hidden' name='name' value='" + name + "'>";
          p += "<button type='submit'>Upload to Convert</button></form>";
          p += "<a class='btn danger' href='/delete-confirm?name=" + name + "'>Delete recording</a>";
        }
        p += "</div>";
      }
      file.close();
      file = root.openNextFile();
    }
    root.close();
  }
  if (!found) p += "<div class='card'>No WAV recordings found on the SD card yet.</div>";
  p += "<a class='btn' href='/recordings'>Refresh recordings</a>";
  p += "<p class='footnote'>Your recordings remain on the SD card until you explicitly choose to delete them.</p>";
  p += pageEnd();
  return p;
}

String createListenPage(const String &name) {
  String p = pageStart("Play Recording - Lecture Mate");
  p += "<a class='btn' href='/recordings'>Back to Recordings</a>";
  p += "<h1>" + name + "</h1>";
  p += "<p class='small'>Playback is streamed from the SD card. Keep the phone connected to LectureMate-Setup.</p>";
  p += "<audio controls preload='metadata' style='width:100%;margin:18px 0' src='/audio?name=" + name + "'></audio>";
  p += pageEnd();
  return p;
}

String createDeleteConfirmPage(const String &name) {
  String p = pageStart("Confirm Delete - Lecture Mate");
  p += "<h1>Delete recording?</h1>";
  p += "<div class='card'><b>" + name + "</b><p>This permanently deletes the file from the Lecture Mate SD card. This cannot be undone.</p></div>";
  p += "<form method='POST' action='/delete'><input type='hidden' name='name' value='" + name + "'>";
  p += "<button class='danger' type='submit'>Yes, delete permanently</button></form>";
  p += "<a class='btn' href='/recordings'>Cancel</a>";
  p += pageEnd();
  return p;
}

// =====================================================
// ESP32 -> LOCAL LECTURE MATE BACKEND UPLOAD
// Uses the exact backend multipart fields and 256 KB chunk size.
// Audio is read from SD and streamed directly; it is never deleted.
// =====================================================

bool parseBackendHost(String &host, uint16_t &port) {
  String base = String(LECTURE_MATE_BASE_URL);
  base.trim();
  if (!base.startsWith("https://")) return false;
  base.remove(0, 8);
  int slash = base.indexOf('/');
  if (slash >= 0) base = base.substring(0, slash);
  if (!base.length()) return false;

  int colon = base.lastIndexOf(':');
  if (colon >= 0) {
    host = base.substring(0, colon);
    long parsedPort = base.substring(colon + 1).toInt();
    if (parsedPort < 1 || parsedPort > 65535) return false;
    port = (uint16_t)parsedPort;
  } else {
    host = base;
    port = 443;
  }
  return host.length() > 0;
}

bool writeClientAll(WiFiClientSecure &client, const uint8_t *data, size_t length) {
  size_t sent = 0;
  unsigned long started = millis();
  while (sent < length) {
    if (!client.connected() && !client.available()) return false;
    size_t n = client.write(data + sent, length - sent);
    if (n > 0) {
      sent += n;
      started = millis();
    } else {
      if (millis() - started > 30000UL) return false;
      delay(1);
    }
  }
  return true;
}

int readHttpResponse(WiFiClientSecure &client, String &bodyPreview) {
  client.setTimeout(UPLOAD_RESPONSE_TIMEOUT);
  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  int firstSpace = statusLine.indexOf(' ');
  int statusCode = firstSpace >= 0 ? statusLine.substring(firstSpace + 1, firstSpace + 4).toInt() : -1;
  if (statusCode <= 0) return -1;

  int contentLength = -1;
  bool headerEnded = false;
  unsigned long headerStart = millis();
  while (millis() - headerStart < 20000UL) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (!line.length()) { headerEnded = true; break; }
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) contentLength = lower.substring(15).toInt();
  }
  if (!headerEnded) return statusCode;

  char buffer[512];
  if (contentLength >= 0) {
    int remaining = contentLength;
    while (remaining > 0) {
      size_t want = (size_t)min((int)sizeof(buffer), remaining);
      size_t got = client.readBytes((char *)buffer, want);
      if (!got) break;
      size_t keep = got;
      if (bodyPreview.length() + keep > 6000) keep = 6000 - bodyPreview.length();
      if (keep) bodyPreview.concat(buffer, keep);
      remaining -= got;
      yield();
    }
  } else {
    unsigned long lastData = millis();
    while (client.connected() || client.available()) {
      int availableBytes = client.available();
      if (availableBytes > 0) {
        size_t want = (size_t)min((int)sizeof(buffer), availableBytes);
        int got = client.read((uint8_t *)buffer, want);
        if (got > 0) {
          size_t keep = (size_t)got;
          if (bodyPreview.length() + keep > 6000) keep = 6000 - bodyPreview.length();
          if (keep) bodyPreview.concat(buffer, keep);
          lastData = millis();
        }
      } else {
        if (millis() - lastData > 30000UL) break;
        delay(2);
      }
      yield();
    }
  }
  return statusCode;
}

int postWavChunk(File &audio, const String &fileName, uint32_t fileSize,
                 uint32_t chunkIndex, uint32_t totalChunks,
                 const String &uploadId, uint32_t offset, uint32_t chunkLength,
                 String &responseBody) {
  String host;
  uint16_t port = 3000;
  if (!parseBackendHost(host, port)) {
    Serial.println("ERROR: LECTURE_MATE_BASE_URL must be a valid https:// URL");
    return -1;
  }

  if (!ensureLectureMateClock()) {
    Serial.println("Upload postponed: system clock is not synchronized.");
    return -1;
  }

  WiFiClientSecure client;
  configureLectureMateTLS(client, UPLOAD_RESPONSE_TIMEOUT);
  Serial.printf("[UPLOAD] Connecting to %s:%u via Wi-Fi '%s' (IP %s)\n",
                host.c_str(), port, WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  if (!client.connect(host.c_str(), port)) {
    Serial.println("[UPLOAD] Secure connection to LectureMate failed.");
    printLectureMateTLSError(client, "WAV upload TLS connection");
    return -1;
  }

  String boundary = "----LectureMate" + String((uint32_t)esp_random(), HEX);
  String prefix;
  prefix.reserve(700);
  auto addField = [&](const String &fieldName, const String &value) {
    prefix += "--" + boundary + "\r\n";
    prefix += "Content-Disposition: form-data; name=\"" + fieldName + "\"\r\n\r\n";
    prefix += value + "\r\n";
  };
  addField("uploadId", uploadId);
  addField("chunkIndex", String(chunkIndex));
  addField("totalChunks", String(totalChunks));
  addField("fileName", fileName);
  addField("fileSize", String(fileSize));
  addField("mimeType", "audio/wav");
  prefix += "--" + boundary + "\r\n";
  prefix += "Content-Disposition: form-data; name=\"file\"; filename=\"" + fileName + "\"\r\n";
  prefix += "Content-Type: audio/wav\r\n\r\n";
  String suffix = "\r\n--" + boundary + "--\r\n";
  uint32_t contentLength = prefix.length() + chunkLength + suffix.length();

  String requestHeaders;
  requestHeaders.reserve(500);
  requestHeaders += "POST /api/esp32/upload HTTP/1.1\r\n";
  requestHeaders += "Host: " + host + "\r\n";
  requestHeaders += "Connection: close\r\n";
  requestHeaders += "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n";
  requestHeaders += "Content-Length: " + String(contentLength) + "\r\n";
  requestHeaders += "x-device-token: " + String(ESP32_DEVICE_SECRET) + "\r\n";
  requestHeaders += "x-device-id: " + String(ESP32_DEVICE_ID) + "\r\n\r\n";

  Serial.printf("Uploading chunk %lu/%lu (%lu bytes)\n",
                (unsigned long)(chunkIndex + 1), (unsigned long)totalChunks,
                (unsigned long)chunkLength);

  if (!writeClientAll(client, (const uint8_t *)requestHeaders.c_str(), requestHeaders.length()) ||
      !writeClientAll(client, (const uint8_t *)prefix.c_str(), prefix.length())) {
    client.stop();
    return -1;
  }

  if (!audio.seek(offset)) {
    Serial.println("ERROR: Could not seek to WAV chunk position.");
    client.stop();
    return -1;
  }

  uint8_t buffer[2048];
  uint32_t remaining = chunkLength;
  while (remaining > 0) {
    size_t want = (size_t)min((uint32_t)sizeof(buffer), remaining);
    size_t got = audio.read(buffer, want);
    if (got == 0) {
      Serial.println("ERROR: SD read failed during upload.");
      client.stop();
      return -1;
    }
    if (!writeClientAll(client, buffer, got)) {
      Serial.println("ERROR: Network write failed during chunk upload.");
      client.stop();
      return -1;
    }
    remaining -= got;
    yield();
  }

  if (!writeClientAll(client, (const uint8_t *)suffix.c_str(), suffix.length())) {
    client.stop();
    return -1;
  }
  client.flush();

  int statusCode = readHttpResponse(client, responseBody);
  client.stop();
  Serial.printf("Backend HTTP response: %d\n", statusCode);
  if (responseBody.length()) Serial.println(responseBody.substring(0, 700));
  return statusCode;
}

String createUploadId() {
  return "lm_" + String((uint32_t)esp_random(), HEX) + "_" + String(millis(), HEX);
}

bool uploadRecordingToBackend(const String &fileName) {
  if (recording || paused) {
    Serial.println("Upload blocked: stop recording first.");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Upload blocked: connect Lecture Mate to the same Wi-Fi network as the laptop first.");
    return false;
  }
  if (String(ESP32_DEVICE_SECRET).length() < 32 ||
      String(ESP32_DEVICE_SECRET).startsWith("PASTE_YOUR_")) {
    Serial.println("Upload blocked: set ESP32_DEVICE_SECRET in the sketch first.");
    return false;
  }
  if (!validRecordingName(fileName)) return false;

  String filePath = "/" + fileName;
  File audio = SD.open(filePath, FILE_READ);
  if (!audio) {
    Serial.println("ERROR: Could not open selected WAV on SD card.");
    return false;
  }

  uint32_t fileSize = audio.size();
  if (fileSize > 500UL * 1024UL * 1024UL) {
    Serial.println("ERROR: Backend maximum WAV upload size is 500 MB.");
    audio.close();
    return false;
  }
  if (fileSize <= 44) {
    Serial.println("ERROR: WAV file is empty or incomplete.");
    audio.close();
    return false;
  }
  uint8_t wavHeader[12];
  if (audio.read(wavHeader, sizeof(wavHeader)) != sizeof(wavHeader) ||
      memcmp(wavHeader, "RIFF", 4) != 0 || memcmp(wavHeader + 8, "WAVE", 4) != 0) {
    Serial.println("ERROR: Selected file does not have a valid RIFF/WAVE header.");
    audio.close();
    return false;
  }
  audio.seek(0);

  uint32_t totalChunks = (fileSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
  String uploadId = createUploadId();
  Serial.println();
  Serial.println("========================================");
  Serial.println("LECTURE MATE UPLOAD STARTED");
  Serial.println("File: " + fileName);
  Serial.println("Size: " + formatFileSize(fileSize));
  Serial.println("Upload ID: " + uploadId);
  Serial.println("Original file will remain on microSD.");
  Serial.println("========================================");

  bool completed = false;
  for (int attempt = 0; attempt < 2 && !completed; attempt++) {
    if (attempt > 0) {
      uploadId = createUploadId();
      Serial.println("Server requested restart. Starting again from chunk 1 with a new upload ID.");
    }

    bool restartWholeUpload = false;
    for (uint32_t chunkIndex = 0; chunkIndex < totalChunks; chunkIndex++) {
      uint32_t offset = chunkIndex * CHUNK_SIZE;
      uint32_t chunkLength = min((uint32_t)CHUNK_SIZE, fileSize - offset);
      String responseBody;
      int code = postWavChunk(audio, fileName, fileSize, chunkIndex, totalChunks,
                              uploadId, offset, chunkLength, responseBody);

      // Intermediate chunks are safe to retry: the backend treats duplicate chunks idempotently.
      if (code < 0 && chunkIndex + 1 < totalChunks) {
        responseBody = "";
        code = postWavChunk(audio, fileName, fileSize, chunkIndex, totalChunks,
                            uploadId, offset, chunkLength, responseBody);
      }

      if (code == 409 && responseBody.indexOf("restart_required") >= 0 && attempt == 0) {
        restartWholeUpload = true;
        break;
      }
      if (code != 200) {
        Serial.println("UPLOAD FAILED. HTTP " + String(code));
        if (code < 0 && chunkIndex + 1 == totalChunks) {
          Serial.println("The final request may still be processing. Check the website dashboard before trying again.");
        } else {
          Serial.println("The WAV remains safely on the SD card. You can retry from the portal.");
        }
        audio.close();
        return false;
      }

      if (chunkIndex + 1 == totalChunks) {
        if (responseBody.indexOf("\"status\":\"completed\"") >= 0 ||
            responseBody.indexOf("\"status\": \"completed\"") >= 0) {
          completed = true;
        } else {
          Serial.println("Final chunk was accepted but completion was not confirmed in the response.");
          Serial.println("Check the Lecture Mate website dashboard before uploading again.");
          audio.close();
          return false;
        }
      }
    }

    if (restartWholeUpload) continue;
    if (!completed) break;
  }

  audio.close();
  if (completed) {
    Serial.println();
    Serial.println("UPLOAD + GEMINI PROCESSING COMPLETED.");
    Serial.println("Open Lecture Mate website > New recordings.");
    Serial.println("Original WAV is still on the ESP32 microSD card.");
    return true;
  }

  Serial.println("Upload did not complete. The original WAV remains on the microSD card.");
  return false;
}

void configurePortalRoutes() {
  server.on("/",HTTP_GET,[](){ server.send(200,"text/html",createSetupPage()); });

  server.on("/recordings", HTTP_GET, []() {
    server.send(200, "text/html", createRecordingsPage());
  });

  server.on("/listen", HTTP_GET, []() {
    String name = server.arg("name");
    if (recording || !validRecordingName(name) || !SD.exists("/" + name)) {
      server.send(404, "text/plain", "Recording not available");
      return;
    }
    server.send(200, "text/html", createListenPage(name));
  });

  server.on("/audio", HTTP_GET, []() {
    String name = server.arg("name");
    String path = "/" + name;
    if (recording || !validRecordingName(name) || !SD.exists(path)) {
      server.send(404, "text/plain", "Recording not available");
      return;
    }
    File audio = SD.open(path, FILE_READ);
    if (!audio) {
      server.send(500, "text/plain", "Could not open recording");
      return;
    }
    server.sendHeader("Content-Disposition", "inline; filename=\"" + name + "\"");
    server.streamFile(audio, "audio/wav");
    audio.close();
  });

  server.on("/upload", HTTP_POST, []() {
    String name = server.arg("name");
    if (recording || !validRecordingName(name) || !SD.exists("/" + name)) {
      server.send(400, "text/html", pageStart("Upload unavailable") +
        "<h2>Recording cannot be uploaded.</h2><p class='small'>Stop recording and select a valid saved WAV file.</p><a class='btn' href='/recordings'>Back to recordings</a>" + pageEnd());
      return;
    }
    if (WiFi.status() != WL_CONNECTED) {
      server.send(400, "text/html", pageStart("Wi-Fi not connected") +
        "<h2>Connect to your router first.</h2><p class='small'>The ESP32 and laptop running Lecture Mate must be on the same Wi-Fi network.</p><a class='btn' href='/'>Open Wi-Fi manager</a>" + pageEnd());
      return;
    }
    if (String(ESP32_DEVICE_SECRET).length() < 32 || String(ESP32_DEVICE_SECRET).startsWith("PASTE_YOUR_")) {
      server.send(503, "text/html", pageStart("Device setup required") +
        "<h2>Device secret is not configured.</h2><p class='small'>Update ESP32_DEVICE_SECRET in the Arduino sketch, then upload the firmware again.</p>" + pageEnd());
      return;
    }

    bool ok = uploadRecordingToBackend(name);
    String content = pageStart(ok ? "Upload completed" : "Upload not completed");
    if (ok) {
      content += "<h1>Lecture uploaded successfully</h1><p class='small'>Gemini processing completed. Your generated notes should now appear in the website's New recordings folder.</p>";
    } else {
      content += "<h1>Upload could not be confirmed</h1><p class='small'>Check the website dashboard and Serial Monitor. Your original WAV remains on the SD card and has not been deleted.</p>";
    }
    content += "<a class='btn' href='/recordings'>Back to recordings</a><a class='btn secondary' href='/'>Wi-Fi manager</a>" + pageEnd();
    server.send(ok ? 200 : 500, "text/html", content);
  });

  server.on("/delete-confirm", HTTP_GET, []() {
    String name = server.arg("name");
    String path = "/" + name;
    if (recording || !validRecordingName(name) || !SD.exists(path)) {
      server.send(400, "text/plain", "Recording cannot be deleted");
      return;
    }
    server.send(200, "text/html", createDeleteConfirmPage(name));
  });

  server.on("/delete", HTTP_POST, []() {
    String name = server.arg("name");
    String path = "/" + name;
    if (recording || !validRecordingName(name) || !SD.exists(path)) {
      server.send(400, "text/plain", "Recording cannot be deleted");
      return;
    }
    if (SD.remove(path)) {
      server.send(200, "text/html", pageStart("Recording Deleted") +
        "<h2>Recording deleted successfully.</h2><a class='btn' href='/recordings'>Back to Recordings</a>" + pageEnd());
    } else {
      server.send(500, "text/html", pageStart("Delete Failed") +
        "<h2>Could not delete the recording.</h2><a class='btn' href='/recordings'>Back</a>" + pageEnd());
    }
  });
  server.on("/pairing/generate",HTTP_POST,handleGeneratePairingRoute);
  server.on("/pairing/refresh",HTTP_POST,handleRefreshBindingRoute);
  server.on("/scan",HTTP_GET,[](){ server.sendHeader("Location","/",true); server.send(303,"text/plain",""); });
  server.on("/status",HTTP_GET,[](){
    bool wifiOk = WiFi.status()==WL_CONNECTED;
    bool connecting = wifiState==WIFI_CONNECTING_STATE;
    String ip = wifiOk ? WiFi.localIP().toString() : String("Router IP: not connected");
    String ssid = wifiOk ? WiFi.SSID() : String("Not connected");
    String safeStatus = websiteStatusMessage;
    ssid = jsonEscape(ssid);
    ip = jsonEscape(ip);
    safeStatus = jsonEscape(safeStatus);
    String j="{\"wifiConnected\":"+String(wifiOk?"true":"false")+",\"wifiConnecting\":"+String(connecting?"true":"false")+",\"ssid\":\""+ssid+"\",\"ip\":\""+ip+"\",\"websiteConnected\":"+String(websiteConnected?"true":"false")+",\"websiteStatus\":\""+safeStatus+"\"}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",j);
  });
  server.on("/save",HTTP_POST,[](){
    String ssid=server.arg("ssid"), password=server.arg("password"), bssid=server.arg("bssid");
    if(!ssid.length()){server.send(400,"text/plain","Wi-Fi name required");return;}
    if(findFreeSlot()<0 && findSavedNetwork(ssid,bssid)<0){server.send(400,"text/html",pageStart("Wi-Fi Full")+"<h2>Maximum 5 networks reached. Delete one first.</h2><a class='btn' href='/'>Back</a>"+pageEnd());return;}
    if(!addSavedNetwork(ssid,password,bssid)){server.send(500,"text/plain","Could not save network");return;}
    int idx=findSavedNetwork(ssid,bssid);
    server.send(200,"text/html",pageStart("Connecting")+"<h2>Credentials saved</h2><p>Connecting to "+ssid+". Keep this portal open to monitor status.</p><a class='btn' href='/'>Back to Wi-Fi manager</a>"+pageEnd());
    startConnection(idx);
  });
  server.on("/use",HTTP_GET,[](){
    int i=server.arg("index").toInt();
    if(i<0||i>=MAX_SAVED_WIFI||!savedNetworks[i].used){server.send(404,"text/plain","Network not found");return;}
    server.send(200,"text/html",pageStart("Connecting")+"<h2>Connecting to "+savedNetworks[i].ssid+"</h2><p>Keep the portal open. The status on the main page will update.</p><a class='btn' href='/'>Back</a>"+pageEnd());
    startConnection(i);
  });
  server.on("/delete",HTTP_GET,[](){int i=server.arg("index").toInt();deleteSavedNetwork(i);server.sendHeader("Location","/",true);server.send(303,"text/plain","");});
  server.on("/generate_204",HTTP_ANY,handleCaptivePortal);
  server.on("/gen_204",HTTP_ANY,handleCaptivePortal);
  server.on("/hotspot-detect.html",HTTP_ANY,handleCaptivePortal);
  server.on("/library/test/success.html",HTTP_ANY,handleCaptivePortal);
  server.on("/connecttest.txt",HTTP_ANY,handleCaptivePortal);
  server.on("/ncsi.txt",HTTP_ANY,handleCaptivePortal);
  server.on("/fwlink",HTTP_ANY,handleCaptivePortal);
  server.onNotFound(handleCaptivePortal);
}

void startPortal() {
  websiteConnected = false;
  websiteStatusMessage = "Waiting for router connection";
  lastWebsiteCheck = 0;
  WiFi.disconnect(true,false);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP,AP_IP,AP_NETMASK);
  WiFi.softAP(SETUP_AP_NAME,AP_PASSWORD);
  dnsServer.stop(); dnsServer.start(53,"*",AP_IP);
  configurePortalRoutes(); server.begin();
  portalActive=true; apStartTime=millis(); wifiState=WIFI_SETUP_STATE;
  wifiBlinkTimer=millis(); wifiBlinkState=false;
  Serial.println("Wi-Fi portal started: LectureMate-Setup");
  Serial.println("Open http://192.168.4.1");
}

void startConnection(int index) {
  if(index<0||index>=MAX_SAVED_WIFI||!savedNetworks[index].used){wifiState=WIFI_FAILED_STATE;wifiFailedLED();return;}
  activeNetworkIndex=index;
  // Keep the setup AP alive while connecting to the selected router.
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(savedNetworks[index].ssid.c_str(),savedNetworks[index].password.c_str());
  wifiState=WIFI_CONNECTING_STATE; wifiConnectStart=millis();
  wifiBlinkTimer=millis(); wifiBlinkState=false;
  Serial.print("[WIFI] User-selected network: "); Serial.println(savedNetworks[index].ssid);
  Serial.println("[WIFI] No static router IP is configured; the router supplies the ESP32 IP by DHCP.");
}

void updateWiFiConnection() {
  if(wifiState!=WIFI_CONNECTING_STATE) return;
  if(WiFi.status()==WL_CONNECTED){wifiState=WIFI_CONNECTED_STATE;wifiConnectedLED();Serial.println("Wi-Fi connected: "+WiFi.SSID());Serial.print("ESP32 IP: ");Serial.println(WiFi.localIP());Serial.print("Gateway: ");Serial.println(WiFi.gatewayIP());return;}
  wifiBlinkBoth();
  if(millis()-wifiConnectStart>=WIFI_CONNECT_TIMEOUT){WiFi.disconnect(false,false);wifiState=WIFI_FAILED_STATE;wifiFailedLED();Serial.println("Wi-Fi connection failed. Portal remains active for retry.");}
}

void updateWebsiteConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    websiteConnected = false;
    websiteStatusMessage = "Waiting for router connection";
    lastWebsiteCheck = 0;
    return;
  }

  // Do not make network requests while audio is being captured.
  if (recording) return;

  String base = String(LECTURE_MATE_BASE_URL);
  base.trim();
  while (base.endsWith("/")) base.remove(base.length() - 1);
  if (!base.startsWith("https://") || base.indexOf("lecture-mate.ai.studio") < 0) {
    websiteConnected = false;
    websiteStatusMessage = "Set the HTTPS LectureMate website URL in the sketch";
    return;
  }
  if (String(ESP32_DEVICE_SECRET).length() < 32 ||
      String(ESP32_DEVICE_SECRET).startsWith("PASTE_YOUR_")) {
    websiteConnected = false;
    websiteStatusMessage = "Set the ESP32 device secret in the Arduino sketch";
    return;
  }

  if (lastWebsiteCheck != 0 && millis() - lastWebsiteCheck < WEBSITE_CHECK_INTERVAL) return;
  lastWebsiteCheck = millis();

  if (!ensureLectureMateClock()) {
    websiteConnected = false;
    websiteStatusMessage = "Waiting for network time (NTP) before secure HTTPS";
    return;
  }

  Serial.printf("[BACKEND] Checking via Wi-Fi '%s'; ESP32 IP=%s; gateway=%s\n",
                WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str());
  IPAddress backendIP;
  if (WiFi.hostByName("lecture-mate.ai.studio", backendIP) == 1) {
    Serial.printf("[BACKEND] DNS lecture-mate.ai.studio -> %s\n", backendIP.toString().c_str());
  } else {
    Serial.println("[BACKEND] DNS lookup failed for lecture-mate.ai.studio");
  }

  WiFiClientSecure client;
  configureLectureMateTLS(client, WEBSITE_CHECK_TIMEOUT);
  HTTPClient http;
  http.setTimeout(WEBSITE_CHECK_TIMEOUT);
  String url = base + "/api/esp32/heartbeat";

  if (!http.begin(client, url)) {
    websiteConnected = false;
    websiteStatusMessage = "Could not start backend heartbeat";
    Serial.println("[BACKEND] HTTPClient could not initialise the HTTPS request.");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-token", ESP32_DEVICE_SECRET);
  http.addHeader("x-device-id", ESP32_DEVICE_ID);

  String payload = "{";
  payload += "\"deviceId\":\"" + String(ESP32_DEVICE_ID) + "\",";
  payload += "\"deviceName\":\"" + String(ESP32_DEVICE_NAME) + "\",";
  payload += "\"wifiNetwork\":\"" + jsonEscape(WiFi.SSID()) + "\",";
  payload += "\"ipAddress\":\"" + WiFi.localIP().toString() + "\",";
  payload += "\"sdRecordingCount\":\"" + String(countSDRecordings()) + " recordings\"";
  payload += "}";

  int code = http.POST(payload);
  if (code >= 200 && code < 300) {
    websiteConnected = true;
    websiteStatusMessage = "Backend authenticated; heartbeat acknowledged";
    Serial.printf("[BACKEND] SUCCESS: heartbeat returned HTTP %d\n", code);
  } else {
    websiteConnected = false;
    websiteStatusMessage = code > 0
      ? "Backend heartbeat returned HTTP " + String(code)
      : "Backend unreachable: " + http.errorToString(code);
    Serial.printf("[BACKEND] FAILED: HTTPClient result %d (%s)\n",
                  code, http.errorToString(code).c_str());
    if (code < 0) printLectureMateTLSError(client, "Backend heartbeat TLS/HTTPS");
  }
  http.end();
}
void turnWiFiOff() {
  websiteConnected = false;
  websiteStatusMessage = "Wi-Fi is turned off";
  lastWebsiteCheck = 0;
  Serial.println("Wi-Fi OFF (recording remains active).");
  dnsServer.stop(); server.stop(); portalActive=false;
  WiFi.disconnect(true,false); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_OFF);
  wifiState=WIFI_OFF_STATE; wifiLEDsOff();
}

void turnWiFiOn() { startPortal(); }

void handleWiFiButton() {
  bool reading=digitalRead(WIFI_BUTTON_PIN);
  if(reading!=wifiButtonLastReading) wifiButtonDebounceTime=millis();
  if((millis()-wifiButtonDebounceTime)>WIFI_BUTTON_DEBOUNCE && reading!=wifiButtonStableState){
    wifiButtonStableState=reading;
    if(wifiButtonStableState==LOW){
      if(wifiState==WIFI_OFF_STATE) turnWiFiOn();
      else turnWiFiOff();
    }
  }
  wifiButtonLastReading=reading;
}

// =====================================================
// WAV HEADER
// =====================================================

void write16(File &file, uint16_t value)
{
  file.write(
    (uint8_t)(value & 0xFF)
  );

  file.write(
    (uint8_t)((value >> 8) & 0xFF)
  );
}


void write32(File &file, uint32_t value)
{
  file.write(
    (uint8_t)(value & 0xFF)
  );

  file.write(
    (uint8_t)((value >> 8) & 0xFF)
  );

  file.write(
    (uint8_t)((value >> 16) & 0xFF)
  );

  file.write(
    (uint8_t)((value >> 24) & 0xFF)
  );
}


void writeWavHeader(
  File &file,
  uint32_t dataSize
)
{
  uint32_t fileSize =
    dataSize + 36;

  uint16_t audioFormat = 1;

  uint16_t channels = 1;

  uint16_t bitsPerSample = 16;

  uint32_t sampleRate =
    SAMPLE_RATE;

  uint16_t blockAlign =
    channels *
    bitsPerSample / 8;

  uint32_t byteRate =
    sampleRate *
    blockAlign;


  file.seek(0);


  file.write(
    (const uint8_t *)"RIFF",
    4
  );

  write32(
    file,
    fileSize
  );


  file.write(
    (const uint8_t *)"WAVE",
    4
  );


  file.write(
    (const uint8_t *)"fmt ",
    4
  );

  write32(
    file,
    16
  );


  write16(
    file,
    audioFormat
  );

  write16(
    file,
    channels
  );

  write32(
    file,
    sampleRate
  );

  write32(
    file,
    byteRate
  );

  write16(
    file,
    blockAlign
  );

  write16(
    file,
    bitsPerSample
  );


  file.write(
    (const uint8_t *)"data",
    4
  );

  write32(
    file,
    dataSize
  );
}


// =====================================================
// I2S
// =====================================================

void setupI2S()
{
  i2s_config_t config =
  {
    .mode =
      (i2s_mode_t)(
        I2S_MODE_MASTER |
        I2S_MODE_RX
      ),

    .sample_rate =
      SAMPLE_RATE,

    .bits_per_sample =
      I2S_BITS_PER_SAMPLE_32BIT,

    .channel_format =
      I2S_CHANNEL_FMT_ONLY_LEFT,

    .communication_format =
      I2S_COMM_FORMAT_I2S,

    .intr_alloc_flags =
      ESP_INTR_FLAG_LEVEL1,

    .dma_buf_count = 8,

    .dma_buf_len = 1024,

    .use_apll = false,

    .tx_desc_auto_clear = false,

    .fixed_mclk = 0
  };


  i2s_pin_config_t pins =
  {
    .bck_io_num = I2S_SCK,

    .ws_io_num = I2S_WS,

    .data_out_num =
      I2S_PIN_NO_CHANGE,

    .data_in_num = I2S_SD
  };


  i2s_driver_install(
    I2S_PORT,
    &config,
    0,
    NULL
  );


  i2s_set_pin(
    I2S_PORT,
    &pins
  );


  i2s_zero_dma_buffer(
    I2S_PORT
  );


  Serial.println(
    "INMP441 initialized."
  );
}


// =====================================================
// FILE NAME
// =====================================================

String createFileName()
{
  String filename;


  do
  {
    filename =
      "/lecture_" +
      String(fileNumber) +
      ".wav";

    fileNumber++;

  }
  while (
    SD.exists(filename)
  );


  return filename;
}


// =====================================================
// START RECORDING
// =====================================================

void startRecording()
{
  currentFileName =
    createFileName();


  Serial.println();
  Serial.println("==============================");
  Serial.println("RECORDING STARTED");

  Serial.print("File: ");
  Serial.println(
    currentFileName
  );

  Serial.println("==============================");


  audioFile =
    SD.open(
      currentFileName,
      FILE_WRITE
    );


  if (!audioFile)
  {
    Serial.println(
      "ERROR: Cannot create file!"
    );

    saveErrorLED();

    return;
  }


  uint8_t header[44] = {0};

  audioFile.write(
    header,
    44
  );


  audioDataSize = 0;

  recording = true;
  paused = false;


  digitalWrite(
    GREEN_LED_PIN,
    HIGH
  );

  digitalWrite(
    ORANGE_LED_PIN,
    LOW
  );


  Serial.println(
    "Speak now..."
  );
}


// =====================================================
// PAUSE
// =====================================================

void pauseRecording()
{
  if (
    !recording ||
    paused
  )
    return;


  paused = true;


  digitalWrite(
    GREEN_LED_PIN,
    LOW
  );

  digitalWrite(
    ORANGE_LED_PIN,
    HIGH
  );


  Serial.println(
    "RECORDING PAUSED"
  );
}


// =====================================================
// RESUME
// =====================================================

void resumeRecording()
{
  if (
    !recording ||
    !paused
  )
    return;


  paused = false;


  digitalWrite(
    GREEN_LED_PIN,
    HIGH
  );

  digitalWrite(
    ORANGE_LED_PIN,
    LOW
  );


  Serial.println(
    "RECORDING RESUMED"
  );
}


// =====================================================
// RECORD AUDIO
// =====================================================

void recordAudio()
{
  static int32_t samples[1024];

  static int16_t output[1024];

  size_t bytesRead = 0;


  i2s_read(
    I2S_PORT,
    samples,
    sizeof(samples),
    &bytesRead,
    portMAX_DELAY
  );


  if (
    bytesRead == 0
  )
    return;


  int sampleCount =
    bytesRead /
    sizeof(int32_t);


  for (
    int i = 0;
    i < sampleCount;
    i++
  )
  {
    output[i] =
      (int16_t)(
        samples[i] >> 14
      );
  }


  size_t bytesToWrite =
    sampleCount *
    sizeof(int16_t);


  size_t written =
    audioFile.write(
      (uint8_t *)output,
      bytesToWrite
    );


  audioDataSize += written;
}


// =====================================================
// STOP RECORDING
// =====================================================

void stopRecording()
{
  recording = false;

  paused = false;


  digitalWrite(
    GREEN_LED_PIN,
    LOW
  );

  digitalWrite(
    ORANGE_LED_PIN,
    LOW
  );


  Serial.println();
  Serial.println(
    "RECORDING STOPPED"
  );

  Serial.println(
    "Saving WAV file..."
  );


  if (!audioFile)
  {
    Serial.println(
      "ERROR: Audio file is not open!"
    );

    saveErrorLED();

    return;
  }


  audioFile.flush();


  Serial.print(
    "Audio data: "
  );

  Serial.print(
    audioDataSize
  );

  Serial.println(
    " bytes"
  );


  writeWavHeader(
    audioFile,
    audioDataSize
  );


  audioFile.flush();

  audioFile.close();


  delay(200);


  bool saveSuccessful =
    false;


  if (
    SD.exists(
      currentFileName
    )
  )
  {
    File verifyFile =
      SD.open(
        currentFileName,
        FILE_READ
      );


    if (verifyFile)
    {
      uint32_t savedSize =
        verifyFile.size();


      verifyFile.close();


      uint32_t expectedSize =
        audioDataSize + 44;


      Serial.println();
      Serial.println("==============================");

      Serial.print(
        "Expected size: "
      );

      Serial.println(
        expectedSize
      );


      Serial.print(
        "Actual size: "
      );

      Serial.println(
        savedSize
      );


      if (
        savedSize >= expectedSize
      )
      {
        saveSuccessful = true;
      }
    }
  }


  if (saveSuccessful)
  {
    Serial.println(
      "RECORDING SAVED SUCCESSFULLY!"
    );


    saveSuccessLED();

    return;
  }


  Serial.println(
    "ERROR: RECORDING SAVE FAILED!"
  );


  saveErrorLED();
}


// =====================================================
// SAVE SUCCESS
// =====================================================

void saveSuccessLED()
{
  for (
    int i = 0;
    i < 3;
    i++
  )
  {
    digitalWrite(
      GREEN_LED_PIN,
      HIGH
    );

    delay(300);

    digitalWrite(
      GREEN_LED_PIN,
      LOW
    );

    delay(300);
  }
}


// =====================================================
// SAVE ERROR
// =====================================================

void saveErrorLED()
{
  for (
    int i = 0;
    i < 6;
    i++
  )
  {
    digitalWrite(
      ORANGE_LED_PIN,
      HIGH
    );

    delay(150);

    digitalWrite(
      ORANGE_LED_PIN,
      LOW
    );

    delay(150);
  }
}


// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println("       LECTURE MATE");
  Serial.println("================================");

  pinMode(GREEN_LED_PIN,OUTPUT);
  pinMode(RED_LED_PIN,OUTPUT);
  pinMode(ORANGE_LED_PIN,OUTPUT);

  digitalWrite(GREEN_LED_PIN,LOW);
  digitalWrite(ORANGE_LED_PIN,LOW);
  digitalWrite(RED_LED_PIN,HIGH);

  pinMode(WIFI_RED_LED,OUTPUT);
  pinMode(WIFI_GREEN_LED,OUTPUT);
  wifiLEDsOff();

  pinMode(START_STOP_BUTTON_PIN,INPUT_PULLUP);
  pinMode(PAUSE_BUTTON_PIN,INPUT_PULLUP);
  pinMode(WIFI_BUTTON_PIN,INPUT_PULLUP);

  loadSavedNetworks();
  setupI2S();

  SPI.begin(SD_SCK,SD_MISO,SD_MOSI,SD_CS);

  if(!SD.begin(SD_CS)) {
    Serial.println("ERROR: SD CARD FAILED!");
    while(true) {
      digitalWrite(ORANGE_LED_PIN,HIGH);
      delay(250);
      digitalWrite(ORANGE_LED_PIN,LOW);
      delay(250);
    }
  }

  Serial.println("SD card initialized.");

  WiFi.mode(WIFI_OFF);
  wifiState=WIFI_OFF_STATE;

  Serial.println("READY!");
  Serial.println("Start/Stop = Recording");
  Serial.println("Pause = Pause/Resume");
  Serial.println("D15 button = Wi-Fi portal ON/OFF");
  Serial.println("Wi-Fi starts OFF. No automatic connection.");
  Serial.println("Portal timeout = 5 minutes unless router is connected.");
  Serial.println("Cloud backend URL: " + String(LECTURE_MATE_BASE_URL));
  Serial.println("Select a saved WAV in /recordings to upload it to Convert.");
  Serial.println("Device ID for linked accounts: "+String(ESP32_DEVICE_ID));
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  static bool lastStartStopState=HIGH;
  static bool lastPauseState=HIGH;

  handleWiFiButton();

  if(portalActive) {
    dnsServer.processNextRequest();
    server.handleClient();
    if(wifiState==WIFI_SETUP_STATE) wifiBlinkBoth();
    // Only time out the setup portal if the router connection has NOT
    // succeeded. Once connected, keep Wi-Fi/AP+STA running until the user
    // presses the D15 button. This prevents the 5-minute setup timer from
    // unexpectedly switching off an established connection.
    if (wifiState != WIFI_CONNECTED_STATE &&
        millis() - apStartTime >= SETUP_AP_TIMEOUT) {
      Serial.println("Wi-Fi setup portal timeout (router was not connected).");
      turnWiFiOff();
    }
  }

  updateWiFiConnection();
  updateWebsiteConnection();

  if (WiFi.status()==WL_CONNECTED && !recording && !queueUploadInProgress) {
    unsigned long nowMs=millis();
    if(lastBindingSync==0 || nowMs-lastBindingSync>=HEARTBEAT_SYNC_INTERVAL){lastBindingSync=nowMs;refreshDeviceBinding();}
    if(lastCommandPoll==0 || nowMs-lastCommandPoll>=COMMAND_POLL_INTERVAL){lastCommandPoll=nowMs;pollUploadQueue();}
    if(activePairingCode.length() && getPairingRemaining()=="0"){activePairingCode="";pairingStatusMessage="Pairing code expired. Generate a new code when ready.";}
  }

  bool startStopState=digitalRead(START_STOP_BUTTON_PIN);

  if(lastStartStopState==HIGH && startStopState==LOW) {
    delay(50);

    if(digitalRead(START_STOP_BUTTON_PIN)==LOW) {
      if(!recording) startRecording();
      else stopRecording();

      while(digitalRead(START_STOP_BUTTON_PIN)==LOW) delay(10);
    }
  }

  lastStartStopState=startStopState;

  bool pauseButtonState=digitalRead(PAUSE_BUTTON_PIN);

  if(lastPauseState==HIGH && pauseButtonState==LOW) {
    delay(50);

    if(digitalRead(PAUSE_BUTTON_PIN)==LOW) {
      if(recording) {
        if(!paused) pauseRecording();
        else resumeRecording();
      }

      while(digitalRead(PAUSE_BUTTON_PIN)==LOW) delay(10);
    }
  }

  lastPauseState=pauseButtonState;

  if(recording && !paused) recordAudio();
}
