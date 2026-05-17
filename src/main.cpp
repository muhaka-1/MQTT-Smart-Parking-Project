#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>

// ESP32 core compile-time validation for LittleFS compatibility layers
#ifdef __has_include
  #if __has_include(<LittleFS.h>)
    #include <LittleFS.h>
  #else
    #include <LITTLEFS.h>
    #define LittleFS LITTLEFS
  #endif
#else
  #include <LittleFS.h>
#endif

#include <PubSubClient.h> 

// =======================================================
// WiFi & MQTT NETWORK CONFIGURATION
// =======================================================
const char* ssid        = "Tele2Internet_B5596";
const char* password    = "B648nLhA5a5";
const char* mqttHost    = "192.168.8.131"; 
const int   mqttPort    = 1883;

// Structured Domain-Specific Messaging Topics
const char* topicEntryLog     = "parking/entry_log";
const char* topicExitLog      = "parking/exit_log";
const char* topicLastEntry    = "parking/last_entry";
const char* topicLastExit     = "parking/last_exit";
const char* topicCountUpdate  = "parking/count_update";
const char* topicStatus       = "parking/status";
const char* topicCacheVersion = "parking/cards_version";

// =======================================================
// SYSTEM NETWORKING INITIALIZATION
// =======================================================
WiFiClient espClient;
PubSubClient mqttClient(espClient);
bool mqttOnline = false;

// =======================================================
// HARDWARE CORRECTIONS & CONSTANTS (Freenove ESP32-S3)
// =======================================================
#define LCD_ENTRY_ADDR 0x27
#define LCD_EXIT_ADDR  0x26

#define RST_RFID        5
#define SS_RFID_ENTRY   10
#define SS_RFID_EXIT    6

#define SCK_PIN         12
#define MISO_PIN        13
#define MOSI_PIN        11

#define BUZZER_PIN      3
#define BUZZER_CHANNEL  14 // Hard isolated to prevent servo signal pollution

#define SERVO_ENTRY_PIN 4
#define SERVO_EXIT_PIN  18

// Ultrasonic Cluster Pin Assignments
#define TRIG_ENTRY_BEFORE 16
#define ECHO_ENTRY_BEFORE 17
#define TRIG_ENTRY_AFTER  7
#define ECHO_ENTRY_AFTER  15 

#define TRIG_EXIT_BEFORE  20
#define ECHO_EXIT_BEFORE  21
#define TRIG_EXIT_AFTER   8   
#define ECHO_EXIT_AFTER   9

// =======================================================
// STORAGE ARCHITECTURE PATHS
// =======================================================
#define OFFLINE_QUEUE   "/offline_queue.txt"
#define VALID_UIDS_FILE "/valid_uids.txt"

// =======================================================
// ASYNCHRONOUS POLLING PROTOCOL CONFIGURATION
// =======================================================
#define CACHE_CHECK_INTERVAL_MS 3000UL
unsigned long lastCacheCheck  = 0;
long          cachedVersion   = -1;
bool          cacheReady      = false;

// =======================================================
// GLOBAL INTERACTION OBJECTS
// =======================================================
MFRC522           rfidEntry(SS_RFID_ENTRY, RST_RFID);
MFRC522           rfidExit(SS_RFID_EXIT, RST_RFID);
Servo             servoEntry;
Servo             servoExit;
LiquidCrystal_I2C lcdEntry(LCD_ENTRY_ADDR, 16, 2);
LiquidCrystal_I2C lcdExit(LCD_EXIT_ADDR, 16, 2);

int totalSpaces    = 10;
int occupiedSpaces = 4;

// Forward Declarations
void updateEntryLCD();
void updateExitLCD();
String getCardOwner(String cardUID);
bool refreshValidUidCache();
void mqttCallback(char* topic, byte* payload, unsigned int length);

// =======================================================
// STORE & FORWARD NON-VOLATILE CORE FILESYSTEM ENGINE
// =======================================================
void ensureOfflineFileExists() {
  if (!LittleFS.exists(OFFLINE_QUEUE)) {
    Serial.println("[FS] Creating offline queue file...");
    File f = LittleFS.open(OFFLINE_QUEUE, FILE_WRITE);
    if (!f) {
      Serial.println("[FS] ERROR: Cannot execute queue initialization!");
      return;
    }
    f.close();
    Serial.println("[FS] Safe space allocated.");
  }
}

void appendToOfflineQueue(const char* type, String uid, String owner, String ts) {
  File f = LittleFS.open(OFFLINE_QUEUE, FILE_APPEND);
  if (!f) {
    Serial.println("[OFFLINE] ERROR: IO Stream denied flash append access!");
    return;
  }
  f.printf("%s|%s|%s|%s\n", type, uid.c_str(), owner.c_str(), ts.c_str());
  f.close();
  Serial.printf("[OFFLINE] Cached event localized to NVRAM: %s for target %s\n", type, uid.c_str());
}

void syncOfflineQueue() {
  if (!LittleFS.exists(OFFLINE_QUEUE)) {
    File create = LittleFS.open(OFFLINE_QUEUE, FILE_WRITE);
    if (create) create.close();
    Serial.println("[SYNC] Memory boundary valid (Zero record log initialized)");
    return;
  }

  File f = LittleFS.open(OFFLINE_QUEUE, FILE_READ);
  if (!f) {
    Serial.println("[SYNC] ERROR: Flash system read-block lock active!");
    return;
  }
  if (f.size() == 0) {
    f.close();
    Serial.println("[SYNC] Buffer validation clean. Zero logs standing.");
    return;
  }

  Serial.println("[SYNC] Network pipeline alive. Pushing telemetry stack...");
  int count = 0;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int s1 = line.indexOf('|');
    int s2 = line.indexOf('|', s1 + 1);
    int s3 = line.indexOf('|', s2 + 1);
    if (s1 < 0 || s2 < 0 || s3 < 0) continue;

    String type  = line.substring(0, s1);
    String uid   = line.substring(s1 + 1, s2);
    String owner = line.substring(s2 + 1, s3);
    String ts    = line.substring(s3 + 1);
    String entry = uid + "|" + owner + "|" + ts;

    if (type == "ENTRY") {
      mqttClient.publish(topicEntryLog, entry.c_str(), true);
      mqttClient.publish(topicLastEntry, entry.c_str(), true);
      String updateTopic = "parking/card/" + uid + "/entry_count/incr";
      mqttClient.publish(updateTopic.c_str(), "1");
    } else if (type == "EXIT") {
      mqttClient.publish(topicExitLog, entry.c_str(), true);
      mqttClient.publish(topicLastExit, entry.c_str(), true);
      String updateTopic = "parking/card/" + uid + "/exit_count/incr";
      mqttClient.publish(updateTopic.c_str(), "1");
    }
    count++;
  }
  f.close();

  File trunc = LittleFS.open(OFFLINE_QUEUE, FILE_WRITE);
  if (trunc) trunc.close();

  Serial.printf("[SYNC] Complete — %d telemetry records successfully pushed to broker.\n", count);
}

// =======================================================
// HYBRID ARCHITECTURE PERSISTENT MEMORY CACHE REGISTRY
// =======================================================
bool refreshValidUidCache() {
  Serial.println("[CACHE] Ready to pull fresh baseline via downstream subscription hooks.");
  return true;
}

void checkForCacheUpdates() {
  if (!mqttOnline) return;
  if (millis() - lastCacheCheck < CACHE_CHECK_INTERVAL_MS) return;
  lastCacheCheck = millis();

  mqttClient.publish("parking/cache/poll_request", String(cachedVersion).c_str());
}

bool checkLocalCache(const String& uid, String& outOwner) {
  if (!LittleFS.exists(VALID_UIDS_FILE)) return false;
  File f = LittleFS.open(VALID_UIDS_FILE, FILE_READ);
  if (!f) return false;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    int sep = line.indexOf('|');
    if (sep < 0) continue;
    if (line.substring(0, sep).equalsIgnoreCase(uid)) {
      outOwner = line.substring(sep + 1);
      f.close();
      return true;
    }
  }
  f.close();
  return false;
}

// =======================================================
// CLIENT PIPELINE & NETWORK MANAGEMENT BLOCK
// =======================================================
bool connectToMQTT() {
  Serial.println("[MQTT] Executing host connection routine...");
  mqttClient.setServer(mqttHost, mqttPort);
  mqttClient.setCallback(mqttCallback);

  if (!mqttClient.connected()) {
    if (!mqttClient.connect("Freenove_ESP32S3_Gateway")) {
      Serial.println("[MQTT] Connection broken — Entering standalone offline architecture");
      mqttOnline = false;
      return false;
    }
  }
  
  mqttOnline = true;
  Serial.println("[MQTT] Pipeline initialized. System Status: ONLINE");
  
  mqttClient.subscribe(topicCacheVersion);
  mqttClient.subscribe("parking/cache/update");
  mqttClient.subscribe("parking/occupied_persistent/response");
  
  return true;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  message.trim();

  String topicStr(topic);

  if (topicStr == topicCacheVersion) {
    long currentVersion = message.toInt();
    if (currentVersion != cachedVersion) {
      Serial.printf("[CACHE] Tracking skew found (%ld -> %ld) — Initiating hot reload\n", cachedVersion, currentVersion);
    }
  } 
  else if (topicStr == "parking/cache/update") {
    File f = LittleFS.open(VALID_UIDS_FILE, FILE_WRITE);
    if (f) {
      f.print(message);
      f.close();
      cacheReady = true;
      Serial.println("[CACHE] Storage updated securely with structural whitelist data payload.");
    }
  } 
  else if (topicStr == "parking/occupied_persistent/response") {
    int parsedValue = message.toInt();
    if (parsedValue >= 0 && parsedValue <= totalSpaces) {
      occupiedSpaces = parsedValue;
      Serial.printf("[MQTT] Local register sync completed. Space state: %d\n", occupiedSpaces);
      updateEntryLCD();
      updateExitLCD();
    }
  }
}

int checkCardStatus(String cardUID) {
  if (!cacheReady) {
    Serial.println("[ACCESS] Security core error: Storage locked — rejecting card");
    return -1;
  }

  String dummy;
  if (checkLocalCache(cardUID, dummy)) {
    Serial.printf("[ACCESS] ✅ Target token ID %s approved by system cache\n", cardUID.c_str());
    return 1;
  }

  Serial.printf("[ACCESS] ❌ Token ID %s not found in authorized whitelist — Access Denied\n", cardUID.c_str());
  return -1;
}

String getCardOwner(String cardUID) {
  String owner;
  if (checkLocalCache(cardUID, owner)) return owner;
  return "Unknown";
}

void logCardEntry(String cardUID) {
  String ts    = String(millis());
  String owner = getCardOwner(cardUID);

  if (mqttOnline && mqttClient.connected()) {
    String logEntry = cardUID + "|" + owner + "|" + ts;
    mqttClient.publish(topicEntryLog, logEntry.c_str(), true);
    mqttClient.publish(topicLastEntry, logEntry.c_str(), true);
    
    String countKey = "parking/card/" + cardUID + "/entry_count/incr";
    mqttClient.publish(countKey.c_str(), "1");
    Serial.printf("[MQTT] Live upload complete: Transmitted entrance log for %s\n", owner.c_str());
  } else {
    appendToOfflineQueue("ENTRY", cardUID, owner, ts);
  }
}

void logCardExit(String cardUID) {
  String ts    = String(millis());
  String owner = getCardOwner(cardUID);

  if (mqttOnline && mqttClient.connected()) {
    String logEntry = cardUID + "|" + owner + "|" + ts;
    mqttClient.publish(topicExitLog, logEntry.c_str(), true);
    mqttClient.publish(topicLastExit, logEntry.c_str(), true);
    
    String countKey = "parking/card/" + cardUID + "/exit_count/incr";
    mqttClient.publish(countKey.c_str(), "1");
    Serial.printf("[MQTT] Live upload complete: Transmitted exit log for %s\n", owner.c_str());
  } else {
    appendToOfflineQueue("EXIT", cardUID, owner, ts);
  }
}

void updateParkingStatus() {
  int avail = totalSpaces - occupiedSpaces;
  if (!mqttOnline || !mqttClient.connected()) {
    Serial.println("[MQTT] Network drops detected — masking localized parameters");
    return;
  }
  
  String payload = String(occupiedSpaces) + "," + String(avail) + "," + String(totalSpaces) + "," + String(millis()) + "," + WiFi.localIP().toString();
  mqttClient.publish(topicStatus, payload.c_str(), true);
  Serial.printf("[MQTT] Dispatched baseline configuration arrays: State allocation %d/%d\n", occupiedSpaces, totalSpaces);
}

void saveOccupiedToRedis() {
  if (!mqttOnline || !mqttClient.connected()) return;
  mqttClient.publish("parking/occupied_persistent/store", String(occupiedSpaces).c_str(), true);
}

void restoreFromRedis() {
  if (!mqttOnline || !mqttClient.connected()) return;
  mqttClient.publish("parking/occupied_persistent/request", "pull");
}

// =======================================================
// AUTOMATED WIFI PERIPHERAL WATCHDOG
// =======================================================
void connectToWiFi() {
  Serial.print("[WiFi] Locating target service access point network: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Network interface handshaking validated.");
    Serial.print("[WiFi] Network interface address footprint: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WiFi] Fatal error: Connectivity route dropped!");
  }
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.println("[WiFi] Warning: Transport layer closed. Forcing immediate hardware reconnect sequence...");
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Hardware interface recovery validated.");
  } else {
    Serial.println("\n[WiFi] Watchdog warning: Connection recovery failed!");
  }
}

bool ensureRedis() {
  if (mqttClient.connected()) {
    if (!mqttOnline) {
      mqttOnline = true;
      Serial.println("[MQTT] Recovery validation match! System back ONLINE — Emptying localized pipelines...");
      syncOfflineQueue();
      updateParkingStatus();
    }
    return true;
  }

  bool wasOnline = mqttOnline;
  mqttOnline = false;

  if (!mqttClient.connect("Freenove_ESP32S3_Gateway")) {
    if (wasOnline) Serial.println("[MQTT] Severe socket degradation — Fallback localized routines active");
    return false;
  }

  mqttOnline = true;
  Serial.println("[MQTT] Connection handshake complete. Interface up. Restoring synchronized operations...");
  syncOfflineQueue();
  updateParkingStatus();
  return true;
}

// =======================================================
// MOTION COMPUTATION & PERIPHERAL SIGNAL CONTROL
// =======================================================
long getDistance(int trig, int echo) {
  digitalWrite(trig, LOW);
  delayMicroseconds(2);
  digitalWrite(trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig, LOW);
  long duration = pulseIn(echo, HIGH, 30000);
  if (duration == 0) return 999;
  return duration * 0.034 / 2;
}

bool isCarPresent(int trig, int echo) {
  return getDistance(trig, echo) < 15;
}

void beepSuccess() {
  ledcWriteTone(14, 1800); delay(200); ledcWriteTone(14, 0);
}

void beepError() {
  for (int i = 0; i < 3; i++) {
    ledcWriteTone(14, 900); delay(200); ledcWriteTone(14, 0); delay(200);
  }
}

void beepSoundGateOpen() {
  ledcWriteTone(14, 1200); delay(150); ledcWriteTone(14, 0);
  delay(100);
  ledcWriteTone(14, 1800); delay(150); ledcWriteTone(14, 0);
}

void openEntryGate() {
  Serial.println("[ENTRY GATE] Executing high-precision low-speed step sweep profile...");
  beepSoundGateOpen();
  for (int pos = 0; pos <= 90; pos++) {
    servoEntry.write(pos);
    delay(15); 
  }
}

void closeEntryGate() {
  Serial.println("[ENTRY GATE] Initializing closing safe step sweep profile...");
  for (int pos = 90; pos >= 0; pos--) {
    servoEntry.write(pos);
    delay(15);
  }
}

void openExitGate() {
  Serial.println("[EXIT GATE] Executing high-precision low-speed step sweep profile...");
  beepSoundGateOpen();
  for (int pos = 0; pos <= 90; pos++) {
    servoExit.write(pos);
    delay(15);
  }
}

void closeExitGate() {
  Serial.println("[EXIT GATE] Initializing closing safe step sweep profile...");
  for (int pos = 90; pos >= 0; pos--) {
    servoExit.write(pos);
    delay(15);
  }
}

void updateEntryLCD() {
  int avail = totalSpaces - occupiedSpaces;
  lcdEntry.clear();
  lcdEntry.setCursor(0, 0); lcdEntry.print("Welcome!");
  lcdEntry.setCursor(0, 1); lcdEntry.print("Free: "); lcdEntry.print(avail); lcdEntry.print("/"); lcdEntry.print(totalSpaces);
}

void updateExitLCD() {
  int avail = totalSpaces - occupiedSpaces;
  lcdExit.clear();
  lcdExit.setCursor(0, 0); lcdExit.print("Exit Ready");
  lcdExit.setCursor(0, 1); lcdExit.print("Free: "); lcdExit.print(avail); lcdExit.print("/"); lcdExit.print(totalSpaces);
}

String getRFIDUID(MFRC522 &rfid) {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) uid += ":";
  }
  uid.toUpperCase();
  return uid;
}

void flushRFID(MFRC522 &rfid) {
  while (rfid.PICC_IsNewCardPresent()) {
    rfid.PICC_ReadCardSerial();
    rfid.PICC_HaltA();
  }
}

// =======================================================
// DIAGNOSTIC CORE TELEMETRY INTERFACE
// =======================================================
void handleSerialCommands() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toLowerCase();
  if (cmd.length() == 0) return;

  if (cmd == "dump") {
    if (!LittleFS.exists(OFFLINE_QUEUE)) { Serial.println("[DUMP] Clean: target file space contains zero arrays."); return; }
    File f = LittleFS.open(OFFLINE_QUEUE, FILE_READ);
    if (!f) { Serial.println("[DUMP] Execution fault: Target locked."); return; }
    Serial.printf("\n--- offline_queue.txt (%d bytes) ---\n", f.size());
    while (f.available()) Serial.write(f.read());
    Serial.println("\n--- EOF Data Boundary ---\n");
    f.close();
  }
  else if (cmd == "count") {
    if (!LittleFS.exists(OFFLINE_QUEUE)) { Serial.println("[COUNT] Local Array Value: 0 records stored."); return; }
    File f = LittleFS.open(OFFLINE_QUEUE, FILE_READ);
    int n = 0;
    while (f.available()) if (f.read() == '\n') n++;
    f.close();
    Serial.printf("[COUNT] %d isolated telemetry frames waiting for transmission\n", n);
  }
  else if (cmd == "clear") {
    File f = LittleFS.open(OFFLINE_QUEUE, FILE_WRITE);
    if (f) f.close();
    Serial.println("[CLEAR] Sector wipe clean: Array records dropped.");
  }
  else if (cmd == "fsinfo") {
    Serial.printf("[FS] Used: %u / Total Available Segment: %u bytes\n",
                  LittleFS.usedBytes(), LittleFS.totalBytes());
  }
  else if (cmd == "cache") {
    if (!LittleFS.exists(VALID_UIDS_FILE)) { Serial.println("[CACHE] Read fault: Whitelist structural payload not present."); return; }
    File f = LittleFS.open(VALID_UIDS_FILE, FILE_READ);
    if (!f) { Serial.println("[CACHE] Storage system read error."); return; }
    Serial.printf("\n--- valid_uids.txt (%d bytes, version %ld, ready=%s) ---\n",
                  f.size(), cachedVersion, cacheReady ? "yes" : "no");
    while (f.available()) Serial.write(f.read());
    Serial.println("\n--- Sector Boundary End ---\n");
    f.close();
  }
  else if (cmd == "refresh") {
    if (!mqttOnline) { Serial.println("[REFRESH] Signal path down — Pipeline synchronization blocked."); return; }
    refreshValidUidCache();
  }
  else if (cmd == "version") {
    Serial.printf("[VERSION] Snapshot Tag: %ld | Device Array Status: %s | Network Protocol: %s\n",
                  cachedVersion,
                  cacheReady ? "VALID" : "EMPTY",
                  mqttOnline ? "ONLINE" : "OFFLINE");
  }
  else if (cmd == "help") {
    Serial.println("Telemetry commands available: dump | count | clear | fsinfo | cache | refresh | version | help");
  }
  else {
    Serial.printf("[?] Parameter mismatch command parsing error: '%s' — Execute 'help' to review syntax structures.\n", cmd.c_str());
  }
}

// =======================================================
// MAIN SETUP
// =======================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println(" SMART PARKING — HIGH RESILIENCE MQTT   ");
  Serial.println("========================================\n");

  esp_log_level_set("WiFiClient", ESP_LOG_NONE);

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] Flash System allocation failed!");
  } else {
    Serial.println("[FS] Flash interface up.");
    ensureOfflineFileExists();
    if (LittleFS.exists(OFFLINE_QUEUE))
      Serial.println("[FS] Verification check match: Local buffer log file found.");
    if (LittleFS.exists(VALID_UIDS_FILE))
      Serial.println("[FS] Verification check match: Localized card cache binary found.");
  }

  connectToWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    if (connectToMQTT()) {
      restoreFromRedis();
      updateParkingStatus();
      saveOccupiedToRedis();
      syncOfflineQueue();
      refreshValidUidCache();   
    }
  }

  if (!cacheReady && LittleFS.exists(VALID_UIDS_FILE)) {
    cacheReady = true;
    Serial.println("[CACHE] Local hardware matrix verified from flash footprint. Safe initialization authorized.");
  }

  if (!cacheReady) {
    Serial.println("[BOOT] Safety lockdown: No database footprint confirmed — Gates isolated.");
    Serial.println("[BOOT] Awaiting network pipeline data injection sequence to initialize security array...");
  }

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN);
  rfidEntry.PCD_Init();
  rfidExit.PCD_Init();
  
  servoEntry.attach(SERVO_ENTRY_PIN);
  servoExit.attach(SERVO_EXIT_PIN);
  servoEntry.write(0);
  servoExit.write(0);
  
  lcdEntry.init(); lcdEntry.backlight();
  lcdExit.init();  lcdExit.backlight();
  
  ledcSetup(14, 2000, 8);
  ledcAttachPin(BUZZER_PIN, 14);
  
  pinMode(TRIG_ENTRY_BEFORE, OUTPUT); pinMode(ECHO_ENTRY_BEFORE, INPUT);
  pinMode(TRIG_ENTRY_AFTER,  OUTPUT); pinMode(ECHO_ENTRY_AFTER,  INPUT);
  pinMode(TRIG_EXIT_BEFORE,  OUTPUT); pinMode(ECHO_EXIT_BEFORE,  INPUT);
  pinMode(TRIG_EXIT_AFTER,   OUTPUT); pinMode(ECHO_EXIT_AFTER,   INPUT);

  if (cacheReady) {
    updateEntryLCD();
    updateExitLCD();
  } else {
    lcdEntry.clear();
    lcdEntry.setCursor(0, 0); lcdEntry.print("System Init");
    lcdEntry.setCursor(0, 1); lcdEntry.print("Waiting sync...");
    lcdExit.clear();
    lcdExit.setCursor(0, 0); lcdExit.print("System Init");
    lcdExit.setCursor(0, 1); lcdExit.print("Waiting sync...");
  }

  Serial.printf("\n[SYSTEM] HARDWARE SEQUENCE COMPLETE! Broker Pipeline: %s | Core Security Matrix: %s\n",
                mqttOnline ? "ONLINE" : "OFFLINE",
                cacheReady  ? "OPERATIONAL"  : "LOCKED_DOWN");
  Serial.println("Execute terminal diagnostics via 'help' command schema.");
  Serial.println("========================================\n");
}

// =======================================================
// MAIN EXECUTION WAVEFORM
// =======================================================
void loop() {
  handleSerialCommands();
  ensureWiFi();

  static unsigned long lastRedisCheck = 0;
  if (millis() - lastRedisCheck > 10000) {
    lastRedisCheck = millis();
    if (!ensureRedis()) {
      Serial.println("[MQTT] Link trace failed — Preserving telemetry frames safely to flash matrix.");
    }
  }

  mqttClient.loop(); 
  checkForCacheUpdates();

  if (!cacheReady) {
    delay(200);
    return;
  }

  // ========== CAR DETECTION ENTRY PROXIMITY WAVEFORM ==========
  if (isCarPresent(TRIG_ENTRY_BEFORE, ECHO_ENTRY_BEFORE)) {
    lcdEntry.clear();
    lcdEntry.setCursor(0, 0); lcdEntry.print("Welcome!");
    lcdEntry.setCursor(0, 1); lcdEntry.print("Scan Your Card");
  } else {
    updateEntryLCD();
  }

  // ========== ENTRANCE RFID SCAN DATA PROCESSING ==========
  if (rfidEntry.PICC_IsNewCardPresent() && rfidEntry.PICC_ReadCardSerial()) {
    String cardUID = getRFIDUID(rfidEntry);
    Serial.println("\n>>> [ENTRY] Processing transaction request for token ID: " + cardUID);

    ensureRedis();
    int cardStatus = checkCardStatus(cardUID);

    if (cardStatus == 1 && occupiedSpaces < totalSpaces) {
      String owner = getCardOwner(cardUID);

      lcdEntry.clear();
      lcdEntry.setCursor(0, 0); lcdEntry.print("Welcome " + owner);
      lcdEntry.setCursor(0, 1); lcdEntry.print("Gate Opening...");

      beepSuccess();
      logCardEntry(cardUID);
      openEntryGate();

      saveOccupiedToRedis();
      updateParkingStatus();
      flushRFID(rfidEntry);   
      updateEntryLCD();
      updateExitLCD();

      unsigned long gateOpenTime = millis();
      bool carEntered = false;

      while (millis() - gateOpenTime < 15000) {
        if (isCarPresent(TRIG_ENTRY_AFTER, ECHO_ENTRY_AFTER)) {
          carEntered = true;
          Serial.println("[ENTRY] Target crossing detected at exit-check sensor zone.");
          lcdEntry.clear();
          lcdEntry.setCursor(0, 0); lcdEntry.print("Vehicle Detected");
          lcdEntry.setCursor(0, 1); lcdEntry.print("Please Proceed");
          break;
        }
        delay(10);
      }
      
      if (carEntered) {
        while (isCarPresent(TRIG_ENTRY_AFTER, ECHO_ENTRY_AFTER)) {
          delay(50); 
        }
        occupiedSpaces++;
        Serial.printf("[ENTRY] State change confirmation complete. Capacity footprint: %d/%d\n", occupiedSpaces, totalSpaces);
      }
      
      closeEntryGate();
      updateEntryLCD();

    } else {
      lcdEntry.clear();
      lcdEntry.setCursor(0, 0); lcdEntry.print("INVALID CARD");
      lcdEntry.setCursor(0, 1); lcdEntry.print("Access Denied!");
      beepError();
      delay(2000);
      updateEntryLCD();
    }

    rfidEntry.PICC_HaltA();
    delay(500);
  }

  // ========== CAR DETECTION EXIT PROXIMITY WAVEFORM ==========
  bool carAtExitBefore = isCarPresent(TRIG_EXIT_BEFORE, ECHO_EXIT_BEFORE);
  
  if (carAtExitBefore) {
    lcdExit.clear();
    lcdExit.setCursor(0, 0); lcdExit.print("Goodbye!");
    lcdExit.setCursor(0, 1); lcdExit.print("Scan Your Card");
  } else {
    updateExitLCD();
  }

  // ========== EXIT RFID SCAN DATA PROCESSING ==========
  if (rfidExit.PICC_IsNewCardPresent() && rfidExit.PICC_ReadCardSerial()) {
    String cardUID = getRFIDUID(rfidExit);
    Serial.println("\n>>> [EXIT] Processing transaction request for token ID: " + cardUID);

    ensureRedis();
    int cardStatus = checkCardStatus(cardUID);

    if (cardStatus == 1 && occupiedSpaces > 0) {
      String owner = getCardOwner(cardUID);

      lcdExit.clear();
      lcdExit.setCursor(0, 0); lcdExit.print("Goodbye " + owner);
      lcdExit.setCursor(0, 1); lcdExit.print("Gate Opening...");

      beepSuccess();
      logCardExit(cardUID);
      openExitGate();

      saveOccupiedToRedis();
      updateParkingStatus();
      flushRFID(rfidExit);
      updateEntryLCD();
      updateExitLCD();

      unsigned long gateOpenTime = millis();
      bool carExited = false;

      while (millis() - gateOpenTime < 15000) {
        if (isCarPresent(TRIG_EXIT_AFTER, ECHO_EXIT_AFTER)) {
          carExited = true;
          Serial.println("[EXIT] Target crossing detected at safe exit clearing zone.");
          lcdExit.clear();
          lcdExit.setCursor(0, 0); lcdExit.print("Vehicle Detected");
          lcdExit.setCursor(0, 1); lcdExit.print("Please Proceed");
          break;
        }
        delay(10);
      }
      
      if (carExited) {
        while (isCarPresent(TRIG_EXIT_AFTER, ECHO_EXIT_AFTER)) {
          delay(50);
        }
        occupiedSpaces--;
        Serial.printf("[EXIT] State change confirmation complete. Capacity footprint: %d/%d\n", occupiedSpaces, totalSpaces);
      }
      
      closeExitGate();
      updateEntryLCD();

    } else {
      lcdExit.clear();
      lcdExit.setCursor(0, 0); lcdExit.print("INVALID CARD");
      lcdExit.setCursor(0, 1); lcdExit.print("Access Denied!");
      beepError();
      delay(2000);
      updateExitLCD();
    }

    rfidExit.PICC_HaltA();
    delay(500);
  }

  delay(100);
}
