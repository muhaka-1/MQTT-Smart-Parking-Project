#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include <PubSubClient.h> 
#include <FS.h>
#include <LittleFS.h>

// --- CONFIGURATION ---
const char* ssid = "Wokwi-GUEST"; 
const char* password = "";
const char* mqtt_server = "192.168.1.XX"; // Replace with your Raspberry Pi IP
const int mqtt_port = 1883;

// --- OFFLINE SETTINGS ---
// "742B4912" is the default tag UID for Wokwi simulation testing
String localWhitelist[] = {"742B4912", "A1B2C3D4", "B2C3D4E5"}; 
const int whitelistSize = 3;
const char* LOG_FILE = "/offline_logs.txt";

// --- PIN MAPPING ---
#define BUZZER_PIN 2
#define RST_PIN 27      
#define SS_IN_PIN 5     
#define SS_OUT_PIN 4    
#define SERVO_IN_PIN 13
#define SERVO_OUT_PIN 14
#define TRIG_IN_AFTER 33
#define ECHO_IN_AFTER 35
#define TRIG_OUT_AFTER 26
#define ECHO_OUT_AFTER 15  

// --- OBJECTS ---
MFRC522 rfidIn(SS_IN_PIN, RST_PIN);
MFRC522 rfidOut(SS_OUT_PIN, RST_PIN);
Servo gateIn;
Servo gateOut;
LiquidCrystal_I2C lcdIn(0x27, 16, 2);  
LiquidCrystal_I2C lcdOut(0x3F, 16, 2); 

WiFiClient espClient;
PubSubClient mqttClient(espClient);

int occupiedSpaces = 0;
const int TOTAL_SPACES = 100;
bool accessGranted = false;

// --- DISPLAY & SOUND ---
void updateDisplays() {
    lcdIn.clear();
    lcdIn.setCursor(0, 0);
    lcdIn.print(WiFi.status() == WL_CONNECTED ? "Mode: Online" : "Mode: Offline");
    lcdIn.setCursor(0, 1);
    lcdIn.print("Spaces: ");
    lcdIn.print(TOTAL_SPACES - occupiedSpaces);

    lcdOut.clear();
    lcdOut.setCursor(0, 0);
    lcdOut.print("Smart Parking");
    lcdOut.setCursor(0, 1);
    lcdOut.print("Safe Travels!");
}

void triggerBuzzer(int ms) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(ms);
    digitalWrite(BUZZER_PIN, LOW);
}

// --- SENSOR LOGIC ---
long getDistance(int trig, int echo) {
    digitalWrite(trig, LOW); delayMicroseconds(2);
    digitalWrite(trig, HIGH); delayMicroseconds(10);
    digitalWrite(trig, LOW);
    long duration = pulseIn(echo, HIGH, 30000); 
    return (duration == 0) ? 999 : duration * 0.034 / 2;
}

// --- STORAGE & SYNC ---
void logOfflineAccess(String uid, String type) {
    File file = LittleFS.open(LOG_FILE, FILE_APPEND);
    if (!file) {
        Serial.println("Failed to open log file");
        return;
    }
    file.println(uid + "," + type);
    file.close();
    Serial.println("Stored offline: " + uid + " (" + type + ")");
}

void syncOfflineData() {
    if (!LittleFS.exists(LOG_FILE)) return;

    Serial.println("Syncing offline data to MQTT...");
    File file = LittleFS.open(LOG_FILE, FILE_READ);
    if (!file) return;

    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
            mqttClient.publish("parking/sync/offline", line.c_str());
            delay(50); // Prevent flooding
        }
    }
    file.close();
    LittleFS.remove(LOG_FILE);
    Serial.println("Sync complete. Logs cleared.");
}

bool checkLocalWhitelist(String uid) {
    for (int i = 0; i < whitelistSize; i++) {
        if (uid == localWhitelist[i]) return true;
    }
    return false;
}

// --- MQTT CALLBACK ---
void callback(char* topic, byte* payload, unsigned int length) {
    String message = "";
    for (int i = 0; i < length; i++) message += (char)payload[i];
    if (String(topic) == "parking/access/res" && message == "GRANTED") {
        accessGranted = true;
    }
}

// --- GATE PROCESSOR ---
void processGate(MFRC522 &rfid, LiquidCrystal_I2C &lcd, Servo &servo, int trigAfter, int echoAfter, String type) {
    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

    String uid = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
        uid += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
        uid += String(rfid.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();

    lcd.clear();
    lcd.print("ID: " + uid);
    lcd.setCursor(0, 1);
    lcd.print("Verifying...");
    
    bool allowed = false;
    accessGranted = false;

    // 1. Attempt Online Check
    if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
        mqttClient.publish("parking/access/req", uid.c_str());
        unsigned long startWait = millis();
        while (millis() - startWait < 1500) { // 1.5s timeout
            mqttClient.loop();
            if (accessGranted) {
                allowed = true;
                break;
            }
        }
    }

    // 2. Fallback to Offline Check
    if (!allowed) {
        if (checkLocalWhitelist(uid)) {
            allowed = true;
            logOfflineAccess(uid, type); // Save for later sync
            Serial.println("Allowed via Whitelist (Offline)");
        }
    }

    // 3. Action
    if (allowed) {
        triggerBuzzer(100);
        lcd.clear();
        lcd.print(type == "IN" ? "Access Granted" : "Goodbye!");
        
        servo.write(90); 
        Serial.println("Gate Open. Waiting for vehicle...");
        
        // Wait for car to trigger the ultrasonic sensor
        while (getDistance(trigAfter, echoAfter) > 15) { 
            delay(100); 
            // Safety: could add a timeout here
        }
        
        delay(2000); // Wait for vehicle to clear
        servo.write(0); 
        Serial.println("Gate Closed.");

        if (type == "IN") occupiedSpaces++;
        else if (occupiedSpaces > 0) occupiedSpaces--;
        
        updateDisplays();
    } else {
        triggerBuzzer(500);
        lcd.clear();
        lcd.print("Access Denied");
        delay(2000);
        updateDisplays();
    }
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
}

// --- SETUP & LOOP ---
void setup() {
    Serial.begin(115200);
    SPI.begin();

    // Initialize FS
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS Mount Failed");
    }
    
    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(TRIG_IN_AFTER, OUTPUT); pinMode(ECHO_IN_AFTER, INPUT);
    pinMode(TRIG_OUT_AFTER, OUTPUT); pinMode(ECHO_OUT_AFTER, INPUT);

    rfidIn.PCD_Init();
    rfidOut.PCD_Init();
    
    gateIn.attach(SERVO_IN_PIN);
    gateOut.attach(SERVO_OUT_PIN);
    gateIn.write(0);
    gateOut.write(0);

    lcdIn.init(); lcdIn.backlight();
    lcdOut.init(); lcdOut.backlight();

    WiFi.begin(ssid, password);
    mqttClient.setServer(mqtt_server, mqtt_port);
    mqttClient.setCallback(callback);
    
    updateDisplays();
}

void loop() {
    static unsigned long lastReconnectAttempt = 0;
    
    if (WiFi.status() == WL_CONNECTED) {
        if (!mqttClient.connected()) {
            unsigned long now = millis();
            if (now - lastReconnectAttempt > 5000) {
                lastReconnectAttempt = now;
                if (mqttClient.connect("ESP32_Parking_System")) {
                    Serial.println("MQTT Connected");
                    mqttClient.subscribe("parking/access/res");
                    syncOfflineData(); // Push saved logs to Pi
                    updateDisplays();
                }
            }
        }
    }

    mqttClient.loop();
    processGate(rfidIn, lcdIn, gateIn, TRIG_IN_AFTER, ECHO_IN_AFTER, "IN");
    processGate(rfidOut, lcdOut, gateOut, TRIG_OUT_AFTER, ECHO_OUT_AFTER, "OUT");
    
    // Refresh display status occasionally if WiFi state changes
    static long lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 10000) {
        lastWifiCheck = millis();
        updateDisplays();
    }
}