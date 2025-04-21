# ESP32 Audio Recording and FTP Upload System

## Project Overview

This project is an ESP32-based audio recording system that captures audio data through a microphone, saves it in WAV format to an SD card, and uploads the recorded files to a specified server via FTP. The system is designed to enter deep sleep mode after recording to conserve power and automatically wake up at predetermined intervals to continue operation.

## Features

- Audio data collection using ESP32's I2S interface
- WAV format encoding and storage on SD card
- WiFi connectivity for network operations
- System time synchronization via SNTP
- FTP protocol for uploading audio files to NAS or FTP server
- Deep sleep functionality for low power consumption
- Automatic deletion of successfully uploaded local files to free SD card space
- Error handling and system restart mechanisms

## Hardware Requirements

- ESP32 development board (such as ESP-LYRA or other ESP32 boards with audio codec)
- SD card and reader module
- Microphone (compatible with ESP32 audio codec)
- Power supply (battery or USB)

## Software Dependencies

- ESP-IDF framework
- ESP-ADF (Audio Development Framework)
- FTP client library
- FAT file system

## Configuration Parameters

Key parameters that can be adjusted in the code:

- `WAKEUP_TIME_SECONDS`: Deep sleep duration (seconds)
- `RECORD_TIME_SECONDS`: Recording duration per session (seconds)
- WiFi connection parameters (SSID and password)
- FTP server configuration (defined through CONFIG_FTP_SERVER, etc.)
- FTP upload path

## Usage Instructions

1. Install ESP-IDF and ESP-ADF development environments
2. Configure FTP server parameters
3. Set up WiFi connection information
4. Compile and upload the code to ESP32
5. Connect external SD card and ensure microphone is working properly
6. The system will automatically start the work cycle: recording → saving → uploading → deep sleep
