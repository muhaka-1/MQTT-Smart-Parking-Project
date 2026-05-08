
# 🚗 Automated IoT Smart Parking System with MQTT & Edge Resilience

> A real-time parking management system designed using ESP32 (Edge Client), MQTT Broker (Central Communication), LCD Display with I2C, SPI, Wi-Fi (TCP/IP), RFID sensors, and Servo-controlled barriers..

![System Overview]()
<img width="612" height="792" alt="System_Overview_Design_page_1" src="https://github.com/user-attachments/assets/af62bb73-367e-40c2-b27d-7cfa71aad8e8" />

# 📜 Project Overview

The Smart Parking System addresses common urban challenges such as traffic congestion and inefficient parking usage. By moving to an MQTT-based architecture, the system achieves high-speed, asynchronous communication between the sensors and the management backend.

The system ensures reliability through Edge Computing principles: it detects occupancy, automates gates via RFID, and displays real-time status, all while maintaining a local buffer (LittleFS) to prevent data loss during network outages.

The system uses:
ESP32: For hardware control (sensors & actuators).

MQTT Protocol: For lightweight, real-time data pub/sub.

PubSubClient: To handle messaging between the Edge and the Broker.

LittleFS: For "Store & Forward" offline resilience.

# 🧩 Key Features

✅ RFID Authentication – Automated gate access through secure UID verification.

✅ Vehicle Detection – Ultrasonic sensors (HC-SR04) for arrival detection and safe passage.

✅ MQTT Communication – Real-time logging and access requests via a central broker.

✅ Dual Feedback System – LCD screens for status and a buzzer for acoustic alerts.

✅ Offline Support (Store & Forward) – Data is buffered locally on ESP32 flash memory 
if the broker is unreachable and synced automatically upon reconnection.

✅ Hierarchical Logic – Efficient "Edge-first" processing for low-latency gate response.

# 🎯Objectives

Minimize time to find a free parking space

Automate entry and exit via RFID

Provide real-time availability updates

Ensure stable operation during network outages

Improve parking flow and reduce traffic congestion

# 🏗 System Architecture
The system follows an Event-Driven IoT Architecture:

Edge Device (ESP32): Monitors ultrasonic sensors and RFID readers.

Communication (MQTT): Acts as the central nervous system, passing logs and requests.

Entry/Exit Detection: RFID RC522 captures encrypted tags for authentication.

Safety Monitoring: Ultrasonic sensors prevent the servo barrier from closing while a car is still underneath.

Real-Time Display: Dual LCDs show available spots at both entrance and exit.

Power Management: Uses common ground and specific voltage rails (3.3V for RFID, 5V for Servos).

Resilience: If the MQTT broker fails, logic continues using local state, and logs are queued in LittleFS.

### 🛠 Hardware Components


| Component                        | Purpose                                           |
| -------------------------------- | ------------------------------------------------- |
| **ESP32**                        | Controls the entire system                        |
| **IR Sensors**                   | Detect vehicles at entry, exit, and parking slots |
| **ESP32 Dev Kit V1**             | Main controller (Client)                          |
| **Raspberry Pi Zero 2 W**        | Backend server (Edge)                             |
| **RFID RC522 (SPI)**             | Secure vehicle authentication                     |
| **Ultrasonic Sensors (HC-SR04)** | Vehicle and slot detection                        |
| **LCD Display (20x4)**           | Shows real-time slot availability                 |
| **I2C Module**                   | Simplifies LCD wiring                             |
| **Servo Motor (SG-90)**          | Opens and closes the barrier gate                 |
| **5V 2A Adapter**                | Powers ESP32 and connected modules                |
| **Breadboard & Wires**           | Assembly and prototyping                          |
| **MB102 Power Module**           | Stable external power supply for motors           |

# 💻 Software & Technology Stack
Language: C++ (Arduino Framework / PlatformIO)

Communication: MQTT (via PubSubClient)

Messaging Format: CSV/Plaintext (can be upgraded to JSON)

Local Storage: LittleFS (Flash File System)

Libraries: MFRC522, ESP32Servo, LiquidCrystal_I2C, PubSubClient
        * Wi-Fi (TCP/IP)
        
        * SPI (RFID)
        
        *I2C (LCD)

* Deployment: Docker (MQTT-container)

# 🔌 Technical Design Choices
MQTT vs Redis
Moving to MQTT allows the system to be asynchronous. The ESP32 doesn't have to wait for a database "write" to complete; it simply "publishes" an event and moves to the next task, significantly improving responsiveness.

Store & Forward (LittleFS)
If the WiFi.status() is not WL_CONNECTED or the MQTT client is disconnected:

Data is written to /queue.txt.

A background process checks for reconnection.

Once back online, syncOfflineData() reads the file and clears the queue.

Safety Logic
The barriers use a Timeout + Distance check. The gate only closes after 10 seconds IF the "After" sensor detects that the car has physically cleared the gate area, preventing damage to vehicles.

## In case of network failure:

Data is saved in /queue.txt via LittleFS

System continues to function normally

When reconnected, data is automatically synchronized with Redis

Voltage adaptation

## ESP32 operates at 3.3V while some sensors use 5V:

Voltage divider (1kΩ/2kΩ) is used for Echo signals

Current stability

Servo motors are driven via separate MB102 module

Common ground between ESP32 and motors

Capacitors prevent brownout reset

I2C optimization

LCD uses I2C to minimize GPIO usage and simplify wiring.

# 🚀 Challenges and Solutions

During development, the following critical challenges were identified and resolved:

| Challenge | Technical Solution |
|-----|-------------------|
| **Logic levels (3.3V vs 5V)** – ESP32 operates at 3.3V while some sensors use 5V | Voltage dividers (1kΩ / 2kΩ) were implemented on the Echo pins to protect the ESP32 |
| **Current spikes during motor load** – Microcontroller rebooted during servo activity | Separate MB102 power supply for motors as well as decoupling capacitors for stable voltage |
| **Data loss during network outage** | Implementation of *Store & Forward*: Data is buffered locally in `/queue.txt` via LittleFS and automatically synchronized when the connection is restored |

# 📋 Installation
🔧 Backend (Raspberry Pi)

Install Docker and start Redis:

docker run -d \
--name redis-stack \
-p 6379:6379 \
-v redis-data:/data \
redis/redis-stack:latest

Verify:

docker ps

⚙ ESP32

Clone repository

Open project in PlatformIO

Configure Wi-Fi tasks

Upload firmware to ESP32

#🔌 Wiring

RFID → SPI

LCD → I2C (SDA, SCL)

IR/Ultrasonic sensors → Digital pins

Servo → Dedicated 5V supply


# 🚀 Framtida Utveckling

📱 Mobilapplikation (bokning & navigering)

☁ Molnintegration för analys

🔍 Nummerplåtsigenkänning

🔌 EV-laddstationer

📊 Prediktiv analys av parkeringsbeläggning



# English 











