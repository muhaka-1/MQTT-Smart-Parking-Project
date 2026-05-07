#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <PubSubClient.h> 
#include <map> 

// =======================================================
// 1. SETTINGS & VARIABLES
// =======================================================
const char* ssid = "Tele2Internet-B5596";
const char* password = "B648nLhA5a5";
const char* mqtt_broker = "192.168.8.131"; 
const int mqtt_port = 1883;
const char* queueFile = "/queue.txt";

// MQTT Topics
const char* topic_logs = "parking/logs";
const char* topic_access_request = "parking/access/request";

int freeSpots = 100; 
WiFiClient espClient;
PubSubClient mqttClient(espClient);

std::map<String, unsigned long> parkingTimes;

// =======================================================
// 2. PIN DEFINITIONS
// =======================================================
#define BUZZER_PIN 15 
#define BUZZER_CHANNEL 7  
#define RST_PIN 27
#define SS_IN_PIN 5
#define SS_OUT_PIN 4

#define SERVO_IN_PIN 13
#define SERVO_OUT_PIN 14

#define TRIG_IN_BEFORE 32
#define ECHO_IN_BEFORE 34
#define TRIG_IN_AFTER 33
#define ECHO_IN_AFTER 35

#define TRIG_OUT_BEFORE 25
#define ECHO_OUT_BEFORE 2
#define TRIG_OUT_AFTER 16 
#define ECHO_OUT_AFTER 15

// =======================================================
// 3. OBJECTS & STATES
// =======================================================
LiquidCrystal_I2C lcdIn(0x27, 16, 2); 
LiquidCrystal_I2C lcdOut(0x26, 16, 2); 

MFRC522 rfidIn(SS_IN_PIN, RST_PIN);
MFRC522 rfidOut(SS_OUT_PIN, RST_PIN);

Servo servoIn;
Servo servoOut;

bool gateInOpen = false;
unsigned long gateInTimer = 0;

bool gateOutOpen = false;
unsigned long gateOutTimer = 0;

bool carAtInBefore = false;
bool carAtOutBefore = false;
unsigned long lastSensorCheck = 0;

// =======================================================
// 4. HARDWARE HELPER FUNCTIONS
// =======================================================

long getDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW); delayMicroseconds(2);
  digitalWrite(trigPin, HIGH); delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  
  long duration = pulseIn(echoPin, HIGH, 20000); 
  if (duration == 0) return 999;
  return duration * 0.034 / 2;
}

void initBuzzer() {
  ledcSetup(BUZZER_CHANNEL, 2000, 8);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
  ledcWrite(BUZZER_CHANNEL, 0);
}

void beep(int duration, int times = 1) {
  for (int i = 0; i < times; i++) {
    ledcWriteTone(BUZZER_CHANNEL, 2000);
    delay(duration);
    ledcWrite(BUZZER_CHANNEL, 0);
    if (times > 1 && i < times-1) delay(100);
  }
}

void initLCD() {
  Wire.begin(21, 22);
  lcdIn.init(); lcdIn.backlight();
  lcdOut.init(); lcdOut.backlight();
}

void updateDisplays() {
  lcdIn.setCursor(0, 0);  lcdIn.print("Spots: " + String(freeSpots) + "    ");
  lcdIn.setCursor(0, 1);  lcdIn.print("Scan card...    ");
  
  lcdOut.setCursor(0, 0); lcdOut.print("Spots: " + String(freeSpots) + "    ");
  lcdOut.setCursor(0, 1); lcdOut.print("Scan card...    ");
}

String getUID(MFRC522 &rfid) {
  String uidStr = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    uidStr += String(rfid.uid.uidByte[i], HEX);
  }
  return uidStr;
}

// =======================================================
// 5. NETWORK & MQTT LOGIC
// =======================================================

void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (mqttClient.connect("ESP32ParkingClient")) {
      Serial.println(" connected");
    } else {
      Serial.print(" failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" retrying in 5 seconds");
      delay(5000);
    }
  }
}

void connectWiFi() {
  lcdIn.clear(); lcdIn.print("WiFi: Connecting");
  Serial.print("Connecting to Wi-Fi...");
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 10) {
    delay(500); Serial.print("."); attempts++;
  }
  lcdIn.clear();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" OK!"); lcdIn.print("WiFi: OK!");
    lcdOut.clear(); lcdOut.print("IP: " + WiFi.localIP().toString());
    mqttClient.setServer(mqtt_broker, mqtt_port);
  } else {
    Serial.println(" OFFLINE!"); lcdIn.print("WiFi: OFFLINE");
    lcdOut.clear(); lcdOut.print("Local Mode");
  }
  delay(2000); 
}

bool checkAccess(String uid) {
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) reconnectMQTT();
    String msg = "Access Request UID: " + uid;
    mqttClient.publish(topic_access_request, msg.c_str());
    return true; // Logic defaults to true unless server sends command to block
  }
  return true; 
}

void logPassage(String uid, String direction) {
  String logData = uid + "," + direction + "," + String(millis());
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) reconnectMQTT();
    mqttClient.publish(topic_logs, logData.c_str());
  } else {
    File file = LittleFS.open(queueFile, FILE_APPEND);
    if (file) { file.println(logData); file.close(); }
  }
}

void syncOfflineData() {
  if (WiFi.status() == WL_CONNECTED && LittleFS.exists(queueFile)) {
    if (!mqttClient.connected()) reconnectMQTT();
    File file = LittleFS.open(queueFile, FILE_READ);
    while (file.available()) {
      String logData = file.readStringUntil('\n');
      logData.trim();
      if (logData.length() > 0) mqttClient.publish(topic_logs, logData.c_str());
    }
    file.close(); 
    LittleFS.remove(queueFile); 
  }
}

// =======================================================
// 6. SETUP 
// =======================================================
void setup() {
  Serial.begin(115200);
  delay(1000); 
  
  initBuzzer(); 
  initLCD();
  lcdIn.clear(); lcdIn.print("Loading System..");
  delay(1000);

  Serial.println("\n=== SYSTEM BOOT ===");

  pinMode(SS_IN_PIN, OUTPUT); digitalWrite(SS_IN_PIN, HIGH);
  pinMode(SS_OUT_PIN, OUTPUT); digitalWrite(SS_OUT_PIN, HIGH);
  SPI.begin(18, 19, 23, 27);
  rfidIn.PCD_Init();
  rfidOut.PCD_Init();

  ESP32PWM::allocateTimer(0); ESP32PWM::allocateTimer(1);
  servoIn.setPeriodHertz(50); servoOut.setPeriodHertz(50);
  servoIn.attach(SERVO_IN_PIN, 500, 2400); 
  servoOut.attach(SERVO_OUT_PIN, 500, 2400);
  servoIn.write(0); servoOut.write(0); 

  pinMode(TRIG_IN_BEFORE, OUTPUT); pinMode(ECHO_IN_BEFORE, INPUT);
  pinMode(TRIG_IN_AFTER, OUTPUT); pinMode(ECHO_IN_AFTER, INPUT);
  pinMode(TRIG_OUT_BEFORE, OUTPUT); pinMode(ECHO_OUT_BEFORE, INPUT);
  pinMode(TRIG_OUT_AFTER, OUTPUT); pinMode(ECHO_OUT_AFTER, INPUT);

  LittleFS.begin(true);
  connectWiFi(); 
  syncOfflineData();

  updateDisplays();
  beep(100, 3); 
  Serial.println("\n=== SYSTEM READY ===");
}

// =======================================================
// 7. MAIN LOOP 
// =======================================================
void loop() {
  if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  // --- CAR DETECTION (Sensors) ---
  if (millis() - lastSensorCheck > 1000) { 
    long distInBefore = getDistance(TRIG_IN_BEFORE, ECHO_IN_BEFORE);
    long distInAfter  = getDistance(TRIG_IN_AFTER, ECHO_IN_AFTER);
    long distOutBefore = getDistance(TRIG_OUT_BEFORE, ECHO_OUT_BEFORE);
    long distOutAfter  = getDistance(TRIG_OUT_AFTER, ECHO_OUT_AFTER);

    Serial.print("IN - Before: "); Serial.print(distInBefore); Serial.print(" cm, After: "); Serial.print(distInAfter);
    Serial.print(" | OUT - Before: "); Serial.print(distOutBefore); Serial.print(" cm, After: "); Serial.print(distOutAfter);
    Serial.println(" cm");

    if (distInBefore <= 5 && !carAtInBefore) { carAtInBefore = true; beep(100); } 
    else if (distInBefore > 5) { carAtInBefore = false; }

    if (distOutBefore <= 5 && !carAtOutBefore) { carAtOutBefore = true; beep(100); } 
    else if (distOutBefore > 5) { carAtOutBefore = false; }
    
    lastSensorCheck = millis();
  }

  // --- ENTRANCE HANDLER ---
  if (!gateInOpen && rfidIn.PICC_IsNewCardPresent() && rfidIn.PICC_ReadCardSerial()) {
    beep(50);
    String uid = getUID(rfidIn);
    
    if (freeSpots <= 0) {
      lcdIn.clear(); 
      lcdIn.print("PARKING FULL!");
      lcdIn.setCursor(0, 1);
      lcdIn.print("No spots left");
      beep(500, 3);
    }
    else if (checkAccess(uid)) {
      freeSpots--;
      logPassage(uid, "IN");
      parkingTimes[uid] = millis();
      lcdIn.clear(); 
      lcdIn.print("Welcome!");
      lcdIn.setCursor(0, 1);
      lcdIn.print("Spots left: " + String(freeSpots));
      beep(100, 2);
      servoIn.write(90); 
      gateInOpen = true;
      gateInTimer = millis();
      updateDisplays();
    } 
    else {
      lcdIn.clear(); 
      lcdIn.print("Access Denied!");
      lcdIn.setCursor(0, 1);
      lcdIn.print("Invalid Card");
      beep(500);
    }
    rfidIn.PICC_HaltA();
  }

  // --- ENTRANCE GATE TIMEOUT ---
  if (gateInOpen && (millis() - gateInTimer > 10000)) { 
    if (getDistance(TRIG_IN_AFTER, ECHO_IN_AFTER) > 5) {
      servoIn.write(0); 
      gateInOpen = false;
      updateDisplays();
    } else {
      gateInTimer = millis(); 
    }
  }

  // --- EXIT HANDLER ---
  if (!gateOutOpen && rfidOut.PICC_IsNewCardPresent() && rfidOut.PICC_ReadCardSerial()) {
    beep(50);
    String uid = getUID(rfidOut);
    if (checkAccess(uid)) {
      freeSpots++;
      logPassage(uid, "OUT");

      String timeMsg = "Time unknown";
      if (parkingTimes.count(uid)) { 
        unsigned long parkedTimeMs = millis() - parkingTimes[uid];
        unsigned long totalSecs = parkedTimeMs / 1000;
        unsigned long mins = totalSecs / 60;
        unsigned long secs = totalSecs % 60;
        if (mins > 0) { timeMsg = "Time: " + String(mins) + "m " + String(secs) + "s"; } 
        else { timeMsg = "Time: " + String(secs) + " seconds"; }
        parkingTimes.erase(uid); 
      }
      lcdOut.clear(); 
      lcdOut.setCursor(0, 0); lcdOut.print("Thanks for visit");
      lcdOut.setCursor(0, 1); lcdOut.print(timeMsg);
      beep(100, 2);
      servoOut.write(90); 
      gateOutOpen = true;
      gateOutTimer = millis();
    } else {
      lcdOut.clear(); lcdOut.print("Access Denied!");
      beep(500); 
    }
    rfidOut.PICC_HaltA();
  }

  // --- EXIT GATE TIMEOUT ---
  if (gateOutOpen && (millis() - gateOutTimer > 10000)) {
    if (getDistance(TRIG_OUT_AFTER, ECHO_OUT_AFTER) > 5) {
      servoOut.write(0); 
      gateOutOpen = false;
      updateDisplays();
    } else {
      gateOutTimer = millis(); 
    }
  }
}