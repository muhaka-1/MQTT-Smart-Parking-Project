
# 🚗 Automated IoT Smart Parking System with MQTT & Edge Resilience

> A real-time parking management system designed using ESP32 (Edge Client), MQTT Broker (Central Communication), LCD Display with I2C, SPI, Wi-Fi (TCP/IP), RFID sensors, and Servo-controlled barriers..

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

# 🛠  Hardware Components
Component,Purpose
ESP32 Dev Kit V1,Main controller (Edge Client)
RFID RC522 (SPI),Secure vehicle authentication
Ultrasonic Sensors,Detects vehicle arrival and gate passage
LCD Display (16x2),Real-time user interface (I2C)
Servo Motor (MG90S),Physical barrier/gate control
Buzzer,Acoustic feedback for scans and errors
MB102 Module,Dedicated power supply for Servos
Logic Converters,Protects ESP32 pins from 5V ultrasonic signals

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
# Automatiserat IoT-SmartParkeringssystem med Edge Computing
> A real-time parking management system designed using ESP32 – Klient (Edge), Raspberry Pi – Server (Edge), LCD Display with I2C module, SPI, Wi-Fi (TCP/IP), RFID sensor, Redis with offline support and Servo motor.
>

📜 Project-Overview

The Smart Parking System addresses common parking challenges such as traffic congestion, inefficient parking space usage, and helping  users to find parking slots faster and reducing urban traffic congestion. The purpose of this project is to design and implement a robust and automated Smart Parking System using IoT and Edge Computing principles to maintain reliable operation even during network failures. It detects parking slot occupancy, automates gate control, and displays availability status.

![System Overview]()
<img width="612" height="792" alt="System_Overview_Design_page_1" src="https://github.com/user-attachments/assets/af62bb73-367e-40c2-b27d-7cfa71aad8e8" />

The system uses:

     * ESP32 for hardware control (sensors & actuators)
     * Raspberry Pi Zero 2 W as an Edge backend server
     * Redis (Dockerized) for fast in-memory data management
     * LittleFS buffering for offline resilience

The architecture ensures low latency, scalability, and robustness for small- to medium-sized parking facilities.

### 🧩Key Features

* **RFID Authentication:** Automated gate access through RFID authentication with RC522 reader.
* **Vehicle Detection:** Ultrasonic sensors ensure automated gate opening/closing.
* **Edge Database:** Local Redis database running in Docker on a Raspberry Pi Zero 2 W.
* **Visual Feedback:** LCD screens and servo-controlled barriers.
* * **LCD display:** Real-time slot status updates
* * **Offline support:** Ensures continuous operation even during network outages through intelligent offline buffering and synchronization. Data is buffered locally on the ESP32 (LittleFS) in case of network failure and automatically synchronized when the connection is resumed.
* *Low-cost and scalable design
It is ideal for small to medium-scale parking facilities like shopping malls, offices, and urban centers.



### 🎯 Objectives

* Minimize the time required to locate available parking slots
* Automate gate access using RFID-based authentication
* Provide real-time parking availability updates
* Ensure reliable operation with offline data buffering
* Improve parking efficiency and reduce urban congestion

### 🏗 System Architecture

The system follows a Client–Server Edge Architecture:

          1 Client (Edge Device): ESP32 microcontroller controls sensors and actuators.
          
          2 Server (Edge Backend): Raspberry Pi runs Redis database in Docker.
          
          3 Entry/Exit Detection: RFID sensors detect incoming and outgoing vehicles.
          
          4 Slot Monitoring: Each parking slot monitored via dedicated IR sensors.
          
          5 Real-Time Display: LCD shows available parking slots dynamically.
          
          6 Barrier Control: Servo motor opens or closes gate based on availability.
          
          7 Power Management: Stable 5V supply ensures reliability.
          
          8 Communication is handled over Wi-Fi using TCP/IP protocols.
          
          9 Offline data handling is implemented through local storage and synchronization.

The separation between hardware control (frontend) and data management (backend) improvesodularity, scalability, and maintainability.


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




### 💻 Software & Technology Stack

* Programming Language: C++ (Arduino Framework via PlatformIO)

* Database: Redis (running in Docker container)

* File System: LittleFS (local buffering on ESP32)

* Communication Protocols:

     Wi-Fi (TCP/IP)

     SPI (RFID)

     I2C (LCD display)
* Deployment: Docker (Redis container)



### 🔌 Technical Design Considerations
Edge Computing Implementation

The system processes authentication and slot logic locally on the ESP32, reducing latency and server dependency.

Offline Support (Store and Forward)

In case of network failure:

Data is temporarily stored in /queue.txt using LittleFS.

When connection is restored, buffered data is automatically synchronized with Redis.

Voltage Compatibility

Since ESP32 operates at 3.3V and certain sensors operate at 5V:

Voltage dividers (1kΩ/2kΩ) are used on Echo pins.

Power Stability

Servo motors are powered through a dedicated MB102 power supply.

Capacitors are used to prevent brownout resets.

I2C Optimization

The LCD display uses an I2C module to minimize GPIO usage and simplify wiring.



### 📋 Installation Instructions

1. Wiring:

      Connect IR sensors to Digital Pins 2, 4, 5–10.
      Connect Servo Motor signal wire to Digital Pin 3.
      Connect LCD via I2C (SDA → A4, SCL → A5).
      RFID → SPI
      LCD → I2C (SDA, SCL)
      Ultrasonic Sensors → Digital pins
      Servo → Dedicated 5V supply
   
3. Power Setup:

      Separate power supply (MB102) for servo motors

      Shared ground between ESP32 and motor supply

      Voltage divider (1kΩ / 2kΩ) for 5V → 3.3V signal protection

      Capacitors added to prevent brownout resets

4. Backend Setup (Raspberry Pi)

Install Docker and run Redis:

docker run -d \
  --name redis-stack \
  -p 6379:6379 \
  -v redis-data:/data \
  redis/redis-stack:latest


Verify:

docker ps


5. ESP32 Setup

Clone repository

Open project in PlatformIO

Configure Wi-Fi credentials

Upload firmware to ESP32   
   
6. Upload Code:

    Use Arduino IDE.
   
    Install libraries:

         Wire.h
   
         LiquidCrystal_I2C.h
   
        Servo.h
   


### 🚀 Future Enhancements

          Mobile application integration (slot booking & navigation)
          EV charging station support
          Advanced analytics & reporting
          Mobile app (slot booking & navigation)
          Cloud integration for analytics
          License plate recognition
          EV charging station integration
          Predictive occupancy analysis


