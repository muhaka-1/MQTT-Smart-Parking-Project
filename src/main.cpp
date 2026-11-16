// =======================================================
// SMART PARKING - ESP32 gate controller (edge node)
//
// Edge-first design: every gate decision is made locally from a card
// whitelist cached in flash, so the gates keep working when Wi-Fi or the
// MQTT broker is down. Events are stored in LittleFS until the backend has
// acknowledged them (store & forward).
//
// MQTT contract: docs/MQTT_TOPICS.md (same as backend/src/topics.js)
// =======================================================
#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "Copy include/secrets.example.h to include/secrets.h and fill in your Wi-Fi and broker settings."
#endif

// =======================================================
// MQTT TOPICS
// =======================================================
// device -> backend
const char* topicOnline          = "parking/device/online";   // retained, "offline" is the last will
const char* topicStatus          = "parking/status";          // retained JSON snapshot
const char* topicEntryLog        = "parking/entry_log";
const char* topicExitLog         = "parking/exit_log";
const char* topicDeniedLog       = "parking/denied_log";
const char* topicCachePoll       = "parking/cache/poll_request";
const char* topicOccupiedRequest = "parking/occupied_persistent/request";
// backend -> device
const char* topicAck              = "parking/ack";
const char* topicTime             = "parking/time";
const char* topicCacheVersion     = "parking/cards_version";
const char* topicCacheUpdate      = "parking/cache/update";
const char* topicOccupiedResponse = "parking/occupied_persistent/response";
const char* topicCmd              = "parking/cmd";

#define DEVICE_NAME      "esp32-gate"
#define MQTT_CLIENT_ID   "Freenove_ESP32S3_Gateway"
#define MQTT_BUFFER_SIZE 4096   // whitelist payload must fit (about 150 cards)

// =======================================================
// HARDWARE PINS (Freenove ESP32-S3)
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
#define BUZZER_CHANNEL  14 // kept away from the servo PWM channels

#define SERVO_ENTRY_PIN 4
#define SERVO_EXIT_PIN  18

#define TRIG_ENTRY_BEFORE 16
#define ECHO_ENTRY_BEFORE 17
#define TRIG_ENTRY_AFTER  7
#define ECHO_ENTRY_AFTER  15

#define TRIG_EXIT_BEFORE  20
#define ECHO_EXIT_BEFORE  21
#define TRIG_EXIT_AFTER   8
#define ECHO_EXIT_AFTER   9

// =======================================================
// BEHAVIOUR
// =======================================================
#define CAR_DISTANCE_CM        15       // closer than this = vehicle present
#define GATE_TIMEOUT_MS        15000UL  // close again if nobody drives through
#define CLEAR_READINGS_NEEDED  5        // consecutive "no vehicle" readings before closing
#define ACK_TIMEOUT_MS         5000UL   // no ack from backend -> keep the event in flash
#define STATUS_INTERVAL_MS     30000UL
#define CACHE_POLL_INTERVAL_MS 30000UL
#define WIFI_RETRY_MS          10000UL
#define MQTT_RETRY_MIN_MS      5000UL   // first retry after a lost broker
#define MQTT_RETRY_MAX_MS      30000UL  // back off to this while it stays away
#define MQTT_TCP_TIMEOUT_MS    1000     // longest the gates may freeze per retry
#define QUEUE_RETRY_MS         10000UL
#define QUEUE_MAX_BYTES        200000UL
#define SYNC_BATCH             10
#define MAX_PENDING            8

#define OFFLINE_QUEUE   "/offline_queue.txt"
#define QUEUE_TMP       "/offline_queue.tmp"
#define VALID_UIDS_FILE "/valid_uids.txt"
#define VALID_UIDS_TMP  "/valid_uids.tmp"

// =======================================================
// GLOBAL OBJECTS AND STATE
// =======================================================
WiFiClient        espClient;
PubSubClient      mqttClient(espClient);
Preferences       prefs;
MFRC522           rfidEntry(SS_RFID_ENTRY, RST_RFID);
MFRC522           rfidExit(SS_RFID_EXIT, RST_RFID);
Servo             servoEntry;
Servo             servoExit;
LiquidCrystal_I2C lcdEntry(LCD_ENTRY_ADDR, 16, 2);
LiquidCrystal_I2C lcdExit(LCD_EXIT_ADDR, 16, 2);

bool mqttOnline      = false;
bool fsReady         = false;
bool cacheReady      = false;
long cachedVersion   = -1;
int  totalSpaces     = 10;
int  occupiedSpaces  = 0;
bool occupancyKnown  = false;   // false after a flash erase: ask the backend once
bool entryGateOpen   = false;
bool exitGateOpen    = false;
int  queuedCount     = 0;

uint32_t bootCount   = 0;
uint32_t eventSeq    = 0;

// Clock: the backend publishes unix time on parking/time (works without internet).
bool     clockSynced   = false;
uint32_t epochAtSync   = 0;
uint32_t millisAtSync  = 0;

unsigned long lastWifiTry    = 0;
unsigned long lastMqttTry    = 0;
unsigned long mqttRetryMs    = MQTT_RETRY_MIN_MS;
unsigned long lastStatus     = 0;
unsigned long lastCachePoll  = 0;
unsigned long lastQueueTry   = 0;

struct ParkEvent {
  String   id;
  char     type;      // 'E' entry, 'X' exit, 'D' denied
  bool     atExit;    // which gate it happened at
  String   uid;
  String   owner;
  String   reason;
  bool     passed;
  uint32_t ts;        // unix seconds, 0 = clock unknown when it happened
  uint32_t boot;
  uint32_t up;        // millis() when it happened
  int      occupied;
  bool     offline;
};

struct PendingEvent {
  bool          used;
  unsigned long sentAt;
  ParkEvent     evt;
};
PendingEvent pending[MAX_PENDING];

// Batch that syncOfflineQueue() is currently waiting on.
String batchIds[SYNC_BATCH];
bool   batchAcked[SYNC_BATCH];
int    batchSize = 0;

String lcdEntryShown[2];
String lcdExitShown[2];

void publishStatus();
void refreshIdleLCDs();
void runGateCycle(bool isEntry, const String& uid, const String& owner);

// =======================================================
// CLOCK
// =======================================================
uint32_t epochForUptime(uint32_t upMs) {
  if (!clockSynced) return 0;
  int64_t diffMs = (int64_t)upMs - (int64_t)millisAtSync;
  return (uint32_t)((int64_t)epochAtSync + diffMs / 1000);
}

// =======================================================
// DISPLAY, BUZZER, SENSORS
// =======================================================
void showLCD(LiquidCrystal_I2C& lcd, String* shown, String l1, String l2) {
  if (l1.length() > 16) l1 = l1.substring(0, 16);
  if (l2.length() > 16) l2 = l2.substring(0, 16);
  if (shown[0] == l1 && shown[1] == l2) return;   // no rewrite = no flicker
  shown[0] = l1;
  shown[1] = l2;
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(l1);
  lcd.setCursor(0, 1); lcd.print(l2);
}

void showEntry(const String& l1, const String& l2) { showLCD(lcdEntry, lcdEntryShown, l1, l2); }
void showExit(const String& l1, const String& l2)  { showLCD(lcdExit, lcdExitShown, l1, l2); }

// A '*' in the top right corner means "working offline".
String withLinkMark(String text) {
  if (mqttOnline) return text;
  while (text.length() < 15) text += ' ';
  return text.substring(0, 15) + "*";
}

String freeText() {
  return "Free: " + String(totalSpaces - occupiedSpaces) + "/" + String(totalSpaces);
}

void beepSuccess() {
  ledcWriteTone(BUZZER_CHANNEL, 1800); delay(200); ledcWriteTone(BUZZER_CHANNEL, 0);
}

void beepError() {
  for (int i = 0; i < 3; i++) {
    ledcWriteTone(BUZZER_CHANNEL, 900); delay(200); ledcWriteTone(BUZZER_CHANNEL, 0); delay(200);
  }
}

void beepGateOpen() {
  ledcWriteTone(BUZZER_CHANNEL, 1200); delay(150); ledcWriteTone(BUZZER_CHANNEL, 0);
  delay(100);
  ledcWriteTone(BUZZER_CHANNEL, 1800); delay(150); ledcWriteTone(BUZZER_CHANNEL, 0);
}

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
  return getDistance(trig, echo) < CAR_DISTANCE_CM;
}

void refreshIdleLCDs() {
  if (!cacheReady) {
    showEntry("System Init", "Waiting sync...");
    showExit("System Init", "Waiting sync...");
    return;
  }
  bool atEntry = isCarPresent(TRIG_ENTRY_BEFORE, ECHO_ENTRY_BEFORE);
  bool atExit  = isCarPresent(TRIG_EXIT_BEFORE, ECHO_EXIT_BEFORE);

  if (occupiedSpaces >= totalSpaces) showEntry(withLinkMark("Parking FULL"), freeText());
  else showEntry(withLinkMark("Welcome!"), atEntry ? String("Scan Your Card") : freeText());

  showExit(withLinkMark(atExit ? "Goodbye!" : "Exit Ready"), atExit ? String("Scan Your Card") : freeText());
}

// =======================================================
// PERSISTENT STATE (NVS)
// =======================================================
void saveOccupancy() {
  prefs.putInt("occ", occupiedSpaces);
  prefs.putInt("total", totalSpaces);
  occupancyKnown = true;
}

// =======================================================
// CARD WHITELIST CACHE (LittleFS)
// =======================================================
bool checkLocalCache(const String& uid, String& outOwner) {
  if (!fsReady || !LittleFS.exists(VALID_UIDS_FILE)) return false;
  File f = LittleFS.open(VALID_UIDS_FILE, FILE_READ);
  if (!f) return false;

  bool found = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int sep = line.indexOf('|');
    if (sep < 0) continue;
    if (line.substring(0, sep).equalsIgnoreCase(uid)) {
      outOwner = line.substring(sep + 1);
      found = true;
      break;
    }
  }
  f.close();
  return found;
}

// Payload: first line "v=<version>", then one "UID|Owner" per line.
void storeWhitelist(const String& message) {
  int nl = message.indexOf('\n');
  String head = nl < 0 ? message : message.substring(0, nl);
  head.trim();
  if (!head.startsWith("v=")) {
    Serial.println("[CACHE] Ignored whitelist without version header.");
    return;
  }
  long version = head.substring(2).toInt();
  if (version == cachedVersion && cacheReady) return;   // already have it
  if (!fsReady) return;

  // Write to a temp file first so a power cut cannot leave half a whitelist.
  File f = LittleFS.open(VALID_UIDS_TMP, FILE_WRITE);
  if (!f) {
    Serial.println("[CACHE] ERROR: cannot write whitelist to flash.");
    return;
  }
  if (nl >= 0) f.print(message.substring(nl + 1));
  f.close();
  LittleFS.remove(VALID_UIDS_FILE);
  LittleFS.rename(VALID_UIDS_TMP, VALID_UIDS_FILE);

  cachedVersion = version;
  cacheReady = true;
  prefs.putLong("cver", cachedVersion);
  Serial.printf("[CACHE] Whitelist v%ld stored in flash.\n", cachedVersion);
  publishStatus();
}

// =======================================================
// STORE & FORWARD
// =======================================================
String eventToLine(const ParkEvent& e) {
  String line = e.id;
  line += '|'; line += e.type;
  line += '|'; line += e.uid;
  line += '|'; line += e.owner;
  line += '|'; line += e.reason;
  line += '|'; line += (e.passed ? '1' : '0');
  line += '|'; line += String(e.ts);
  line += '|'; line += String(e.boot);
  line += '|'; line += String(e.up);
  line += '|'; line += String(e.occupied);
  line += '|'; line += (e.atExit ? '1' : '0');
  return line;
}

bool lineToEvent(const String& line, ParkEvent& e) {
  const int FIELDS = 11;
  String parts[FIELDS];
  int start = 0;
  for (int i = 0; i < FIELDS; i++) {
    int sep = line.indexOf('|', start);
    if (sep < 0) {
      if (i != FIELDS - 1) return false;
      parts[i] = line.substring(start);
    } else {
      parts[i] = line.substring(start, sep);
      start = sep + 1;
    }
  }
  if (parts[0].length() == 0 || parts[1].length() != 1) return false;
  e.id       = parts[0];
  e.type     = parts[1][0];
  e.uid      = parts[2];
  e.owner    = parts[3];
  e.reason   = parts[4];
  e.passed   = parts[5] == "1";
  e.ts       = strtoul(parts[6].c_str(), nullptr, 10);
  e.boot     = strtoul(parts[7].c_str(), nullptr, 10);
  e.up       = strtoul(parts[8].c_str(), nullptr, 10);
  e.occupied = parts[9].toInt();
  e.atExit   = parts[10] == "1";
  e.offline  = true;
  return true;
}

int countQueueLines() {
  if (!fsReady || !LittleFS.exists(OFFLINE_QUEUE)) return 0;
  File f = LittleFS.open(OFFLINE_QUEUE, FILE_READ);
  if (!f) return 0;
  int n = 0;
  while (f.available()) if (f.read() == '\n') n++;
  f.close();
  return n;
}

void appendToOfflineQueue(const ParkEvent& e) {
  if (!fsReady) {
    Serial.println("[OFFLINE] ERROR: flash not available, event lost!");
    return;
  }
  File f = LittleFS.open(OFFLINE_QUEUE, FILE_APPEND);
  if (!f) {
    Serial.println("[OFFLINE] ERROR: cannot open queue file, event lost!");
    return;
  }
  if (f.size() > QUEUE_MAX_BYTES) {
    f.close();
    Serial.println("[OFFLINE] ERROR: queue is full, event dropped!");
    return;
  }
  f.println(eventToLine(e));
  f.close();
  queuedCount++;
  Serial.printf("[OFFLINE] Event %s saved to flash (%d waiting).\n", e.id.c_str(), queuedCount);
}

bool publishEvent(const ParkEvent& e) {
  const char* topic = e.type == 'E' ? topicEntryLog : e.type == 'X' ? topicExitLog : topicDeniedLog;

  // An event recorded before the clock was known can still be dated if it
  // happened during this boot.
  uint32_t ts = e.ts;
  if (ts == 0 && e.boot == bootCount) ts = epochForUptime(e.up);

  JsonDocument doc;
  doc["id"]       = e.id;
  doc["device"]   = DEVICE_NAME;
  doc["uid"]      = e.uid;
  doc["owner"]    = e.owner;
  doc["gate"]     = e.atExit ? "exit" : "entry";
  if (e.type == 'D') {
    doc["reason"] = e.reason;
  } else {
    doc["passed"] = e.passed;
  }
  doc["ts"]       = ts;
  doc["offline"]  = e.offline;
  doc["occupied"] = e.occupied;

  char buf[384];
  size_t len = serializeJson(doc, buf, sizeof(buf));
  return mqttClient.publish(topic, (const uint8_t*)buf, len, false);
}

void handleAck(const String& id) {
  for (int i = 0; i < MAX_PENDING; i++) {
    if (pending[i].used && pending[i].evt.id == id) pending[i].used = false;
  }
  for (int i = 0; i < batchSize; i++) {
    if (batchIds[i] == id) batchAcked[i] = true;
  }
}

// Events sent live wait here for the backend's ack. No ack -> flash queue.
void checkPendingAcks() {
  for (int i = 0; i < MAX_PENDING; i++) {
    if (!pending[i].used) continue;
    if (millis() - pending[i].sentAt < ACK_TIMEOUT_MS) continue;
    pending[i].used = false;
    Serial.printf("[MQTT] No ack for %s, keeping it in flash.\n", pending[i].evt.id.c_str());
    appendToOfflineQueue(pending[i].evt);
  }
}

void logEvent(char type, bool atExit, const String& uid, const String& owner, const String& reason, bool passed) {
  ParkEvent e;
  e.atExit   = atExit;
  e.id       = "esp-" + String(bootCount) + "-" + String(++eventSeq);
  e.type     = type;
  e.uid      = uid;
  e.owner    = owner;
  e.reason   = reason;
  e.passed   = passed;
  e.up       = millis();
  e.ts       = epochForUptime(e.up);
  e.boot     = bootCount;
  e.occupied = occupiedSpaces;
  e.offline  = false;

  if (mqttOnline && mqttClient.connected()) {
    int slot = -1;
    for (int i = 0; i < MAX_PENDING; i++) if (!pending[i].used) { slot = i; break; }
    if (slot >= 0 && publishEvent(e)) {
      pending[slot].used = true;
      pending[slot].sentAt = millis();
      pending[slot].evt = e;
      Serial.printf("[MQTT] Event %s sent, waiting for ack.\n", e.id.c_str());
      return;
    }
  }
  appendToOfflineQueue(e);
}

// Sends buffered events in small batches. A line is removed from flash only
// after the backend acknowledged it, so nothing is lost if the link drops
// again half-way. The backend ignores duplicates (same event id).
void syncOfflineQueue() {
  if (!fsReady || !mqttOnline || queuedCount == 0) return;
  Serial.printf("[SYNC] %d events in flash, sending...\n", queuedCount);

  for (int round = 0; round < 5 && queuedCount > 0 && mqttClient.connected(); round++) {
    File f = LittleFS.open(OFFLINE_QUEUE, FILE_READ);
    if (!f) return;

    batchSize = 0;
    int linesRead = 0;
    bool keepLine[SYNC_BATCH];
    while (f.available() && linesRead < SYNC_BATCH) {
      String line = f.readStringUntil('\n');
      line.trim();
      int idx = linesRead++;
      keepLine[idx] = false;
      batchIds[idx] = "";
      batchAcked[idx] = true;              // empty or broken lines are simply dropped
      batchSize = linesRead;
      ParkEvent e;
      if (line.length() == 0 || !lineToEvent(line, e)) continue;
      batchIds[idx] = e.id;
      batchAcked[idx] = false;
      publishEvent(e);
      mqttClient.loop();
    }
    f.close();

    // Wait for the acks of this batch.
    unsigned long waitStart = millis();
    while (millis() - waitStart < 3000) {
      bool all = true;
      for (int i = 0; i < batchSize; i++) if (!batchAcked[i]) all = false;
      if (all) break;
      mqttClient.loop();
      delay(5);
    }

    int acked = 0;
    for (int i = 0; i < batchSize; i++) {
      keepLine[i] = !batchAcked[i];
      if (batchAcked[i]) acked++;
    }
    int handled = batchSize;
    batchSize = 0;

    if (acked == 0) {
      Serial.println("[SYNC] No acknowledgement from backend, will retry later.");
      return;
    }

    // Rewrite the queue without the acknowledged lines.
    File src = LittleFS.open(OFFLINE_QUEUE, FILE_READ);
    File dst = LittleFS.open(QUEUE_TMP, FILE_WRITE);
    if (!src || !dst) {
      if (src) src.close();
      if (dst) dst.close();
      Serial.println("[SYNC] ERROR: cannot rewrite queue file.");
      return;
    }
    int idx = 0;
    int remaining = 0;
    while (src.available()) {
      String line = src.readStringUntil('\n');
      line.trim();
      bool keep = idx >= handled || keepLine[idx];
      idx++;
      if (!keep || line.length() == 0) continue;
      dst.println(line);
      remaining++;
    }
    src.close();
    dst.close();
    LittleFS.remove(OFFLINE_QUEUE);
    LittleFS.rename(QUEUE_TMP, OFFLINE_QUEUE);
    queuedCount = remaining;
    Serial.printf("[SYNC] %d events confirmed, %d left.\n", acked, remaining);
    if (acked < handled) break;   // some were not confirmed: try again later
  }
  publishStatus();
}

// =======================================================
// MQTT
// =======================================================
void publishStatus() {
  lastStatus = millis();
  if (!mqttOnline || !mqttClient.connected()) return;

  JsonDocument doc;
  doc["device"]       = DEVICE_NAME;
  doc["occupied"]     = occupiedSpaces;
  doc["total"]        = totalSpaces;
  doc["free"]         = totalSpaces - occupiedSpaces;
  doc["entryGate"]    = entryGateOpen ? "open" : "closed";
  doc["exitGate"]     = exitGateOpen ? "open" : "closed";
  doc["queued"]       = queuedCount;
  doc["cacheVersion"] = cachedVersion;
  doc["ip"]           = WiFi.localIP().toString();
  doc["rssi"]         = WiFi.RSSI();
  doc["uptime"]       = millis() / 1000;

  char buf[320];
  size_t len = serializeJson(doc, buf, sizeof(buf));
  mqttClient.publish(topicStatus, (const uint8_t*)buf, len, true);
}

void handleCommand(const String& message) {
  JsonDocument doc;
  if (deserializeJson(doc, message)) {
    Serial.println("[CMD] Ignored: not valid JSON.");
    return;
  }
  String cmd = doc["cmd"] | "";
  Serial.printf("[CMD] %s\n", cmd.c_str());

  if (cmd == "open_entry") {
    runGateCycle(true, "", "");
  } else if (cmd == "open_exit") {
    runGateCycle(false, "", "");
  } else if (cmd == "set_occupied" && doc["value"].is<int>()) {
    occupiedSpaces = constrain(doc["value"].as<int>(), 0, totalSpaces);
    saveOccupancy();
  } else if (cmd == "set_total" && doc["value"].is<int>()) {
    int value = doc["value"].as<int>();
    if (value < 1 || value > 500) return;
    totalSpaces = value;
    occupiedSpaces = constrain(occupiedSpaces, 0, totalSpaces);
    saveOccupancy();
  } else if (cmd == "sync") {
    lastQueueTry = 0;
  } else {
    return;
  }
  publishStatus();
}

// Commands that move a gate are executed from loop(), not from inside the
// MQTT callback, so the client is never re-entered while it is busy.
String deferredCommand = "";

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length);
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  String topicStr(topic);

  if (topicStr == topicAck) {
    message.trim();
    handleAck(message);
  }
  else if (topicStr == topicTime) {
    uint32_t epoch = strtoul(message.c_str(), nullptr, 10);
    if (epoch > 1700000000UL) {
      epochAtSync = epoch;
      millisAtSync = millis();
      clockSynced = true;
    }
  }
  else if (topicStr == topicCacheUpdate) {
    storeWhitelist(message);
  }
  else if (topicStr == topicCacheVersion) {
    long serverVersion = message.toInt();
    if (serverVersion != cachedVersion) {
      Serial.printf("[CACHE] Whitelist changed (%ld -> %ld), requesting update.\n", cachedVersion, serverVersion);
      lastCachePoll = 0;   // poll on the next loop
    }
  }
  else if (topicStr == topicOccupiedResponse) {
    // Only used when this device has no stored count (first boot / flash erased).
    int value = message.toInt();
    if (!occupancyKnown && value >= 0 && value <= totalSpaces) {
      occupiedSpaces = value;
      saveOccupancy();
      Serial.printf("[MQTT] Occupancy restored from backend: %d\n", occupiedSpaces);
    }
  }
  else if (topicStr == topicCmd) {
    deferredCommand = message;
  }
}

bool connectToMQTT() {
  lastMqttTry = millis();
  Serial.printf("[MQTT] Connecting to %s:%d ...\n", MQTT_HOST, MQTT_PORT);

  // Open the TCP connection ourselves with a short timeout, so an unreachable
  // server cannot freeze card reading for long. PubSubClient reuses the socket.
  // Last will: the broker publishes "offline" for us if the link dies.
  bool ok = espClient.connected() || espClient.connect(MQTT_HOST, MQTT_PORT, MQTT_TCP_TIMEOUT_MS);
  ok = ok && mqttClient.connect(MQTT_CLIENT_ID, nullptr, nullptr, topicOnline, 1, true, "offline");
  if (!ok) {
    espClient.stop();
    mqttRetryMs = min(mqttRetryMs * 2, MQTT_RETRY_MAX_MS);
    Serial.printf("[MQTT] Failed (state %d). Running standalone, next try in %lu s.\n", mqttClient.state(), mqttRetryMs / 1000);
    mqttOnline = false;
    return false;
  }

  mqttOnline = true;
  mqttRetryMs = MQTT_RETRY_MIN_MS;
  Serial.println("[MQTT] Connected. System ONLINE.");

  mqttClient.subscribe(topicAck, 1);
  mqttClient.subscribe(topicTime);
  mqttClient.subscribe(topicCacheVersion, 1);
  mqttClient.subscribe(topicCacheUpdate, 1);
  mqttClient.subscribe(topicOccupiedResponse);
  mqttClient.subscribe(topicCmd, 1);

  mqttClient.publish(topicOnline, "online", true);
  if (!occupancyKnown) mqttClient.publish(topicOccupiedRequest, "pull");
  publishStatus();
  lastQueueTry = 0;      // flush the flash queue right away
  lastCachePoll = 0;
  return true;
}

// Never blocks for long: Wi-Fi reconnects in the background and the broker is
// retried on a timer, so the gates stay responsive while the network is down.
void maintainConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    if (mqttOnline) {
      mqttOnline = false;
      Serial.println("[WiFi] Connection lost. Running standalone, events go to flash.");
    }
    if (millis() - lastWifiTry > WIFI_RETRY_MS) {
      lastWifiTry = millis();
      Serial.println("[WiFi] Reconnecting in background...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    return;
  }

  if (mqttClient.connected()) return;

  if (mqttOnline) {
    mqttOnline = false;
    Serial.println("[MQTT] Broker connection lost. Running standalone, events go to flash.");
  }
  if (millis() - lastMqttTry > mqttRetryMs) connectToMQTT();
}

// Keeps MQTT alive. Safe to call from inside waiting loops.
void serviceNetwork(bool allowReconnect) {
  if (allowReconnect) maintainConnection();
  if (mqttClient.connected()) mqttClient.loop();
  checkPendingAcks();
}

// =======================================================
// GATES
// =======================================================
void moveServo(Servo& servo, int from, int to) {
  int step = from < to ? 1 : -1;
  for (int pos = from; pos != to + step; pos += step) {
    servo.write(pos);
    delay(15);
  }
}

// Opens the barrier, waits for the vehicle, and closes only when the "after"
// sensor reports that the vehicle has physically left the gate area.
// uid is empty for a remote open from the dashboard.
void runGateCycle(bool isEntry, const String& uid, const String& owner) {
  Servo& servo = isEntry ? servoEntry : servoExit;
  int trigAfter = isEntry ? TRIG_ENTRY_AFTER : TRIG_EXIT_AFTER;
  int echoAfter = isEntry ? ECHO_ENTRY_AFTER : ECHO_EXIT_AFTER;
  const char* name = isEntry ? "ENTRY" : "EXIT";
  bool& gateOpen = isEntry ? entryGateOpen : exitGateOpen;

  Serial.printf("[%s GATE] Opening...\n", name);
  beepGateOpen();
  moveServo(servo, 0, 90);
  gateOpen = true;
  publishStatus();

  bool carSeen = false;
  unsigned long openedAt = millis();
  while (millis() - openedAt < GATE_TIMEOUT_MS) {
    if (isCarPresent(trigAfter, echoAfter)) {
      carSeen = true;
      break;
    }
    serviceNetwork(false);
    delay(20);
  }

  if (carSeen) {
    Serial.printf("[%s GATE] Vehicle in the gate area.\n", name);
    if (isEntry) showEntry("Vehicle Detected", "Please Proceed");
    else showExit("Vehicle Detected", "Please Proceed");

    // Stay open until the sensor is clear several times in a row.
    int clearReadings = 0;
    while (clearReadings < CLEAR_READINGS_NEEDED) {
      clearReadings = isCarPresent(trigAfter, echoAfter) ? 0 : clearReadings + 1;
      serviceNetwork(false);
      delay(50);
    }

    occupiedSpaces = constrain(occupiedSpaces + (isEntry ? 1 : -1), 0, totalSpaces);
    saveOccupancy();
    Serial.printf("[%s GATE] Vehicle passed. Occupancy %d/%d\n", name, occupiedSpaces, totalSpaces);
  } else {
    Serial.printf("[%s GATE] Nobody drove through.\n", name);
  }

  moveServo(servo, 90, 0);
  gateOpen = false;
  Serial.printf("[%s GATE] Closed.\n", name);

  if (uid.length() > 0) logEvent(isEntry ? 'E' : 'X', !isEntry, uid, owner, "", carSeen);
  publishStatus();
  refreshIdleLCDs();
}

// =======================================================
// RFID
// =======================================================
String getRFIDUID(MFRC522& rfid) {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) uid += ":";
  }
  uid.toUpperCase();
  return uid;
}

void flushRFID(MFRC522& rfid) {
  while (rfid.PICC_IsNewCardPresent()) {
    rfid.PICC_ReadCardSerial();
    rfid.PICC_HaltA();
  }
}

void handleCardScan(bool isEntry, MFRC522& rfid) {
  String cardUID = getRFIDUID(rfid);
  rfid.PICC_HaltA();
  const char* name = isEntry ? "ENTRY" : "EXIT";
  Serial.printf("\n>>> [%s] Card %s\n", name, cardUID.c_str());

  String owner = "";
  const char* reason = nullptr;
  if (!cacheReady)                                        reason = "cache_not_ready";
  else if (!checkLocalCache(cardUID, owner))              reason = "unknown_card";
  else if (isEntry && occupiedSpaces >= totalSpaces)      reason = "lot_full";
  // A valid card may always leave, even if the counter says the lot is empty.

  if (reason) {
    Serial.printf("[ACCESS] Denied: %s\n", reason);
    bool full = strcmp(reason, "lot_full") == 0;
    if (isEntry) showEntry(full ? "Parking FULL" : "INVALID CARD", full ? "Please wait" : "Access Denied!");
    else showExit("INVALID CARD", "Access Denied!");
    beepError();
    logEvent('D', !isEntry, cardUID, owner, reason, false);
    unsigned long shownAt = millis();
    while (millis() - shownAt < 1500) { serviceNetwork(false); delay(20); }
    flushRFID(rfid);
    refreshIdleLCDs();
    return;
  }

  Serial.printf("[ACCESS] Granted for %s\n", owner.c_str());
  if (isEntry) showEntry("Welcome " + owner, "Gate Opening...");
  else showExit("Goodbye " + owner, "Gate Opening...");
  beepSuccess();
  runGateCycle(isEntry, cardUID, owner);
  flushRFID(rfid);
}

// =======================================================
// SERIAL DIAGNOSTICS
// =======================================================
void dumpFile(const char* path) {
  if (!fsReady || !LittleFS.exists(path)) { Serial.printf("[FS] %s does not exist.\n", path); return; }
  File f = LittleFS.open(path, FILE_READ);
  if (!f) { Serial.printf("[FS] Cannot open %s.\n", path); return; }
  Serial.printf("\n--- %s (%d bytes) ---\n", path, (int)f.size());
  while (f.available()) Serial.write(f.read());
  Serial.println("\n--- end ---\n");
  f.close();
}

void handleSerialCommands() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toLowerCase();
  if (cmd.length() == 0) return;

  if (cmd == "dump") {
    dumpFile(OFFLINE_QUEUE);
  } else if (cmd == "count") {
    Serial.printf("[COUNT] %d events waiting in flash\n", countQueueLines());
  } else if (cmd == "clear") {
    File f = LittleFS.open(OFFLINE_QUEUE, FILE_WRITE);
    if (f) f.close();
    queuedCount = 0;
    Serial.println("[CLEAR] Offline queue erased.");
  } else if (cmd == "fsinfo") {
    Serial.printf("[FS] Used %u of %u bytes\n", (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  } else if (cmd == "cache") {
    Serial.printf("[CACHE] version %ld, ready=%s\n", cachedVersion, cacheReady ? "yes" : "no");
    dumpFile(VALID_UIDS_FILE);
  } else if (cmd == "refresh") {
    cachedVersion = -1;      // forces the backend to send the whitelist again
    lastCachePoll = 0;
    Serial.println("[REFRESH] Whitelist will be requested again.");
  } else if (cmd == "sync") {
    lastQueueTry = 0;
  } else if (cmd == "status" || cmd == "version") {
    Serial.printf("[STATUS] WiFi=%s MQTT=%s | occupancy %d/%d | whitelist v%ld (%s) | queued %d | clock %s | boot #%u\n",
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "down",
                  mqttOnline ? "online" : "offline",
                  occupiedSpaces, totalSpaces, cachedVersion, cacheReady ? "ready" : "empty",
                  queuedCount, clockSynced ? "synced" : "unknown", (unsigned)bootCount);
  } else if (cmd == "open entry") {
    runGateCycle(true, "", "");
  } else if (cmd == "open exit") {
    runGateCycle(false, "", "");
  } else if (cmd == "help") {
    Serial.println("Commands: status | dump | count | clear | sync | cache | refresh | fsinfo | open entry | open exit | help");
  } else {
    Serial.printf("[?] Unknown command '%s'. Type 'help'.\n", cmd.c_str());
  }
}

// =======================================================
// SETUP
// =======================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println(" SMART PARKING - MQTT EDGE CONTROLLER");
  Serial.println("========================================\n");

  // --- persistent state ---
  prefs.begin("parking", false);
  bootCount = prefs.getUInt("boot", 0) + 1;
  prefs.putUInt("boot", bootCount);
  totalSpaces = prefs.getInt("total", 10);
  int storedOccupied = prefs.getInt("occ", -1);
  occupancyKnown = storedOccupied >= 0;
  occupiedSpaces = occupancyKnown ? constrain(storedOccupied, 0, totalSpaces) : 0;
  cachedVersion = prefs.getLong("cver", -1);

  fsReady = LittleFS.begin(true);
  if (!fsReady) {
    Serial.println("[FS] ERROR: LittleFS mount failed!");
  } else {
    LittleFS.remove(QUEUE_TMP);
    LittleFS.remove(VALID_UIDS_TMP);
    queuedCount = countQueueLines();
    cacheReady = cachedVersion >= 0 && LittleFS.exists(VALID_UIDS_FILE);
    if (!cacheReady) cachedVersion = -1;
    Serial.printf("[FS] Ready. %d events queued, whitelist %s.\n", queuedCount, cacheReady ? "found" : "missing");
  }

  // --- hardware first, so the gates work even if the network never comes up ---
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN);
  rfidEntry.PCD_Init();
  rfidExit.PCD_Init();

  servoEntry.attach(SERVO_ENTRY_PIN);
  servoExit.attach(SERVO_EXIT_PIN);
  servoEntry.write(0);
  servoExit.write(0);

  lcdEntry.init(); lcdEntry.backlight();
  lcdExit.init();  lcdExit.backlight();

  ledcSetup(BUZZER_CHANNEL, 2000, 8);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);

  pinMode(TRIG_ENTRY_BEFORE, OUTPUT); pinMode(ECHO_ENTRY_BEFORE, INPUT);
  pinMode(TRIG_ENTRY_AFTER,  OUTPUT); pinMode(ECHO_ENTRY_AFTER,  INPUT);
  pinMode(TRIG_EXIT_BEFORE,  OUTPUT); pinMode(ECHO_EXIT_BEFORE,  INPUT);
  pinMode(TRIG_EXIT_AFTER,   OUTPUT); pinMode(ECHO_EXIT_AFTER,   INPUT);

  showEntry("Smart Parking", "Starting...");
  showExit("Smart Parking", "Starting...");

  // --- network ---
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(3);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiTry = millis();
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {   // max 10 s at boot
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected, IP %s\n", WiFi.localIP().toString().c_str());
    connectToMQTT();
  } else {
    Serial.println("\n[WiFi] Not available. Starting standalone, will keep retrying.");
  }

  if (!cacheReady) {
    Serial.println("[BOOT] No card whitelist in flash yet: gates stay closed until the backend sends one.");
  }
  refreshIdleLCDs();

  Serial.printf("\n[SYSTEM] Ready. Broker: %s | Whitelist: %s | Occupancy %d/%d\n",
                mqttOnline ? "ONLINE" : "OFFLINE",
                cacheReady ? "READY" : "MISSING",
                occupiedSpaces, totalSpaces);
  Serial.println("Type 'help' for diagnostics.");
  Serial.println("========================================\n");
}

// =======================================================
// LOOP
// =======================================================
void loop() {
  handleSerialCommands();
  serviceNetwork(true);

  if (deferredCommand.length() > 0) {
    String cmd = deferredCommand;
    deferredCommand = "";
    handleCommand(cmd);
  }

  if (mqttOnline) {
    if (millis() - lastStatus > STATUS_INTERVAL_MS) publishStatus();

    // Ask for the whitelist when ours differs from the server's (or we have none).
    if (millis() - lastCachePoll > CACHE_POLL_INTERVAL_MS || lastCachePoll == 0) {
      lastCachePoll = millis();
      mqttClient.publish(topicCachePoll, String(cachedVersion).c_str());
    }

    if (queuedCount > 0 && (millis() - lastQueueTry > QUEUE_RETRY_MS || lastQueueTry == 0)) {
      lastQueueTry = millis();
      syncOfflineQueue();
    }
  }

  refreshIdleLCDs();

  if (rfidEntry.PICC_IsNewCardPresent() && rfidEntry.PICC_ReadCardSerial()) {
    handleCardScan(true, rfidEntry);
  }
  if (rfidExit.PICC_IsNewCardPresent() && rfidExit.PICC_ReadCardSerial()) {
    handleCardScan(false, rfidExit);
  }

  delay(50);
}
