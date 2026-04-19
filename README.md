# ESP32-S3 Internet Radio (Waveshare 1.54" LCD)

A standalone Wi-Fi internet radio built on the **Waveshare ESP32-S3 Touch LCD 1.54"** board using Arduino.

<p align="left">
  <img src="/ESP32-S3_1.54inch_Internet-Radio_Waveshare_MiroKod.jpg" width="400">
</p>

---

## ✨ Features

- Wi-Fi internet radio
- Playlist loaded from SD card
- Time synchronization via NTP
- Battery monitoring with visual indicator
- Clean UI with grouped panels
- Persistent last station (stored in NVS)
- Deep sleep power saving

---

## 🧰 Hardware

- Board: Waveshare ESP32-S3 Touch LCD 1.54"
- Audio codec: ES8311 (onboard)
- Display: ST7789 (240x240)
- SD card required

## Documentation:
- https://www.waveshare.com/esp32-s3-lcd-1.54.htm
- https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54/Instructions-For-Use

---

## ⚙️ Software Setup (Arduino IDE)

### Install Board Support
- Install **ESP32 by Espressif Systems** via Boards Manager

### Install Libraries
- ESP32-audioI2S
- LovyanGFX

---

## 🔧 Arduino Configuration

Set the following:
- Board: ESP32S3 Dev Module
- USB CDC On Boot: Enabled
- CPU Frequency: 240 MHz (WiFi)
- Core Debug Level: None
- USB DFU On Boot: Disabled
- Erase All Flash Before Sketch Upload: Disabled
- Events Run On: Core 1
- Flash Mode: QIO 80MHz
- Flash Size: 16MB (128Mb)
- JTAG Adapter: Disabled
- Arduino Runs On: Core 1
- USB Firmware MSC On Boot: Disabled
- Partition Scheme: 16M Flash (3MB APP/9.9MB FATFS)
- PSRAM: OPI PSRAM
- Upload Mode: UART0 / Hardware CDC
- Upload Speed: 921600
- USB Mode: Hardware CDC and JTAG
- Zigbee Mode: Disabled


---

## 💾 SD Card Setup

Create **3 files in the root directory**:

### `/wifi.txt`
MyWiFi|password123

PhoneHotspot|12345678


---

### `/stations.txt`
Cas Rock|http://icecast6.play.cz/casrock128.mp3

Radio Paradise|https://stream.radioparadise.com/mp3-192

BBC World Service|http://stream.live.vc.bbcmedia.co.uk/bbc_world_service


---

### `/timezone.txt`
CET-1CEST,M3.5.0/2,M10.5.0/3


---

## 🧠 How It Works

### Boot Process
1. Initialize display and audio codec
2. Mount SD card
3. Load:
   - Wi-Fi networks
   - Radio stations
   - Timezone
   - Last station (from NVS)
4. Connect to Wi-Fi
5. Synchronize time via NTP
6. Start audio stream

---

## 📺 UI Layout

### Left Panel
- Station list (paginated)
- Selected station highlighted

### Right Panel

#### Top Box
- Wi-Fi status
- Battery bar
- Battery voltage

#### Middle Box
- Time (HH:MM)
- Date (DD.MM.YYYY)

#### Bottom Box
- Volume label
- Volume bar
- Playback state (PLAY / PAUSE / STOP)

---

## 🎛 Controls

### GPIO 5 (Station Button)
- Short press → Next station
- Long press → Previous station

### GPIO 4 (Volume Button)
- Short press → Volume up
- Long press → Volume down

### GPIO 0 (Power Button)
- Short press → Play / Pause
- Long press → Deep sleep

---

## 🔋 Battery Monitoring

- Uses ADC measurement
- Averaged readings for stability
- Battery bar colors:
  - Red → low
  - Orange → medium
  - Green → high
- Considered "full" at ~4.08V for better UX

---

## 💤 Power Management

- Device enters deep sleep on long press
- Wake-up via buttons

⚠️ Note:
- Board has no reset button
- If frozen:
  - use external reset button
  - or power cycle (battery switch)

---

## 📂 Station Handling

- Stations loaded from SD card
- Format: NAME|URL
- Names are displayed in UI
- Supports up to 40 stations
- 6 stations per page

---

## 🕒 Time Handling

- Uses NTP (`configTzTime`)
- Timezone loaded from SD card
- Updates every few seconds
- Supports DST via POSIX TZ string

---

## 🧪 Debug Mode

```cpp
#define DEBUG 1
1 → enable serial debug
0 → disable debug (recommended for final build)
```
## Notes

audio.loop() must run frequently → do not block loop

Avoid heavy display redraw

Stable Wi-Fi required for streaming

SD card must be FAT32

## 🙏 Credits

Inspiration:
https://github.com/VolosR/WaveshareRadioStream

## ⚠️ Note:
This project is heavily modified and mostly rewritten.

## License
MIT License
