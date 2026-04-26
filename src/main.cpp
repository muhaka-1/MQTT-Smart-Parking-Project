#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include <PubSubClient.h> 

// --- CONFIGURATION ---
const char* ssid = "Wokwi-GUEST"; // Default Wokwi WiFi
const char* password = "";
const char* mqtt_server = "192.168.1.100"; 
const int mqtt_port = 1883;

// --- OFFLINE SETTINGS ---
// "742B4912" is the default UID for the first tag in Wokwi
String localWhitelist[] = {"742B4912", "A1B2C3D4"}; 
const int whitelistSize = 2;

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

long getDistance(int trig, int echo) {
    digitalWrite(trig, LOW); delayMicroseconds(2);
    digitalWrite(trig, HIGH); delayMicroseconds(10);
    digitalWrite(trig, LOW);
    long duration = pulseIn(echo, HIGH, 30000); 
    return (duration == 0) ? 999 : duration * 0.034 / 2;
}

bool checkLocalAccess(String uid) {
    for (int i = 0; i < whitelistSize; i++) {
        if (uid == localWhitelist[i]) return true;
    }
    return false;
}

void callback(char* topic, byte* payload, unsigned int length) {
    String message = "";
    for (int i = 0; i < length; i++) message += (char)payload[i];
    if (String(topic) == "parking/access/res" && message == "GRANTED") accessGranted = true;
}

void processGate(MFRC522 &rfid, LiquidCrystal_I2C &lcd, Servo &servo, int trigAfter, int echoAfter, String type) {
    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

    String uid = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
        uid += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
        uid += String(rfid.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();

    Serial.print("Detected UID: ");
    Serial.println(uid);

    lcd.clear();
    lcd.print("Checking...");
    
    bool canEnter = false;
    accessGranted = false;

    // Try Online Mode
    if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
        mqttClient.publish("parking/access/req", uid.c_str());
        unsigned long startWait = millis();
        while (millis() - startWait < 1500) {
            mqttClient.loop();
            if (accessGranted) {
                canEnter = true;
                break;
            }
        }
    }

    // Fallback to Offline Mode
    if (!canEnter) {
        if (checkLocalAccess(uid)) {
            canEnter = true;
            Serial.println("Offline Access Granted");
        }
    }

    if (canEnter) {
        triggerBuzzer(100);
        lcd.clear();
        lcd.print(type == "IN" ? "Welcome!" : "Goodbye!");
        
        servo.write(90); 
        Serial.println("Gate Opening. Waiting for car...");
        
        // In Wokwi, move the slider of the Ultrasonic sensor to < 15cm to simulate car passing
        while (getDistance(trigAfter, echoAfter) > 15) { delay(100); }
        
        delay(2000); 
        servo.write(0); 
        Serial.println("Gate Closed.");

        if (type == "IN") occupiedSpaces++;
        else if (occupiedSpaces > 0) occupiedSpaces--;
        
        updateDisplays();
    } else {
        triggerBuzzer(500);
        lcd.clear();
        lcd.print("Access Denied");
        Serial.println("Access Denied");
        delay(2000);
        updateDisplays();
    }
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
}

void setup() {
    Serial.begin(115200);
    SPI.begin();
    
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
    static unsigned long lastReconnect = 0;
    if (!mqttClient.connected() && WiFi.status() == WL_CONNECTED) {
        if (millis() - lastReconnect > 5000) {
            mqttClient.connect("ESP32_Parking_System");
            mqttClient.subscribe("parking/access/res");
            lastReconnect = millis();
        }
    }
    
    mqttClient.loop();
    processGate(rfidIn, lcdIn, gateIn, TRIG_IN_AFTER, ECHO_IN_AFTER, "IN");
    processGate(rfidOut, lcdOut, gateOut, TRIG_OUT_AFTER, ECHO_OUT_AFTER, "OUT");
}