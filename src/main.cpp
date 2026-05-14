

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
// ESP32 core 2.x has LittleFS built-in; core 1.x needs LITTLEFS.h + alias
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
#include <Redis.h>




// =======================================================
// WiFi CONFIGURATION
// =======================================================
const char* ssid = "Tele2Internet_B5596";
const char* password = "B648nLhA5a5";
const char* redisHost  = "192.168.8.115";
const int   redisPort  = 6379;


// =======================================================
// ACCESS POLICY
// Hybrid cache: only cards present in the local cache may
// enter — works the same online or offline. Strangers are
// always denied. The cache is refreshed automatically when
// the admin updates Redis (see CACHE_VERSION_KEY below).
// =======================================================


// =======================================================
// REDIS CLIENT
// =======================================================
WiFiClient redisClient;
Redis      redis(redisClient);
bool       redisOnline = false;


// =======================================================
// PIN DEFINITIONS
// =======================================================
#// --- Pin Configuration (Freenove ESP32-S3) ---
#define LCD_ENTRY_ADDR        1
#define LCD_EXIT_ADDR         2
#define RST_PIN        5
#define SS_ENTRY_PIN   10
#define SS_EXIT_PIN    6
#define BUZZER_PIN      3
#define SERVO_ENTRY_PIN 4
#define SERVO_EXIT_PIN  18

// Hardware SPI pins for S3
#define SCK_PIN         12
#define MISO_PIN        13
#define MOSI_PIN        11

// Ultrasonic - Entry
#define TRIG_ENTRY_BEFORE  47
#define ECHO_ENTRY_BEFORE  48
#define TRIG_ENTRY_AFTER   21
#define ECHO_ENTRY_AFTER   20

// Ultrasonic - Exit
#define TRIG_EXIT_BEFORE   7
#define ECHO_EXIT_BEFORE   15
#define TRIG_EXIT_AFTER    16
#define ECHO_EXIT_AFTER    17

// =======================================================
// LITTLEFS FILE PATHS
// =======================================================
#define OFFLINE_QUEUE     "/offline_queue.txt"
#define VALID_UIDS_FILE   "/valid_uids.txt"


// =======================================================
// CACHE / SYNC PROTOCOL
// Admin contract: every change to authorized cards must
// also INCR parking:cards_version atomically. ESP32 polls
// this counter every CACHE_CHECK_INTERVAL ms — when it
// changes, the local cache is refreshed from Redis.
// =======================================================
#define CACHE_VERSION_KEY        "parking:cards_version"
#define CACHE_CHECK_INTERVAL_MS  3000UL


unsigned long lastCacheCheck = 0;
long          cachedVersion  = -1;     // last version we synced to LittleFS
bool          cacheReady     = false;  // never authorize anyone until true


// =======================================================
// GLOBAL OBJECTS
// =======================================================
MFRC522           rfidEntry(SS_ENTRY_PIN, RST_PIN);
MFRC522           rfidExit(SS_EXIT_PIN, RST_PIN);
Servo             servoEntry;
Servo             servoExit;
LiquidCrystal_I2C lcdEntry(LCD_ENTRY_ADDR, 16, 2);
LiquidCrystal_I2C lcdExit(LCD_EXIT_ADDR, 16, 2);


int totalSpaces    = 10;
int occupiedSpaces = 4;


// =======================================================
// FORWARD DECLARATIONS
// =======================================================
void   updateEntryLCD();
void   updateExitLCD();
String getCardOwner(String cardUID);
bool   refreshValidUidCache();


// =======================================================
// LITTLEFS — OFFLINE LOG QUEUE
// Format per line: TYPE|UID|OWNER|TIMESTAMP
// =======================================================


void ensureOfflineFileExists() {
  if (!LittleFS.exists(OFFLINE_QUEUE)) {
    Serial.println("[FS] Creating offline queue file...");
    File f = LittleFS.open(OFFLINE_QUEUE, FILE_WRITE);
    if (!f) {
      Serial.println("[FS] ERROR: Cannot create file!");
      return;
    }
    f.close();
    Serial.println("[FS] File created");
  }
}


void appendToOfflineQueue(const char* type, String uid, String owner, String ts) {
  File f = LittleFS.open(OFFLINE_QUEUE, FILE_APPEND);
  if (!f) {
    Serial.println("[OFFLINE] Cannot open queue file!");
    return;
  }
  f.printf("%s|%s|%s|%s\n", type, uid.c_str(), owner.c_str(), ts.c_str());
  f.close();
  Serial.printf("[OFFLINE] Saved %s event for %s\n", type, uid.c_str());
}


void syncOfflineQueue() {
  if (!LittleFS.exists(OFFLINE_QUEUE)) {
    File create = LittleFS.open(OFFLINE_QUEUE, FILE_WRITE);
    if (create) create.close();
    Serial.println("[SYNC] No offline file (placeholder created)");
    return;
  }


  File f = LittleFS.open(OFFLINE_QUEUE, FILE_READ);
  if (!f) {
    Serial.println("[SYNC] Cannot open queue file!");
    return;
  }
  if (f.size() == 0) {
    f.close();
    Serial.println("[SYNC] No pending logs");
    return;
  }


  Serial.println("[SYNC] Syncing offline logs...");
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
      redis.lpush("parking:entry_log", entry.c_str());
      redis.hset("parking:last_entry", "card",  uid.c_str());
      redis.hset("parking:last_entry", "owner", owner.c_str());
      redis.hset("parking:last_entry", "time",  ts.c_str());
      String ck = "card:" + uid + ":entry_count";
      String v  = redis.get(ck.c_str());
      redis.set(ck.c_str(), String((v.length() > 0 ? v.toInt() : 0) + 1).c_str());
    } else if (type == "EXIT") {
      redis.lpush("parking:exit_log", entry.c_str());
      redis.hset("parking:last_exit", "card",  uid.c_str());
      redis.hset("parking:last_exit", "owner", owner.c_str());
      redis.hset("parking:last_exit", "time",  ts.c_str());
      String ck = "card:" + uid + ":exit_count";
      String v  = redis.get(ck.c_str());
      redis.set(ck.c_str(), String((v.length() > 0 ? v.toInt() : 0) + 1).c_str());
    }
    count++;
  }
  f.close();


  // Truncate instead of remove — file persists, no future "does not exist" warnings
  File trunc = LittleFS.open(OFFLINE_QUEUE, FILE_WRITE);
  if (trunc) trunc.close();


  Serial.printf("[SYNC] Done — %d log(s) uploaded, queue cleared.\n", count);
}


// =======================================================
// VALID-UID CACHE — local copy of authorized cards
// Format per line: UID|OWNER
// Refreshed from Redis on connect and whenever the version
// counter changes. Used for ALL access decisions.
// =======================================================


bool refreshValidUidCache() {
  if (!redisOnline) return false;


  // Pull the comma-separated UID list (admin maintains this string)
  String csv = redis.get("parking:valid_cards_csv");


  // Read the version we are about to snapshot
  String v = redis.get(CACHE_VERSION_KEY);
  long newVersion = v.length() > 0 ? v.toInt() : 0;


  File f = LittleFS.open(VALID_UIDS_FILE, FILE_WRITE);
  if (!f) {
    Serial.println("[CACHE] Cannot open cache file for writing!");
    return false;
  }


  int written = 0;
  int start = 0;


  // Parse comma-separated UIDs
  while (start <= (int)csv.length()) {
    int comma = csv.indexOf(',', start);
    String uid = (comma < 0) ? csv.substring(start) : csv.substring(start, comma);
    uid.trim();


    if (uid.length() > 0) {
      String key   = "card:" + uid;
      String stat  = redis.hget(key.c_str(), "status");
      if (stat.toInt() == 1) {
        String owner = redis.hget(key.c_str(), "owner");
        if (owner.length() == 0) owner = "Unknown";
        f.printf("%s|%s\n", uid.c_str(), owner.c_str());
        written++;
      }
    }


    if (comma < 0) break;
    start = comma + 1;
  }
  f.close();


  cachedVersion = newVersion;
  cacheReady    = true;


  Serial.printf("[CACHE] Refreshed — %d valid UID(s) cached (version %ld)\n",
                written, cachedVersion);
  return true;
}


// Cheap version-poll. If the counter changed, refresh the cache.
void checkForCacheUpdates() {
  if (!redisOnline) return;
  if (millis() - lastCacheCheck < CACHE_CHECK_INTERVAL_MS) return;
  lastCacheCheck = millis();


  String v = redis.get(CACHE_VERSION_KEY);
  long currentVersion = v.length() > 0 ? v.toInt() : 0;


  if (currentVersion != cachedVersion) {
    Serial.printf("[CACHE] Version changed (%ld -> %ld) — refreshing\n",
                  cachedVersion, currentVersion);
    refreshValidUidCache();
  }
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
// REDIS FUNCTIONS
// =======================================================
bool connectToRedis() {
  Serial.println("[REDIS] Connecting...");
  if (!redisClient.connect(redisHost, redisPort)) {
    Serial.println("[REDIS] Connection failed — going OFFLINE");
    redisOnline = false;
    return false;
  }
  redisOnline = true;
  Serial.println("[REDIS] Connected! ONLINE");
  return true;
}


// incr is not in this library version — simulate with get+set
void redisIncr(const char* key) {
  String val = redis.get(key);
  int n = val.length() > 0 ? val.toInt() : 0;
  redis.set(key, String(n + 1).c_str());
}


// ==== ACCESS DECISION ====
// 1 = card authorized, -1 = denied
// Always validates against local cache (works online or offline).
// Refuses everything until cache has been initialized at least once.
int checkCardStatus(String cardUID) {
  if (!cacheReady) {
    Serial.println("[ACCESS] No cache available yet — denying ALL cards");
    return -1;
  }


  String dummy;
  if (checkLocalCache(cardUID, dummy)) {
    Serial.printf("[ACCESS] ✅ %s authorized (cache)\n", cardUID.c_str());  // ← add
    return 1;
  }


  if (redisOnline) {
    String key    = "card:" + cardUID;
    String status = redis.hget(key.c_str(), "status");
    if (status.toInt() == 1) {
      Serial.println("[ACCESS] Card valid in Redis but not in cache — refreshing");
      refreshValidUidCache();
      return 1;
    }
  }


  Serial.printf("[ACCESS] ❌ %s DENIED — not in authorized list\n", cardUID.c_str());  // ← add
  return -1;
}


String getCardOwner(String cardUID) {
  String owner;
  if (checkLocalCache(cardUID, owner)) return owner;
  if (redisOnline) {
    String key = "card:" + cardUID;
    String o   = redis.hget(key.c_str(), "owner");
    if (o.length() > 0) return o;
  }
  return "Unknown";
}


void logCardEntry(String cardUID) {
  String ts    = String(millis());
  String owner = getCardOwner(cardUID);  // works online OR offline now


  if (redisOnline) {
    String logEntry = cardUID + "|" + owner + "|" + ts;
    redis.lpush("parking:entry_log", logEntry.c_str());
    redis.hset("parking:last_entry", "card",  cardUID.c_str());
    redis.hset("parking:last_entry", "owner", owner.c_str());
    redis.hset("parking:last_entry", "time",  ts.c_str());
    String countKey = "card:" + cardUID + ":entry_count";
    redisIncr(countKey.c_str());
    Serial.printf("[REDIS] Entry logged: %s (%s)\n", cardUID.c_str(), owner.c_str());
  } else {
    appendToOfflineQueue("ENTRY", cardUID, owner, ts);
  }
}


void logCardExit(String cardUID) {
  String ts    = String(millis());
  String owner = getCardOwner(cardUID);


  if (redisOnline) {
    String logEntry = cardUID + "|" + owner + "|" + ts;
    redis.lpush("parking:exit_log", logEntry.c_str());
    redis.hset("parking:last_exit", "card",  cardUID.c_str());
    redis.hset("parking:last_exit", "owner", owner.c_str());
    redis.hset("parking:last_exit", "time",  ts.c_str());
    String countKey = "card:" + cardUID + ":exit_count";
    redisIncr(countKey.c_str());
    Serial.printf("[REDIS] Exit logged: %s (%s)\n", cardUID.c_str(), owner.c_str());
  } else {
    appendToOfflineQueue("EXIT", cardUID, owner, ts);
  }
}


void updateParkingStatus() {
  if (!redisOnline) {
    Serial.println("[REDIS] Skipping status update — offline");
    return;
  }
  int avail = totalSpaces - occupiedSpaces;
  redis.hset("parking:status", "occupied",    String(occupiedSpaces).c_str());
  redis.hset("parking:status", "available",   String(avail).c_str());
  redis.hset("parking:status", "total",       String(totalSpaces).c_str());
  redis.hset("parking:status", "last_update", String(millis()).c_str());
  redis.hset("parking:status", "esp32_ip",    WiFi.localIP().toString().c_str());
  Serial.printf("[REDIS] Status updated: %d/%d occupied\n", occupiedSpaces, totalSpaces);
}


void saveOccupiedToRedis() {
  if (!redisOnline) return;
  redis.set("parking:occupied_persistent", String(occupiedSpaces).c_str());
}


void restoreFromRedis() {
  if (!redisOnline) return;
  String saved = redis.get("parking:occupied_persistent");
  if (saved.length() > 0) {
    int v = saved.toInt();
    if (v >= 0 && v <= totalSpaces) {
      occupiedSpaces = v;
      Serial.printf("[REDIS] Restored occupied count: %d\n", occupiedSpaces);
    }
  }
}


// =======================================================
// WIFI
// =======================================================
void connectToWiFi() {
  Serial.print("[WiFi] Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected!");
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WiFi] Failed to connect!");
  }
}


void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.println("[WiFi] Reconnecting...");
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Reconnected!");
  } else {
    Serial.println("\n[WiFi] Reconnect failed!");
  }
}


// Checks Redis connection, updates redisOnline flag.
// Coming back online: syncs offline queue + refreshes UID cache.
bool ensureRedis() {
  if (redisClient.connected()) {
    if (!redisOnline) {
      redisOnline = true;
      Serial.println("[REDIS] Back ONLINE — syncing logs + cache...");
      syncOfflineQueue();
      refreshValidUidCache();
      updateParkingStatus();
    }
    return true;
  }


  bool wasOnline = redisOnline;
  redisOnline = false;
  redisClient.stop();


  if (!redisClient.connect(redisHost, redisPort)) {
    if (wasOnline) Serial.println("[REDIS] Connection lost — OFFLINE mode active");
    return false;
  }


  redisOnline = true;
  Serial.println("[REDIS] Reconnected! Back ONLINE — syncing logs + cache...");
  syncOfflineQueue();
  refreshValidUidCache();
  updateParkingStatus();
  return true;
}


// =======================================================
// ULTRASONIC & GATE
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


void beepGateOpen() {
  ledcWriteTone(14, 1200); delay(150); ledcWriteTone(14, 0);
  delay(100);
  ledcWriteTone(14, 1800); delay(150); ledcWriteTone(14, 0);
}


// make the servo slow
void openEntryGate() {
  Serial.println("[ENTRY GATE] Opening slowly...");
  beepGateOpen();
  for (int pos = 0; pos <= 90; pos++) {
    servoEntry.write(pos);
    delay(15); // Geschwindigkeit: 15ms pro Grad
  }
}


void closeEntryGate() {
  Serial.println("[ENTRY GATE] Closing slowly...");
  for (int pos = 90; pos >= 0; pos--) {
    servoEntry.write(pos);
    delay(15);
  }
}


void openExitGate() {
  Serial.println("[EXIT GATE] Opening slowly...");
  beepGateOpen();
  for (int pos = 0; pos <= 90; pos++) {
    servoExit.write(pos);
    delay(15);
  }
}


void closeExitGate() {
  Serial.println("[EXIT GATE] Closing slowly...");
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


// Discard any cards that were swiped while the gate was moving
// so we never silently drop a real scan.
void flushRFID(MFRC522 &rfid) {
  while (rfid.PICC_IsNewCardPresent()) {
    rfid.PICC_ReadCardSerial();
    rfid.PICC_HaltA();
  }
}


// =======================================================
// SERIAL COMMANDS — debug + admin
// Type any of: dump | count | clear | fsinfo | cache |
//              refresh | version | help
// =======================================================
void handleSerialCommands() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toLowerCase();
  if (cmd.length() == 0) return;


  if (cmd == "dump") {
    if (!LittleFS.exists(OFFLINE_QUEUE)) { Serial.println("[DUMP] No queue file"); return; }
    File f = LittleFS.open(OFFLINE_QUEUE, FILE_READ);
    if (!f) { Serial.println("[DUMP] Cannot open"); return; }
    Serial.printf("\n--- offline_queue.txt (%d bytes) ---\n", f.size());
    while (f.available()) Serial.write(f.read());
    Serial.println("\n--- end ---\n");
    f.close();
  }
  else if (cmd == "count") {
    if (!LittleFS.exists(OFFLINE_QUEUE)) { Serial.println("[COUNT] 0 (no file)"); return; }
    File f = LittleFS.open(OFFLINE_QUEUE, FILE_READ);
    int n = 0;
    while (f.available()) if (f.read() == '\n') n++;
    f.close();
    Serial.printf("[COUNT] %d buffered log(s)\n", n);
  }
  else if (cmd == "clear") {
    File f = LittleFS.open(OFFLINE_QUEUE, FILE_WRITE);
    if (f) f.close();
    Serial.println("[CLEAR] Queue truncated");
  }
  else if (cmd == "fsinfo") {
    Serial.printf("[FS] Used: %u / Total: %u bytes\n",
                  LittleFS.usedBytes(), LittleFS.totalBytes());
  }
  else if (cmd == "cache") {
    if (!LittleFS.exists(VALID_UIDS_FILE)) { Serial.println("[CACHE] No cache file"); return; }
    File f = LittleFS.open(VALID_UIDS_FILE, FILE_READ);
    if (!f) { Serial.println("[CACHE] Cannot open"); return; }
    Serial.printf("\n--- valid_uids.txt (%d bytes, version %ld, ready=%s) ---\n",
                  f.size(), cachedVersion, cacheReady ? "yes" : "no");
    while (f.available()) Serial.write(f.read());
    Serial.println("\n--- end ---\n");
    f.close();
  }
  else if (cmd == "refresh") {
    if (!redisOnline) { Serial.println("[REFRESH] Redis offline — cannot refresh"); return; }
    refreshValidUidCache();
  }
  else if (cmd == "version") {
    Serial.printf("[VERSION] Local: %ld | Cache ready: %s | Redis: %s\n",
                  cachedVersion,
                  cacheReady   ? "yes"    : "no",
                  redisOnline  ? "ONLINE" : "OFFLINE");
  }
  else if (cmd == "help") {
    Serial.println("Commands: dump | count | clear | fsinfo | cache | refresh | version | help");
  }
  else {
    Serial.printf("[?] Unknown command: '%s' — type 'help'\n", cmd.c_str());
  }
}


// =======================================================
// SETUP
// =======================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println(" SMART PARKING — REDIS + HYBRID CACHE  ");
  Serial.println("========================================\n");


  // Optional: silence the verbose [E][WiFiClient.cpp] noise during reconnects.
  // Comment out if you want to see real socket errors during debugging.
  esp_log_level_set("WiFiClient", ESP_LOG_NONE);


  // LittleFS — both offline queue + valid-UID cache live here
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount failed!");
  } else {
    Serial.println("[FS] LittleFS ready");
    ensureOfflineFileExists();
    if (LittleFS.exists(OFFLINE_QUEUE))
      Serial.println("[FS] Offline queue file found from previous run");
    if (LittleFS.exists(VALID_UIDS_FILE))
      Serial.println("[FS] Valid-UID cache found from previous run");
  }


  connectToWiFi();


  if (WiFi.status() == WL_CONNECTED) {
    if (connectToRedis()) {
      restoreFromRedis();
      updateParkingStatus();
      saveOccupiedToRedis();
      syncOfflineQueue();
      refreshValidUidCache();   // initial sync — sets cacheReady = true
    }
  }


  // If Redis was unreachable at boot but we have a cache from a previous
  // run, trust it. The system can operate offline immediately.
  if (!cacheReady && LittleFS.exists(VALID_UIDS_FILE)) {
    cacheReady = true;
    Serial.println("[CACHE] Using existing cache from previous boot");
  }


  if (!cacheReady) {
    Serial.println("[BOOT] No cache available — gate will deny all cards");
    Serial.println("[BOOT] Waiting for first Redis sync before accepting cards...");
  }


  SPI.begin();
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


  Serial.printf("\n[SYSTEM] READY! Redis: %s | Cache: %s\n",
                redisOnline ? "ONLINE" : "OFFLINE",
                cacheReady  ? "READY"  : "EMPTY");
  Serial.println("Type 'help' for serial commands.");
  Serial.println("========================================\n");
}


// =======================================================
// MAIN LOOP
// =======================================================
void loop() {
  handleSerialCommands();
  ensureWiFi();


  // Periodic Redis health check every 10 s.
  // If Redis just came back: sync logs + refresh cache (inside ensureRedis).
  static unsigned long lastRedisCheck = 0;
  if (millis() - lastRedisCheck > 10000) {
    lastRedisCheck = millis();
    if (!ensureRedis()) {
      Serial.println("[REDIS] OFFLINE — logs buffered to flash");
    }
  }


  // Cheap version-poll for admin updates (every 3 s when online)
  checkForCacheUpdates();


  // If we still have no cache, keep the LCD warning visible and skip RFID
  if (!cacheReady) {
    delay(200);
    return;
  }


  // ========== ENTRY — CAR DETECTION ==========
  if (isCarPresent(TRIG_ENTRY_BEFORE, ECHO_ENTRY_BEFORE)) {
    lcdEntry.clear();
    lcdEntry.setCursor(0, 0); lcdEntry.print("Welcome!");
    lcdEntry.setCursor(0, 1); lcdEntry.print("Scan Your Card");
  } else {
    updateEntryLCD();
  }


  // ========== ENTRY RFID ==========
  if (rfidEntry.PICC_IsNewCardPresent() && rfidEntry.PICC_ReadCardSerial()) {
    String cardUID = getRFIDUID(rfidEntry);
    Serial.println("\n>>> [ENTRY] Card: " + cardUID);


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
      //delay(2000);


      // occupiedSpaces++;   "we minus place after sensor detect enter in line 816"
      saveOccupiedToRedis();
      updateParkingStatus();
     // closeEntryGate();    stop closing until car is passed
      flushRFID(rfidEntry);   // discard any cards swiped during gate motion
      updateEntryLCD();
      updateExitLCD();


      //lcdEntry.clear();
      //lcdEntry.setCursor(0, 0); lcdEntry.print("Welcome " + owner + "!");
      //lcdEntry.setCursor(0, 1); lcdEntry.print("Drive Safely!");
      //delay(2000);
      //updateEntryLCD();




      // Wait for car to enter (using AFTER sensor)
      unsigned long gateOpenTime = millis();
      bool carEntered = false;


      while (millis() - gateOpenTime < 15000) {
        if (isCarPresent(TRIG_ENTRY_AFTER, ECHO_ENTRY_AFTER)) {
          carEntered = true;
          Serial.println("[ENTRY] Car detected at AFTER sensor");
          lcdEntry.clear();
          lcdEntry.setCursor(0, 0);
          lcdEntry.print("Car Detected");
          lcdEntry.setCursor(0, 1);
          lcdEntry.print("Please Proceed");
          break;
        }
        delay(10);
      }
     
      if (carEntered) {
        // Wait for car to clear the gate
        while (isCarPresent(TRIG_ENTRY_AFTER, ECHO_ENTRY_AFTER)) {
          delay(50);
        }
        occupiedSpaces++;
        Serial.printf("[ENTRY] Car entered. Occupied: %d/%d\n", occupiedSpaces, totalSpaces);
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


  // ========== CHECK FOR CAR AT EXIT (BEFORE SENSOR) ==========
  bool carAtExitBefore = isCarPresent(TRIG_EXIT_BEFORE, ECHO_EXIT_BEFORE);
 
  if (carAtExitBefore) {
    // Show scan card message on exit LCD
    lcdExit.clear();
    lcdExit.setCursor(0, 0);
    lcdExit.print("Goodbye!");
    lcdExit.setCursor(0, 1);
    lcdExit.print("Scan Your Card");
    // NO BEEP HERE - Only RFID triggers beep
  } else {
    // Show normal exit display (Exit Ready)
    updateExitLCD();
  }


  // ========== EXIT RFID ==========
  if (rfidExit.PICC_IsNewCardPresent() && rfidExit.PICC_ReadCardSerial()) {
    String cardUID = getRFIDUID(rfidExit);
    Serial.println("\n>>> [EXIT] Card: " + cardUID);


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
      //delay(3000);


      //occupiedSpaces--;
      saveOccupiedToRedis();
      updateParkingStatus();
      //closeExitGate();
      flushRFID(rfidExit);
      updateEntryLCD();
      updateExitLCD();


      //lcdExit.clear();
      //lcdExit.setCursor(0, 0); lcdExit.print("Goodbye " + owner + "!");
      //lcdExit.setCursor(0, 1); lcdExit.print("Drive Safely!");
      //delay(2000);
      //updateExitLCD();


      // Wait for car to Exit (using AFTER sensor)
      unsigned long gateOpenTime = millis();
      bool carEntered = false;


      while (millis() - gateOpenTime < 15000) {
        if (isCarPresent(TRIG_EXIT_AFTER, ECHO_EXIT_AFTER)) {
          carEntered = true;
          Serial.println("[ENTRY] Car detected at AFTER sensor");
          lcdExit.clear();
          lcdExit.setCursor(0, 0);
          lcdExit.print("Car Detected");
          lcdExit.setCursor(0, 1);
          lcdExit.print("Please Proceed");
          break;
        }
        delay(10);
      }
     
      if (carEntered) {
        // Wait for car to clear the gate
        while (isCarPresent(TRIG_EXIT_AFTER, ECHO_EXIT_AFTER)) {
          delay(50);
        }
        occupiedSpaces--;
        Serial.printf("[ENTRY] Car entered. Occupied: %d/%d\n", occupiedSpaces, totalSpaces);
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

