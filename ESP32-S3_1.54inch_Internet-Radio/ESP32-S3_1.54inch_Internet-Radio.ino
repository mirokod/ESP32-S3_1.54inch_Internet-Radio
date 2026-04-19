/*
  ============================================================================
  ESP32-S3 Internet Radio
  ============================================================================

  Required files on the SD card root:

  1) /wifi.txt
    Format:
      SSID|PASSWORD
    Example:
      MyHomeWiFi|supersecretpassword
      PhoneHotspot|12345678

  2) /stations.txt
    Format:
      STATION_NAME|STREAM_URL
    Example:
      Beat|http://icecast2.play.cz/radiobeat128.mp3
      Cas Rock|http://icecast6.play.cz/casrock128.mp3
      Rock SK|http://stream.bauermedia.sk/rock-hi.mp3
      Fajn Rock Music|http://icecast2.play.cz/fajnrock192.mp3
      Rock Zone 105.9|http://icecast2.play.cz/rockzone128.mp3
      Rock Max Hard|http://ice2.radia.cz/metalomanie128.aac
      Oldies Rock|http://mp3stream4.abradio.cz/oldiesrock128.mp3
      Oldies|http://ice.abradio.cz/oldiesradio128.mp3
      Viva|http://stream.sepia.sk:8000/viva320.mp3

  3) /timezone.txt
    Format:
      Single line with POSIX TZ string
      Reference on the bottom of the code
    Example:
      CET-1CEST,M3.5.0/2,M10.5.0/3

  Notes:
  - Lines starting with '#' are treated as comments.
  - Empty lines are ignored.
  - The radio loads Wi-Fi credentials, station list, and timezone from the SD card.
  - Debug logging can be enabled or disabled with the DEBUG macro below.
*/

#include "Arduino.h"
#include <WiFi.h>
#include "WiFiMulti.h"
#include "Audio.h"
#include "SD_MMC.h"
#include "FS.h"
#include <LovyanGFX.hpp>
#include "es8311.h"
#include "esp_check.h"
#include "Wire.h"
#include <Preferences.h>
#include <time.h>

// -----------------------------------------------------------------------------
// Debug configuration
// Set DEBUG to 1 for serial logging, or 0 for a quiet release build.
// -----------------------------------------------------------------------------
#define DEBUG 0

#if DEBUG
  #define DBG_BEGIN(x)       Serial.begin(x)
  #define DBG_PRINT(x)       Serial.print(x)
  #define DBG_PRINTLN(x)     Serial.println(x)
  #define DBG_PRINTF(...)    Serial.printf(__VA_ARGS__)
#else
  #define DBG_BEGIN(x)
  #define DBG_PRINT(x)
  #define DBG_PRINTLN(x)
  #define DBG_PRINTF(...)
#endif

// -----------------------------------------------------------------------------
// Hardware pin definitions
// -----------------------------------------------------------------------------
#define PA_CTRL   7
#define I2S_MCLK  8
#define I2S_BCLK  9
#define I2S_DOUT  12
#define I2S_LRC   10

#define I2C_SDA   42
#define I2C_SCL   41

#define GFX_BL    46

// SD_MMC pins
int clk = 16;
int cmd = 15;
int d0  = 17;
int d1  = 18;
int d2  = 13;
int d3  = 14;

// -----------------------------------------------------------------------------
// ES8311 audio codec configuration
// -----------------------------------------------------------------------------
#define EXAMPLE_SAMPLE_RATE    (16000)
#define EXAMPLE_MCLK_MULTIPLE  (256)
#define EXAMPLE_MCLK_FREQ_HZ   (EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE)
#define EXAMPLE_VOICE_VOLUME   (75)

//static const char *TAG = "radio";

// -----------------------------------------------------------------------------
// LovyanGFX custom config for ST7789 240x240 over SPI
// -----------------------------------------------------------------------------
class LGFX : public lgfx::LGFX_Device
{
  lgfx::Panel_ST7789 _panel_instance;
  lgfx::Bus_SPI _bus_instance;

public:
  LGFX(void)
  {
    { // SPI bus configuration
      auto cfg = _bus_instance.config();

      cfg.spi_host   = SPI2_HOST;
      cfg.spi_mode   = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 16000000;
      cfg.spi_3wire  = true;
      cfg.use_lock   = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk   = 38;
      cfg.pin_mosi   = 39;
      cfg.pin_miso   = -1;
      cfg.pin_dc     = 45;

      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    { // Panel configuration
      auto cfg = _panel_instance.config();

      cfg.pin_cs          = 21;
      cfg.pin_rst         = 40;
      cfg.pin_busy        = -1;

      cfg.memory_width    = 240;
      cfg.memory_height   = 240;
      cfg.panel_width     = 240;
      cfg.panel_height    = 240;
      cfg.offset_x        = 0;
      cfg.offset_y        = 0;
      cfg.offset_rotation = 0;

      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = false;
      cfg.invert           = true;
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;

      _panel_instance.config(cfg);
    }

    setPanel(&_panel_instance);
  }
};

static LGFX lcd;

// -----------------------------------------------------------------------------
// Global objects
// -----------------------------------------------------------------------------
Preferences prefs;
Audio audio;
WiFiMulti wifiMulti;

// -----------------------------------------------------------------------------
// Runtime state
// -----------------------------------------------------------------------------
bool isPaused      = false;
bool connected     = false;
bool wasConnected  = false;
bool gfxReady      = false;

bool redrawDynamic  = false;
bool redrawPlaylist = false;

bool timeSynced = false;

// -----------------------------------------------------------------------------
// Button handling state
// -----------------------------------------------------------------------------
unsigned long powerButtonPressTime = 0;
bool powerButtonHeld = false;
bool debPower = false;
const unsigned long powerLongPressTime = 1000;

unsigned long buttonPressTime = 0;
bool buttonHeld = false;
bool deb = false;
const unsigned long longPressTime = 600;

unsigned long volumeButtonPressTime = 0;
bool volumeButtonHeld = false;
bool deb2 = false;
const unsigned long volumeLongPressTime = 600;

// -----------------------------------------------------------------------------
// Time and battery state
// -----------------------------------------------------------------------------
unsigned long lastClockUpdate = 0;

String tzString = "CET-1CEST,M3.5.0/2,M10.5.0/3";
String timeMain = "--:--";
String dateLine = "--.--.----";

float voltage = 4.20f;
int batLevel = 0;

// -----------------------------------------------------------------------------
// Radio state
// -----------------------------------------------------------------------------
int chosen = 0;
int volume = 16;

#define MAX_STATIONS 40
#define VISIBLE_STATIONS 6

String stationNames[MAX_STATIONS];
String stationUrls[MAX_STATIONS];
int stationCount = 0;

// -----------------------------------------------------------------------------
// AUDIO CODEC
// Initializes the ES8311 codec for playback.
// -----------------------------------------------------------------------------
static esp_err_t es8311_codec_init(void)
{
  es8311_handle_t es_handle = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  ESP_RETURN_ON_FALSE(es_handle, ESP_FAIL, TAG, "es8311 create failed");

  const es8311_clock_config_t es_clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = EXAMPLE_MCLK_FREQ_HZ,
    .sample_frequency = EXAMPLE_SAMPLE_RATE
  };

  ESP_ERROR_CHECK(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
  ESP_RETURN_ON_ERROR(
    es8311_sample_frequency_config(es_handle, EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE, EXAMPLE_SAMPLE_RATE),
    TAG,
    "set es8311 sample frequency failed"
  );
  ESP_RETURN_ON_ERROR(es8311_voice_volume_set(es_handle, EXAMPLE_VOICE_VOLUME, NULL), TAG, "set es8311 volume failed");
  ESP_RETURN_ON_ERROR(es8311_microphone_config(es_handle, false), TAG, "set es8311 microphone failed");

  return ESP_OK;
}

// -----------------------------------------------------------------------------
// HELPERS
// -----------------------------------------------------------------------------

// Copies a String into a C buffer, trims it to maxChars, and guarantees null termination.
void copyTextTrimmed(const String &src, char *dst, size_t dstSize, size_t maxChars)
{
  if (dstSize == 0) return;

  size_t len = src.length();
  if (len > maxChars) len = maxChars;
  if (len > dstSize - 1) len = dstSize - 1;

  memcpy(dst, src.c_str(), len);
  dst[len] = '\0';
}

// Draws a text string at the given position with color, background, and size.
void drawText(int16_t x, int16_t y, uint16_t color, uint16_t bg, uint8_t size, const char *text)
{
  lcd.setCursor(x, y);
  lcd.setTextColor(color, bg);
  lcd.setTextSize(size);
  lcd.print(text);
}

// Draws one trimmed text line so long station names do not overflow the layout.
void drawTextLineTrimmed(int16_t x, int16_t y, uint16_t color, uint16_t bg, uint8_t size, const String &text, size_t maxChars)
{
  char buf[40];
  copyTextTrimmed(text, buf, sizeof(buf), maxChars);
  drawText(x, y, color, bg, size, buf);
}

// Clears the display and shows a simple boot message.
void showBootMessage(const char *msg)
{
  if (!gfxReady) return;

  lcd.fillScreen(TFT_BLACK);
  drawText(10, 30, TFT_GREEN, TFT_BLACK, 2, msg);
}

// -----------------------------------------------------------------------------
// SD LOADING
// -----------------------------------------------------------------------------

// Loads Wi-Fi credentials from /wifi.txt.
// Each non-comment line must have the format: SSID|PASSWORD
bool loadWiFiFromSD(const char *filename)
{
  File file = SD_MMC.open(filename, FILE_READ);
  if (!file) {
    DBG_PRINTLN("Failed to open Wi-Fi file.");
    return false;
  }

  DBG_PRINTLN("wifi.txt opened");
  int count = 0;
  String line = "";

  while (file.available()) {
    char c = file.read();

    if (c == '\r') continue;

    if (c == '\n') {
      line.trim();

      if (line.length() > 0 && !line.startsWith("#")) {
        int sep = line.indexOf('|');
        if (sep > 0) {
          String ssid = line.substring(0, sep);
          String password = line.substring(sep + 1);
          ssid.trim();
          password.trim();

          if (ssid.length() > 0) {
            wifiMulti.addAP(ssid.c_str(), password.c_str());
            DBG_PRINT("Added Wi-Fi: ");
            DBG_PRINTLN(ssid);
            count++;
          }
        }
      }

      line = "";
    } else {
      line += c;
    }
  }

  line.trim();
  if (line.length() > 0 && !line.startsWith("#")) {
    int sep = line.indexOf('|');
    if (sep > 0) {
      String ssid = line.substring(0, sep);
      String password = line.substring(sep + 1);
      ssid.trim();
      password.trim();

      if (ssid.length() > 0) {
        wifiMulti.addAP(ssid.c_str(), password.c_str());
        DBG_PRINT("Added Wi-Fi: ");
        DBG_PRINTLN(ssid);
        count++;
      }
    }
  }

  file.close();

  DBG_PRINT("Loaded Wi-Fi networks: ");
  DBG_PRINTLN(count);

  return count > 0;
}

// Loads radio stations from /stations.txt.
// Each non-comment line must have the format: NAME|URL
bool loadStationsFromSD(const char *filename)
{
  File file = SD_MMC.open(filename, FILE_READ);
  if (!file) {
    DBG_PRINTLN("Failed to open station list file.");
    return false;
  }

  stationCount = 0;

  while (file.available() && stationCount < MAX_STATIONS) {
    String line = file.readStringUntil('\n');
    line.trim();

    if (line.length() == 0) continue;
    if (line.startsWith("#")) continue;

    int sep = line.indexOf('|');
    if (sep <= 0 || sep >= line.length() - 1) continue;

    String name = line.substring(0, sep);
    String url  = line.substring(sep + 1);

    name.trim();
    url.trim();

    if (name.length() == 0 || url.length() == 0) continue;

    stationNames[stationCount] = name;
    stationUrls[stationCount]  = url;

    DBG_PRINT("Station ");
    DBG_PRINT(stationCount);
    DBG_PRINT(": ");
    DBG_PRINT(name);
    DBG_PRINT(" -> ");
    DBG_PRINTLN(url);

    stationCount++;
  }

  file.close();

  DBG_PRINT("Loaded stations: ");
  DBG_PRINTLN(stationCount);

  return stationCount > 0;
}

// Loads the POSIX timezone string from /timezone.txt.
bool loadTimeZoneFromSD(const char *filename)
{
  File file = SD_MMC.open(filename, FILE_READ);
  if (!file) {
    DBG_PRINTLN("Failed to open timezone file.");
    return false;
  }

  String line = file.readStringUntil('\n');
  file.close();

  line.trim();

  if (line.length() == 0 || line.startsWith("#")) {
    DBG_PRINTLN("timezone.txt is empty or invalid.");
    return false;
  }

  tzString = line;

  DBG_PRINT("Loaded timezone: ");
  DBG_PRINTLN(tzString);

  return true;
}

// -----------------------------------------------------------------------------
// TIME
// -----------------------------------------------------------------------------

// Starts NTP synchronization using the timezone loaded from the SD card.
void initInternetTime()
{
  if (WiFi.status() != WL_CONNECTED) {
    DBG_PRINTLN("Wi-Fi is not connected, skipping NTP.");
    return;
  }

  configTzTime(
    tzString.c_str(),
    "pool.ntp.org",
    "time.nist.gov",
    "0.pool.ntp.org"
  );

  DBG_PRINTLN("Waiting for time synchronization...");

  struct tm timeinfo;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&timeinfo, 500)) {
      timeSynced = true;
      DBG_PRINTLN("Time synchronized via NTP.");
      return;
    }
    delay(250);
    DBG_PRINT(".");
  }

  DBG_PRINTLN("");
  DBG_PRINTLN("Failed to synchronize time.");
  timeSynced = false;
}

// Refreshes cached time/date strings used by the UI.
void updateClockStrings()
{
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo, 50)) {
    timeSynced = false;
    timeMain = "--:--";
    dateLine = "--.--.----";
    return;
  }

  timeSynced = true;

  char bufMain[6];
  char bufDate[16];

  strftime(bufMain, sizeof(bufMain), "%H:%M", &timeinfo);
  strftime(bufDate, sizeof(bufDate), "%d.%m.%Y", &timeinfo);

  timeMain = bufMain;
  dateLine = bufDate;
}

// -----------------------------------------------------------------------------
// PREFERENCES
// -----------------------------------------------------------------------------

// Saves the last selected station index into NVS.
bool saveLastStation(int index)
{
  prefs.begin("radio", false);

  int lastSaved = prefs.getInt("station", -1);
  if (lastSaved != index) {
    prefs.putInt("station", index);
    DBG_PRINT("Saved last station to NVS: ");
    DBG_PRINTLN(index);
  } else {
    DBG_PRINTLN("Station unchanged, skipping NVS write.");
  }

  prefs.end();
  return true;
}

// Loads the last selected station index from NVS.
bool loadLastStation()
{
  prefs.begin("radio", true);
  int idx = prefs.getInt("station", 0);
  prefs.end();

  if (idx < 0 || idx >= stationCount) {
    DBG_PRINTLN("Saved station index in NVS is out of range.");
    chosen = 0;
    return false;
  }

  chosen = idx;

  DBG_PRINT("Loaded last station from NVS: ");
  DBG_PRINTLN(chosen);
  return true;
}

// -----------------------------------------------------------------------------
// AUDIO CONTROL
// -----------------------------------------------------------------------------

// Fully restarts audio playback for the currently selected station.
// This also re-enables the power amplifier and re-applies I2S settings.
void restartAudioStream()
{
  if (stationCount <= 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  DBG_PRINTLN("Restarting audio stream...");

  audio.stopSong();
  delay(20);

  digitalWrite(PA_CTRL, LOW);
  delay(20);

  digitalWrite(PA_CTRL, HIGH);
  delay(20);

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK);
  audio.setVolume(volume);

  delay(20);
  audio.connecttohost(stationUrls[chosen].c_str());

  isPaused = false;
}

// Toggles pause/resume state in the audio library.
void togglePause()
{
  if (stationCount <= 0) return;
  if (!connected) return;

  audio.pauseResume();
  isPaused = !isPaused;

  DBG_PRINT("isPaused = ");
  DBG_PRINTLN(isPaused ? "true" : "false");

  redrawDynamic = true;
}

// Selects the next or previous station and starts playback.
void changeStation(int step)
{
  if (stationCount <= 0) return;

  chosen += step;

  if (chosen >= stationCount) chosen = 0;
  if (chosen < 0) chosen = stationCount - 1;

  DBG_PRINTLN("");
  DBG_PRINTLN("=== SWITCHING STATION ===");
  DBG_PRINT("chosen = ");
  DBG_PRINTLN(chosen);
  DBG_PRINT("name   = ");
  DBG_PRINTLN(stationNames[chosen]);
  DBG_PRINT("url    = ");
  DBG_PRINTLN(stationUrls[chosen]);

  audio.stopSong();
  delay(150);

  if (WiFi.status() == WL_CONNECTED) {
    audio.connecttohost(stationUrls[chosen].c_str());
  }

  isPaused = false;
  saveLastStation(chosen);

  redrawPlaylist = true;
  redrawDynamic = true;
}

// Changes volume and updates the audio library immediately.
void changeVolume(int step)
{
  volume += step;

  if (volume > 21) volume = 21;
  if (volume < 0) volume = 0;

  audio.setVolume(volume);

  DBG_PRINT("Volume = ");
  DBG_PRINTLN(volume);

  redrawDynamic = true;
}

// -----------------------------------------------------------------------------
// POWER
// -----------------------------------------------------------------------------

// Stops playback, powers down radios, and enters deep sleep.
// Any of the configured buttons can wake the device.
void goToSleep()
{
  DBG_PRINTLN("Going to deep sleep...");

  while (digitalRead(0) == LOW || digitalRead(4) == LOW || digitalRead(5) == LOW) {
    delay(10);
  }

  audio.stopSong();
  delay(150);

  digitalWrite(PA_CTRL, LOW);

  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);

  uint64_t wakeMask =
    (1ULL << GPIO_NUM_0) |
    (1ULL << GPIO_NUM_4) |
    (1ULL << GPIO_NUM_5);

  esp_sleep_enable_ext1_wakeup_io(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);

  delay(100);
  esp_deep_sleep_start();
}

// -----------------------------------------------------------------------------
// BATTERY
// -----------------------------------------------------------------------------

// Reads battery voltage from the ADC, averages a few samples,
// and converts the result into a 0..100 battery indicator.
//
// Note:
// The display is treated as "full" from 4.08V and above for a nicer UI.
void measureBatt()
{
  constexpr int BAT_ADC_PIN = 1;

  uint32_t sum = 0;
  for (int i = 0; i < 4; i++) {
    sum += analogReadMilliVolts(BAT_ADC_PIN);
  }

  float mv = sum / 4.0f;
  float vbat = (mv / 1000.0f) * 3.0f;
  voltage = vbat;

  float minV = 3.0f;
  float fullV = 4.08f;

  float pct;

  if (vbat >= fullV) {
    pct = 1.0f;
  } else {
    pct = (vbat - minV) / (fullV - minV);
  }

  pct = constrain(pct, 0.0f, 1.0f);

  batLevel = (int)(pct * 100.0f + 0.5f);
}

// -----------------------------------------------------------------------------
// UI
// -----------------------------------------------------------------------------

// Draws a horizontal volume bar based on the current volume range 0..21.
void drawVolumeBar(int x, int y, int w, int h)
{
  lcd.drawRect(x, y, w, h, TFT_YELLOW);

  int innerW = w - 2;
  int fillW = map(volume, 0, 21, 0, innerW);

  if (fillW > 0) {
    lcd.fillRect(x + 1, y + 1, fillW, h - 2, TFT_YELLOW);
  }
}

// Draws a battery-shaped horizontal bar with a small terminal on the right.
// Color changes with battery level.
void drawBatteryBar(int x, int y, int w, int h)
{
  uint16_t color;

  if (batLevel <= 20) {
    color = TFT_RED;
  } else if (batLevel <= 50) {
    color = TFT_ORANGE;
  } else {
    color = TFT_GREEN;
  }

  // Main frame
  lcd.drawRect(x, y, w, h, color);

  // Fill
  int innerW = w - 2;
  int fillW = map(constrain(batLevel, 0, 100), 0, 100, 0, innerW);

  if (fillW > 0) {
    lcd.fillRect(x + 1, y + 1, fillW, h - 2, color);
  }

  // Battery terminal
  int tipW = 4;
  int tipH = h / 2;
  int tipX = x + w;
  int tipY = y + (h - tipH) / 2;

  lcd.fillRect(tipX, tipY, tipW, tipH, color);
}

// Draws all static UI elements that do not change frequently.
void drawStaticUI()
{
  if (!gfxReady) return;

  lcd.fillScreen(TFT_BLACK);
  lcd.setTextWrap(false);

  lcd.drawFastHLine(0, 6, 240, TFT_ORANGE);

  // Left panel: station list
  lcd.drawRect(4, 14, 148, 222, TFT_WHITE);

  // Right panel split into three boxes
  lcd.drawRect(158, 14, 78, 54, TFT_WHITE);    // Wi-Fi + battery
  lcd.drawRect(158, 74, 78, 54, TFT_WHITE);    // Time + date
  lcd.drawRect(158, 134, 78, 102, TFT_WHITE);  // Volume + state

  drawText(10, 20, TFT_YELLOW, TFT_BLACK, 1, "STATIONS");
}

// Draws the current page of stations and highlights the selected one.
void drawPlaylist()
{
  lcd.fillRect(8, 36, 140, 178, TFT_BLACK);

  int pageStart = (chosen / VISIBLE_STATIONS) * VISIBLE_STATIONS;
  int pageEnd = pageStart + VISIBLE_STATIONS;
  if (pageEnd > stationCount) pageEnd = stationCount;

  for (int i = pageStart; i < pageEnd; i++) {
    int row = i - pageStart;
    int y = 38 + (row * 30);

    if (i == chosen) {
      lcd.fillRect(8, y - 2, 140, 18, TFT_DARKGREEN);
      drawTextLineTrimmed(12, y, TFT_WHITE, TFT_DARKGREEN, 2, stationNames[i], 11);
    } else {
      drawTextLineTrimmed(12, y, TFT_GREEN, TFT_BLACK, 2, stationNames[i], 11);
    }
  }

  lcd.fillRect(8, 216, 140, 14, TFT_BLACK);

  char pageBuf[20];
  int totalPages = (stationCount + VISIBLE_STATIONS - 1) / VISIBLE_STATIONS;
  if (totalPages < 1) totalPages = 1;
  int currentPage = chosen / VISIBLE_STATIONS + 1;
  snprintf(pageBuf, sizeof(pageBuf), "PAGE %d/%d", currentPage, totalPages);
  drawText(10, 218, TFT_YELLOW, TFT_BLACK, 1, pageBuf);

  redrawPlaylist = false;
}

// Draws dynamic UI parts such as connection state, battery, time, volume, and playback state.
void drawDynamicUI()
{
  // ---------------------------------------------------------------------------
  // BOX 1: WIFI + BATTERY + VOLTAGE
  // ---------------------------------------------------------------------------
  lcd.fillRect(160, 16, 74, 50, TFT_BLACK);

  drawText(166, 20, connected ? TFT_GREEN : TFT_RED, TFT_BLACK, 1,
           connected ? "WIFI OK" : "WIFI ERR");

  drawBatteryBar(166, 36, 52, 10);

  char voltBuf[16];
  snprintf(voltBuf, sizeof(voltBuf), "%.2fV", voltage);
  drawText(166, 52, TFT_GREEN, TFT_BLACK, 1, voltBuf);

  // ---------------------------------------------------------------------------
  // BOX 2: TIME + DATE
  // ---------------------------------------------------------------------------
  lcd.fillRect(160, 76, 74, 50, TFT_BLACK);

  if (timeSynced) {
    drawText(166, 84, TFT_CYAN, TFT_BLACK, 2, timeMain.c_str());
    drawText(166, 106, TFT_WHITE, TFT_BLACK, 1, dateLine.c_str());
  } else {
    drawText(166, 84, TFT_RED, TFT_BLACK, 2, "--:--");
    drawText(166, 106, TFT_WHITE, TFT_BLACK, 1, "NO TIME");
  }

  // ---------------------------------------------------------------------------
  // BOX 3: VOLUME + BAR + PLAY STATE
  // ---------------------------------------------------------------------------
  lcd.fillRect(160, 136, 74, 98, TFT_BLACK);

  drawText(166, 142, TFT_YELLOW, TFT_BLACK, 1, "VOLUME");

  drawVolumeBar(166, 158, 56, 10);

  char volBuf[12];
  snprintf(volBuf, sizeof(volBuf), "%d/21", volume);
  drawText(166, 174, TFT_GREEN, TFT_BLACK, 1, volBuf);

  if (isPaused) {
    drawText(166, 198, TFT_RED, TFT_BLACK, 2, "PAUSE");
  } else if (connected) {
    drawText(166, 198, TFT_GREEN, TFT_BLACK, 2, "PLAY");
  } else {
    drawText(166, 198, TFT_RED, TFT_BLACK, 2, "STOP");
  }

  redrawDynamic = false;
}

// -----------------------------------------------------------------------------
// SETUP
// -----------------------------------------------------------------------------

void setup()
{
  DBG_BEGIN(115200);
  delay(300);

  Wire.begin(I2C_SDA, I2C_SCL);

  gpio_hold_dis((gpio_num_t)2);

  pinMode(0, INPUT_PULLUP);
  pinMode(5, INPUT_PULLUP);
  pinMode(4, INPUT_PULLUP);

  isPaused = false;
  powerButtonHeld = false;
  powerButtonPressTime = 0;

  pinMode(2, OUTPUT);
  digitalWrite(2, HIGH);

  pinMode(PA_CTRL, OUTPUT);
  digitalWrite(PA_CTRL, HIGH);

  es8311_codec_init();
  gpio_hold_en((gpio_num_t)2);

  lcd.init();
  lcd.setRotation(0);
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextWrap(false);
  analogWrite(GFX_BL, 110);

  gfxReady = true;

  showBootMessage("Starting...");
  delay(300);

  bool sdOk = false;

  showBootMessage("Init SD card...");
  DBG_PRINTLN("Initializing SD_MMC...");
  delay(300);

  if (!SD_MMC.setPins(clk, cmd, d0, d1, d2, d3)) {
    DBG_PRINTLN("SD pin assignment failed.");
  } else if (!SD_MMC.begin()) {
    DBG_PRINTLN("SD card mount failed.");
  } else {
    sdOk = true;
    DBG_PRINTLN("SD card mounted.");
  }

  if (sdOk) {
    showBootMessage("Loading WiFi...");
    loadWiFiFromSD("/wifi.txt");
    delay(150);

    showBootMessage("Loading stations...");
    loadStationsFromSD("/stations.txt");
    delay(150);

    loadTimeZoneFromSD("/timezone.txt");
    loadLastStation();
  }

  if (chosen >= stationCount) chosen = 0;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  showBootMessage("Connecting WiFi...");

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 30000) {
    wifiMulti.run();
    delay(250);
    DBG_PRINT(".");
  }
  DBG_PRINTLN("");

  if (WiFi.status() == WL_CONNECTED) {
    DBG_PRINTLN("Wi-Fi connected.");
    DBG_PRINTLN(WiFi.SSID());
    DBG_PRINTLN(WiFi.localIP());

    initInternetTime();
    updateClockStrings();
  } else {
    DBG_PRINTLN("Failed to connect to Wi-Fi.");
  }

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK);
  audio.setVolume(volume);

  if (stationCount > 0 && WiFi.status() == WL_CONNECTED) {
    showBootMessage("Starting audio...");
    restartAudioStream();
  }

  connected = (WiFi.status() == WL_CONNECTED);
  wasConnected = connected;

  drawStaticUI();
  drawPlaylist();
  drawDynamicUI();
}

// -----------------------------------------------------------------------------
// LOOP
// -----------------------------------------------------------------------------

void loop()
{
  // Keep audio decoding and streaming serviced as often as possible.
  audio.loop();

  // Refresh battery status periodically.
  static unsigned long lastBatt = 0;
  if (millis() - lastBatt > 15000) {
    lastBatt = millis();
    measureBatt();
    redrawDynamic = true;
  }

  // Refresh time/date strings every few seconds.
  if (millis() - lastClockUpdate >= 5000) {
    lastClockUpdate = millis();

    String oldTime = timeMain;
    String oldDate = dateLine;
    bool oldTimeSync = timeSynced;

    updateClockStrings();

    if (timeMain != oldTime || dateLine != oldDate || timeSynced != oldTimeSync) {
      redrawDynamic = true;
    }
  }

  // Station button:
  // short press  -> next station
  // long press   -> previous station
  if (digitalRead(5) == LOW) {
    if (!deb) {
      deb = true;
      buttonPressTime = millis();
      buttonHeld = false;
    }

    if (!buttonHeld && (millis() - buttonPressTime > longPressTime)) {
      buttonHeld = true;
      changeStation(-1);
    }
  } else {
    if (deb) {
      if (!buttonHeld) {
        changeStation(1);
      }
    }
    deb = false;
  }

  // Volume button:
  // short press  -> volume up
  // long press   -> volume down
  if (digitalRead(4) == LOW) {
    if (!deb2) {
      deb2 = true;
      volumeButtonPressTime = millis();
      volumeButtonHeld = false;
    }

    if (!volumeButtonHeld && (millis() - volumeButtonPressTime > volumeLongPressTime)) {
      volumeButtonHeld = true;
      changeVolume(-5);
    }
  } else {
    if (deb2) {
      if (!volumeButtonHeld) {
        changeVolume(2);
      }
    }
    deb2 = false;
  }

  // Power button:
  // short press  -> pause/resume
  // long press   -> deep sleep
  if (digitalRead(0) == LOW) {
    if (!debPower) {
      debPower = true;
      powerButtonPressTime = millis();
      powerButtonHeld = false;
    }

    if (!powerButtonHeld && (millis() - powerButtonPressTime > powerLongPressTime)) {
      powerButtonHeld = true;
      goToSleep();
    }
  } else {
    if (debPower) {
      if (!powerButtonHeld) {
        togglePause();
      }
    }
    debPower = false;
  }

  // Redraw only what is needed.
  if (redrawPlaylist && gfxReady) {
    drawPlaylist();
  }

  if (redrawDynamic && gfxReady) {
    drawDynamicUI();
  }

  vTaskDelay(1);
}




/*
===============================================================================
TIMEZONE (POSIX TZ STRING) REFERENCE
===============================================================================

Format:
  STD-OffsetDST,M3.5.0/2,M10.5.0/3

Explanation:
  STD     = standard time name (any label, e.g. CET, GMT, etc.)
  Offset  = hours FROM UTC (IMPORTANT: sign is inverted!)
            -1  = UTC+1
            -2  = UTC+2
            +2  = UTC-2
  DST     = daylight saving time name (any label, e.g. CEST)
  M3.5.0/2  = last Sunday of March at 02:00
  M10.5.0/3 = last Sunday of October at 03:00

NOTE:
  POSIX uses reversed sign convention:
    "-" means PLUS hours
    "+" means MINUS hours

===============================================================================
COMMON TIMEZONES (EU STYLE DST)
===============================================================================

UTC+0 / UTC+1 (UK)
  GMT0BST,M3.5.0/2,M10.5.0/3

UTC+1 / UTC+2 (Central Europe - Slovakia, Czech, Germany)
  CET-1CEST,M3.5.0/2,M10.5.0/3

UTC+2 / UTC+3 (Eastern Europe - Finland, Romania)
  EET-2EEST,M3.5.0/2,M10.5.0/3

UTC+3 / UTC+4
  MSK-3MSKST,M3.5.0/2,M10.5.0/3

===============================================================================
NEGATIVE OFFSETS (WEST OF UTC)
===============================================================================

UTC-1 / UTC+0
  STD+1DST,M3.5.0/2,M10.5.0/3

UTC-2 / UTC-1
  STD+2DST,M3.5.0/2,M10.5.0/3

UTC-3 / UTC-2
  STD+3DST,M3.5.0/2,M10.5.0/3

===============================================================================
FIXED TIME (NO DST)
===============================================================================

UTC
  UTC0

UTC+1 (no DST)
  UTC-1

UTC+2 (no DST)
  UTC-2

UTC-2 (no DST)
  UTC+2

===============================================================================
TIPS
===============================================================================

- If you only want to shift time for testing, use:
    UTC-1  → +1 hour
    UTC-2  → +2 hours
    UTC+2  → -2 hours

- If you want real-world behavior, use full format with DST.

- Names (CET, CEST, etc.) are just labels.
  Only the offset and rules actually matter.

===============================================================================
*/

