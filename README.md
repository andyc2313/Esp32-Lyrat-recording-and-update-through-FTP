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

## Files to Upload

Based on the provided code, you need to upload the following files:

1. `pipeline_wav_amr_sdcard.c` - Main program file
2. `FtpClient.h` - FTP client header file (referenced in the code)
3. `FtpClient.c` - FTP client implementation (referenced in the code)
4. `sdkconfig` or `sdkconfig.h` - Contains configurations like `CONFIG_FTP_SERVER`, `CONFIG_FTP_USER`, `CONFIG_FTP_PASSWORD`
5. `CMakeLists.txt` - Project build configuration file (for ESP-IDF build system)
6. `partitions.csv` (if using custom partition table)
7. `README.md` - Project documentation

If you're using an ESP-ADF framework example as a base, you may also need to ensure that relevant component configuration files are uploaded.

## Notes

- Ensure the FTP server has sufficient storage space
- Check WiFi connection stability
- Monitor SD card usage
- Ensure stable power supply, especially when battery-powered

## Troubleshooting

- If the system fails to upload files, check WiFi connection and FTP server configuration
- If recording quality is poor, check microphone connection and I2S configuration
- If the device frequently restarts, check if the power supply is stable
- View log output to diagnose specific issues

## Improvement Suggestions

- Add web interface for remote configuration
- Implement local file management mechanism
- Add audio pre-processing capabilities
- Optimize power management to extend battery life# Esp32-Lyrat-recording-and-update-through-FTP
