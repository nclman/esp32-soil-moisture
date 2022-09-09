# ESP32 Soil Moisture Project
This project is aimed to make a small (and hopefully, self-sustaining) automated plant watering device suitable for a single potted plant. But it should be easily scalable to multiple pots if so desired.
The main hardware components are rather cheap and are all sourced online:
  - ESP32-S2-Mini
  - Capacitive soil moisture sensor
  - 5V DC water pump

If you want to make your device cordless:
  - 18650 LiOn battery
  - Battery charging module (3-way: Solar, battery, output)
  - 5-6V solar panel
  - 5V - 3.3V step down module (3.3V for ESP32-S2-Mini power supply)

The device is WiFi-enabled and it can send logs to your Firebase Realtime Database periodically

## Prerequisites
### Hardware
- ESP32-S2-Mini or any ESP32-based devkit should work
- USB connection to your host computer

### Software
- Depending on your ESP32 device, install the appropriate USB-serial driver
- Arduino IDE
  - Install ESP32 boards package from "Boards Manager" in Arduino IDE
- Arduino libraries:
  - "Firebase_Arduino_Client_Library_for_ESP8266_and_ESP32"
  - "FirebaseJson"
  - "ESP32Time"
  
### Others
- Mad soldering skills

## Why ESP32-S2-Mini?
Mostly because of its small form-factor, as I wanted to fit everything in a small water-resistant casing. Most of the ESP32 dev boards are so cheap, some can be had for about USD 2, 3 dollars, yet has WiFi, some with Bluetooth LE (BLE), Real-Time Clock (RTC), multiple analogue-digital converters (ADC) and GPIOs, very low deep-sleep current, what's not to like?
It's also very easy to use with Arduino IDE.

There is nothing in this Arduino sketch that is peculiar to ESP32-S2-Mini. So, it should work with other ESP32 hardware, maybe with a change of the pins used for ADC and GPIO output pin.

