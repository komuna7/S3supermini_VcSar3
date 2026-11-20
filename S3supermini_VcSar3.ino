#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <FFat.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <unzipLIB.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Wire.h>
#include <time.h>
#include "V3DepartureGuide.h"

// Stand-alone successor to ../train_display.  Network parsing is deliberately
// split into stages so an incomplete download never replaces the last good
// timetable cache.
constexpr int SDA_PIN = 8;
constexpr int SCL_PIN = 9;
constexpr uint8_t OLED_ADDRESS = 0x3C;
constexpr int SCREEN_WIDTH = 128;
constexpr int SCREEN_HEIGHT = 64;
constexpr char STATIC_GTFS_URL[] = "https://www.vivi.lv/uploads/GTFS.zip";
constexpr char REALTIME_URL[] =
    "https://modus.pv.lv/transit_realtime.RealTime/GetFeedTripUpdate";
// Nightly restart rebuilds today's/tomorrow's trips and rechecks GTFS.
constexpr int DAILY_RESTART_HOUR = 3;
// Must exceed one hour so a restart inside DAILY_RESTART_HOUR cannot repeat.
constexpr uint32_t DAILY_RESTART_MIN_UPTIME_MS = 70UL * 60UL * 1000UL;
constexpr uint32_t REALTIME_REFRESH_MS = 2UL * 60UL * 1000UL;
constexpr size_t DOWNLOAD_BUFFER_SIZE = 2048;
constexpr size_t MAX_STATIC_GTFS_BYTES = 11UL * 1024UL * 1024UL;
constexpr char STATIC_FILE[] = "/gtfs/static.zip";
constexpr char STATIC_TEMP_FILE[] = "/gtfs/static.zip.tmp";
constexpr char LOCAL_TIMEZONE[] = "EET-2EEST,M3.5.0/3,M10.5.0/4";
constexpr char ORIGIN_STOP_ID[] = "322";
constexpr char DESTINATION_STOP_ID[] = "1";
constexpr size_t MAX_ACTIVE_SERVICES = 160;
constexpr size_t MAX_CANDIDATES = 1024;
constexpr int LIST_BUTTON_PIN = 4;
constexpr uint32_t LIST_VIEW_TIMEOUT_MS = 8000;
// A 20 ms debounce still rejects normal switch bounce, while a quick human
// tap (~40-50 ms) is reliably caught with the 20 ms main-loop cadence.
constexpr uint32_t BUTTON_DEBOUNCE_MS = 20;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Preferences preferences;
UNZIP gtfsZip;
File gtfsZipFile;
WebServer webServer(80);
String originLabel = "MEL";
String destinationLabel = "RIGA";
int32_t simulatedClockSeconds = -1;
String customTimesText;
String customCountdownText;

struct TripCandidate {
  char tripId[24] = {};
  uint16_t originSequence = 0;
  uint16_t destinationSequence = 0;
  uint32_t departureSeconds = 0;
  uint32_t arrivalSeconds = 0;
  bool hasOrigin = false;
  bool hasDestination = false;
  bool activeToday = false;
  int32_t delaySeconds = 0;
  bool realtime = false;
  bool cancelled = false;
  uint8_t dayOffset = 0;
};

char activeServices[MAX_ACTIVE_SERVICES][32] = {};
size_t activeServiceCount = 0;
TripCandidate candidates[MAX_CANDIDATES];
size_t candidateCount = 0;
char displayedTripId[24] = {};
uint32_t countdownRange = 1;
uint8_t buildingDayOffset = 0;
int loadedYearDay = -1;
uint32_t lastRealtimeRefresh = 0;
bool realtimeStale = true;
int lastRealtimeStatus = 0;
bool showingUpcomingList = false;
uint8_t upcomingListPage = 0;
uint32_t lastListInteraction = 0;

TripCandidate *findCandidate(const char *tripId, bool create);

void startConfigurationWeb() {
  webServer.on("/", HTTP_GET, []() {
    const bool simulated = simulatedClockSeconds >= 0;
    const uint32_t value = simulated ? simulatedClockSeconds : 0;
    const String wifiSsids[2] = {preferences.getString("primary_ssid", WiFi.SSID()),
                                 preferences.getString("fallback_ssid", "")};
    String page = "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
                  "<style>body{font:18px sans-serif;max-width:420px;margin:24px auto;padding:0 16px}"
                  "input{font-size:18px;width:100%;margin:6px 0 16px;padding:8px}button{font-size:18px;padding:10px 16px}"
                  ".note{font-size:14px;color:#444}</style>"
                  "<h2>Vilcienu ekrans V3</h2><form method=post action=/save>"
                  "<label>Uzraksts pa kreisi</label><input name=origin value='" + originLabel + "'>"
                  "<label>Uzraksts pa labi</label><input name=destination value='" + destinationLabel + "'>"
                  "<label>Simuletais laiks (HH:MM; tukss = realais)</label><input name=clock value='";
    if (simulated) {
      char clock[6];
      snprintf(clock, sizeof(clock), "%02lu:%02lu", value / 3600UL,
               (value / 60UL) % 60UL);
      page += clock;
    }
    page += "'><hr><h3>Laiku aizvietošana</h3>"
            "<p class=note>Neobligati. Lauks Laiki aizvieto abus automatiskos laikus ar vienu uzrakstu pa visu ekrana platumu; tukss lauks atgriez parastos laikus.</p>"
            "<label>Laiki</label><input maxlength=10 name=custom_times value='" + customTimesText + "'>"
            "<label>Atskaites pulkstena vieta</label><input maxlength=5 name=custom_countdown value='" + customCountdownText + "'>"
            "<button>Saglabat uzrakstus</button></form>"
            "<hr><p class=note>Startejot ESP 5 sekundes rada abus tiklus; ar pogu izvelies, kuram pieslegties. "
            "Ja tas nav pieejams, ESP meginās otru. Paroli ievadi pirmaja saglabasana; velak tuksa parole nozime, ka ta paliek ta pati.</p>";
    for (int slot = 1; slot <= 2; ++slot) {
      page += "<h3>Wi-Fi " + String(slot) + "</h3><form method=post action=/wifi>"
              "<input type=hidden name=slot value=" + String(slot) + ">"
              "<label>Wi-Fi nosaukums (SSID)</label><input name=ssid value='" + wifiSsids[slot - 1] + "'>"
              "<label>Parole</label><input type=password name=password placeholder='Ievadi jaunu paroli'>";
      if (slot == 2) page += "<p class=note>Tukss nosaukums izdzes Wi-Fi 2.</p>";
      page += "<button>Saglabat Wi-Fi " + String(slot) + " un parstartet</button></form>";
    }
    page += "<p>ESP adrese: " + WiFi.localIP().toString() + " (" + WiFi.SSID() + ")</p>";
    webServer.send(200, "text/html; charset=utf-8", page);
  });
  webServer.on("/save", HTTP_POST, []() {
    originLabel = webServer.arg("origin");
    destinationLabel = webServer.arg("destination");
    customTimesText = webServer.arg("custom_times");
    customCountdownText = webServer.arg("custom_countdown");
    String clock = webServer.arg("clock");
    int hours = -1, minutes = -1;
    if (clock.length() && sscanf(clock.c_str(), "%d:%d", &hours, &minutes) == 2 &&
        hours >= 0 && hours < 24 && minutes >= 0 && minutes < 60) {
      simulatedClockSeconds = hours * 3600 + minutes * 60;
    } else {
      simulatedClockSeconds = -1;
    }
    preferences.putString("origin_label", originLabel);
    preferences.putString("destination_label", destinationLabel);
    preferences.putInt("sim_clock", simulatedClockSeconds);
    preferences.putString("custom_times", customTimesText);
    preferences.putString("custom_countdown", customCountdownText);
    webServer.sendHeader("Location", "/");
    webServer.send(303);
  });
  webServer.on("/wifi", HTTP_POST, []() {
    const int slot = webServer.arg("slot") == "2" ? 2 : 1;
    const String ssid = webServer.arg("ssid");
    const String password = webServer.arg("password");
    const String savedSsid = preferences.getString(wifiSsidKey(slot), "");
    if (ssid.isEmpty() && slot == 2) {
      preferences.remove(wifiSsidKey(2));
      preferences.remove(wifiPassKey(2));
      preferences.putInt("wifi_choice", 1);
    } else if (ssid.isEmpty()) {
      webServer.send(400, "text/plain; charset=utf-8", "Wi-Fi 1 nosaukums nedrikst but tukss.");
      return;
    } else if (password.isEmpty() && (savedSsid.isEmpty() || ssid != savedSsid)) {
      webServer.send(400, "text/plain; charset=utf-8",
                     "Ievadi paroli: jaunam vai vel nesaglabatam Wi-Fi ta ir obligata.");
      return;
    } else {
      preferences.putString(wifiSsidKey(slot), ssid);
      // Empty password intentionally preserves the old one, so it is never
      // displayed by the web page and an SSID-only edit cannot erase it.
      if (!password.isEmpty()) preferences.putString(wifiPassKey(slot), password);
    }
    webServer.send(200, "text/html; charset=utf-8",
                   "<meta name=viewport content='width=device-width,initial-scale=1'>"
                   "<p style='font:20px sans-serif;margin:24px'>Wi-Fi saglabats. ESP parstartesies...</p>");
    delay(700);
    ESP.restart();
  });
  webServer.begin();
}

bool readVarint(const uint8_t *data, size_t length, size_t &position,
                uint64_t &value) {
  value = 0;
  for (uint8_t shift = 0; shift < 64 && position < length; shift += 7) {
    const uint8_t byte = data[position++];
    value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if (!(byte & 0x80)) return true;
  }
  return false;
}

bool skipProtoField(const uint8_t *data, size_t length, size_t &position,
                    uint8_t wireType) {
  uint64_t size;
  if (wireType == 0) return readVarint(data, length, position, size);
  if (wireType == 1) { position += 8; return position <= length; }
  if (wireType == 5) { position += 4; return position <= length; }
  if (wireType != 2 || !readVarint(data, length, position, size)) return false;
  position += size;
  return position <= length;
}

void applyRealtimeTrip(const char *tripId, int32_t delay, bool cancelled) {
  TripCandidate *candidate = findCandidate(tripId, false);
  if (!candidate) return;
  candidate->realtime = true;
  candidate->delaySeconds = delay;
  candidate->cancelled = cancelled;
}

void parseTripUpdate(const uint8_t *data, size_t length) {
  char tripId[24] = {};
  int32_t delay = 0;
  bool cancelled = false;
  size_t position = 0;
  while (position < length) {
    uint64_t key; if (!readVarint(data, length, position, key)) return;
    const uint8_t field = key >> 3, wire = key & 7;
    if (field == 1 && wire == 2) { // TripDescriptor
      uint64_t nestedSize; if (!readVarint(data, length, position, nestedSize) || position + nestedSize > length) return;
      size_t end = position + nestedSize;
      while (position < end) {
        uint64_t nestedKey; if (!readVarint(data, end, position, nestedKey)) return;
        uint8_t nestedField = nestedKey >> 3, nestedWire = nestedKey & 7;
        if (nestedField == 1 && nestedWire == 2) {
          uint64_t textSize; if (!readVarint(data, end, position, textSize)) return;
          const size_t copy = min<size_t>(textSize, sizeof(tripId) - 1);
          if (position + textSize > end) return;
          memcpy(tripId, data + position, copy); tripId[copy] = '\0'; position += textSize;
        } else if (nestedField == 4 && nestedWire == 0) {
          uint64_t relationship; if (!readVarint(data, end, position, relationship)) return;
          cancelled = relationship == 3;
        } else if (!skipProtoField(data, end, position, nestedWire)) return;
      }
    } else if (field == 2 && wire == 0) {
      uint64_t rawDelay; if (!readVarint(data, length, position, rawDelay)) return;
      delay = static_cast<int32_t>(rawDelay);
    } else if (!skipProtoField(data, length, position, wire)) return;
  }
  if (tripId[0]) applyRealtimeTrip(tripId, delay, cancelled);
}

void parseRealtimeFeed(const uint8_t *data, size_t length) {
  size_t position = 0;
  while (position < length) {
    uint64_t key; if (!readVarint(data, length, position, key)) return;
    const uint8_t field = key >> 3, wire = key & 7;
    if (field == 2 && wire == 2) { // FeedEntity
      uint64_t entitySize; if (!readVarint(data, length, position, entitySize) || position + entitySize > length) return;
      size_t end = position + entitySize;
      while (position < end) {
        uint64_t entityKey; if (!readVarint(data, end, position, entityKey)) return;
        uint8_t entityField = entityKey >> 3, entityWire = entityKey & 7;
        if (entityField == 3 && entityWire == 2) {
          uint64_t updateSize; if (!readVarint(data, end, position, updateSize) || position + updateSize > end) return;
          parseTripUpdate(data + position, updateSize); position += updateSize;
        } else if (!skipProtoField(data, end, position, entityWire)) return;
      }
    } else if (!skipProtoField(data, length, position, wire)) return;
  }
}

enum class AppState : uint8_t {
  starting,
  connectingWifi,
  preparingCache,
  downloadingStatic,
  parsingStatic,
  ready,
  waitingForParser,
  error,
};

AppState appState = AppState::starting;
String detail;

void centeredText(const String &text, int16_t y, uint8_t size = 1) {
  int16_t x1, y1;
  uint16_t width, height;
  display.setTextSize(size);
  display.getTextBounds(text, 0, y, &x1, &y1, &width, &height);
  display.setCursor((SCREEN_WIDTH - width) / 2, y);
  display.print(text);
}

void drawStatus() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  centeredText("MELLUZI -> RIGA", 4);

  switch (appState) {
    case AppState::starting:
      centeredText("STARTING...", 27);
      break;
    case AppState::connectingWifi:
      centeredText("CONNECTING WI-FI", 21);
      centeredText(detail, 37);
      break;
    case AppState::preparingCache:
      centeredText("PREPARING CACHE", 21);
      centeredText(detail, 37);
      break;
    case AppState::downloadingStatic:
      centeredText("DOWNLOADING GTFS", 21);
      centeredText(detail, 37);
      break;
    case AppState::parsingStatic:
      centeredText("READING GTFS", 21);
      centeredText(detail, 37);
      break;
    case AppState::ready:
      break;
    case AppState::waitingForParser:
      centeredText("STANDALONE V2", 21);
      centeredText("PARSER: NEXT STEP", 37);
      break;
    case AppState::error:
      centeredText("ERROR", 21, 2);
      centeredText(detail, 47);
      break;
  }
  display.display();
}

bool waitForWifi(uint32_t timeoutMs) {
  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < timeoutMs) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Wi-Fi slot 1 keeps the old "primary_*" keys and slot 2 the "fallback_*"
// keys, so networks saved by earlier firmware stay valid.
const char *wifiSsidKey(int slot) { return slot == 1 ? "primary_ssid" : "fallback_ssid"; }
const char *wifiPassKey(int slot) { return slot == 1 ? "primary_pass" : "fallback_pass"; }

constexpr uint32_t WIFI_CHOICE_MS = 5000;

constexpr int32_t RSSI_NOT_SEEN = -1000;

// Strongest RSSI of each saved SSID (several access points may share a name).
void scanWifiSlots(const String ssids[3], int32_t rssi[3]) {
  rssi[1] = rssi[2] = RSSI_NOT_SEEN;
  const int found = WiFi.scanNetworks();
  for (int i = 0; i < found; ++i) {
    for (int slot = 1; slot <= 2; ++slot) {
      if (WiFi.SSID(i) == ssids[slot] && WiFi.RSSI(i) > rssi[slot]) rssi[slot] = WiFi.RSSI(i);
    }
  }
  WiFi.scanDelete();
}

void drawWifiChoice(int chosen, bool automatic, const String ssids[3], const int32_t rssi[3],
                    uint32_t remainingMs) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  centeredText(automatic ? "WI-FI: AUTO (SIGNALS)" : "WI-FI: TAVA IZVELE", 2);
  for (int slot = 1; slot <= 2; ++slot) {
    const int16_t rowY = 4 + slot * 14;
    if (slot == chosen) {
      display.fillRect(0, rowY - 2, SCREEN_WIDTH, 12, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    }
    display.setTextSize(1);
    display.setCursor(2, rowY);
    display.print(String(slot) + " " + ssids[slot].substring(0, 14));
    const String level = rssi[slot] == RSSI_NOT_SEEN ? String("--") : String(rssi[slot]);
    display.setCursor(SCREEN_WIDTH - 2 - level.length() * 6, rowY);
    display.print(level);
    display.setTextColor(SSD1306_WHITE);
  }
  centeredText("POGA: MAINIT  " + String((remainingMs + 999) / 1000) + "s", 54);
  display.display();
}

// Each press switches network and restarts the countdown.
int chooseWifiSlot(int chosen, const String ssids[3], const int32_t rssi[3]) {
  uint32_t deadline = millis() + WIFI_CHOICE_MS;
  bool automatic = true;
  bool wasPressed = digitalRead(LIST_BUTTON_PIN) == LOW;
  while (static_cast<int32_t>(deadline - millis()) > 0) {
    const bool pressed = digitalRead(LIST_BUTTON_PIN) == LOW;
    if (pressed && !wasPressed) {
      chosen = chosen == 1 ? 2 : 1;
      automatic = false;
      deadline = millis() + WIFI_CHOICE_MS;
    }
    wasPressed = pressed;
    drawWifiChoice(chosen, automatic, ssids, rssi, deadline - millis());
    delay(BUTTON_DEBOUNCE_MS);
  }
  return chosen;
}

bool connectWifiSlot(int slot, const String ssids[3]) {
  if (ssids[slot].isEmpty()) return false;
  detail = ssids[slot];
  drawStatus();
  WiFi.begin(ssids[slot].c_str(), preferences.getString(wifiPassKey(slot), "").c_str());
  return waitForWifi(12000);
}

bool configureWifi() {
  preferences.begin("standalone", false);
  WiFi.mode(WIFI_STA);

  const String ssids[3] = {"", preferences.getString(wifiSsidKey(1), ""),
                           preferences.getString(wifiSsidKey(2), "")};
  int chosen = preferences.getInt("wifi_choice", 1) == 2 ? 2 : 1;
  if (ssids[chosen].isEmpty()) chosen = chosen == 1 ? 2 : 1;
  if (!ssids[1].isEmpty() && !ssids[2].isEmpty()) {
    appState = AppState::connectingWifi;
    detail = "Skenee tiklus...";
    drawStatus();
    int32_t rssi[3];
    scanWifiSlots(ssids, rssi);
    // Neither visible: keep the last choice; otherwise take the stronger one.
    if (rssi[1] != RSSI_NOT_SEEN || rssi[2] != RSSI_NOT_SEEN) {
      chosen = rssi[1] >= rssi[2] ? 1 : 2;
    }
    chosen = chooseWifiSlot(chosen, ssids, rssi);
    preferences.putInt("wifi_choice", chosen);
  }

  appState = AppState::connectingWifi;
  const int other = chosen == 1 ? 2 : 1;
  bool connected = connectWifiSlot(chosen, ssids) || connectWifiSlot(other, ssids);
  if (!connected && ssids[1].isEmpty() && ssids[2].isEmpty()) {
    // Compatibility with the Wi-Fi profile saved by the original setup.
    detail = "Saved network";
    drawStatus();
    WiFi.begin();
    connected = waitForWifi(12000);
    if (connected) {
      preferences.putString(wifiSsidKey(1), WiFi.SSID());
      preferences.putString(wifiPassKey(1), WiFi.psk());
    }
  }

  if (!connected) {
    detail = "AP Melluzi-Standalone";
    drawStatus();
    WiFiManager manager;
    manager.setConnectTimeout(20);
    manager.setConfigPortalTimeout(300);
    connected = manager.autoConnect("Melluzi-Standalone");
    // Remember a network entered in the portal in a free (or the chosen) slot.
    if (connected) {
      const int slot = ssids[1].isEmpty() ? 1 : ssids[2].isEmpty() ? 2 : chosen;
      preferences.putString(wifiSsidKey(slot), WiFi.SSID());
      preferences.putString(wifiPassKey(slot), WiFi.psk());
      preferences.putInt("wifi_choice", slot);
    }
  }
  if (connected) {
    detail = WiFi.SSID() + " " + WiFi.localIP().toString();
    drawStatus();
    delay(5000);
  }
  return connected;
}

bool synchronizeClock() {
  appState = AppState::preparingCache;
  detail = "Syncing Riga time";
  drawStatus();
  configTzTime(LOCAL_TIMEZONE, "time.cloudflare.com", "pool.ntp.org");
  struct tm localTime;
  for (uint8_t attempt = 0; attempt < 20; ++attempt) {
    if (getLocalTime(&localTime, 500)) return true;
    delay(250);
  }
  detail = "NTP time failed";
  return false;
}

bool prepareCache() {
  appState = AppState::preparingCache;
  detail = "FFat";
  drawStatus();

  if (!FFat.begin(true)) {
    detail = "FFat failed";
    return false;
  }

  // Reserve the cache location now. The downloader writes into .tmp and
  // rename atomically only after the ZIP has been validated.
  if (!FFat.exists("/gtfs")) FFat.mkdir("/gtfs");
  return true;
}

// unzipLIB is storage-agnostic. These callbacks connect it to FFat without
// loading the ZIP or an entire CSV file into RAM.
void *openGtfsZip(const char *filename, int32_t *size) {
  gtfsZipFile = FFat.open(filename, FILE_READ);
  if (!gtfsZipFile) {
    *size = 0;
    return nullptr;
  }
  *size = gtfsZipFile.size();
  return &gtfsZipFile;
}

void closeGtfsZip(void *handle) {
  ZIPFILE *zipFile = static_cast<ZIPFILE *>(handle);
  File *file = static_cast<File *>(zipFile->fHandle);
  if (file) file->close();
}

int32_t readGtfsZip(void *handle, uint8_t *buffer, int32_t length) {
  ZIPFILE *zipFile = static_cast<ZIPFILE *>(handle);
  File *file = static_cast<File *>(zipFile->fHandle);
  return file ? file->read(buffer, length) : 0;
}

int32_t seekGtfsZip(void *handle, int32_t position, int origin) {
  ZIPFILE *zipFile = static_cast<ZIPFILE *>(handle);
  File *file = static_cast<File *>(zipFile->fHandle);
  if (!file) return 0;
  if (origin == SEEK_SET) return file->seek(position);
  if (origin == SEEK_END) return file->seek(zipFile->iSize + position);
  return file->seek(file->position() + position);
}

bool requiredGtfsFile(const char *name) {
  return strcmp(name, "calendar.txt") == 0 ||
         strcmp(name, "calendar_dates.txt") == 0 ||
         strcmp(name, "trips.txt") == 0 ||
         strcmp(name, "stop_times.txt") == 0;
}

bool inspectGtfsArchive() {
  if (gtfsZip.openZIP(STATIC_FILE, openGtfsZip, closeGtfsZip, readGtfsZip,
                      seekGtfsZip) != UNZ_OK) {
    detail = "Cannot open GTFS ZIP";
    return false;
  }

  uint8_t foundFiles = 0;
  char name[64];
  char comment[1];
  unz_file_info info;
  int result = gtfsZip.gotoFirstFile();
  while (result == UNZ_OK) {
    if (gtfsZip.getFileInfo(&info, name, sizeof(name), nullptr, 0, comment,
                            sizeof(comment)) == UNZ_OK &&
        requiredGtfsFile(name)) {
      ++foundFiles;
    }
    result = gtfsZip.gotoNextFile();
  }
  gtfsZip.closeZIP();
  if (foundFiles != 4) {
    detail = "GTFS CSV missing";
    return false;
  }
  detail = "GTFS ZIP verified";
  return true;
}

struct ZipLineReader {
  uint8_t buffer[512];
  size_t position = 0;
  size_t available = 0;

  bool next(char *line, size_t lineSize) {
    size_t length = 0;
    while (true) {
      if (position == available) {
        const int bytes = gtfsZip.readCurrentFile(buffer, sizeof(buffer));
        if (bytes <= 0) return length > 0;
        position = 0;
        available = bytes;
      }
      const char value = static_cast<char>(buffer[position++]);
      if (value == '\n') {
        line[length] = '\0';
        if (length && line[length - 1] == '\r') line[length - 1] = '\0';
        return true;
      }
      if (length + 1 < lineSize) line[length++] = value;
    }
  }
};

bool openGtfsEntry(const char *name) {
  return gtfsZip.openZIP(STATIC_FILE, openGtfsZip, closeGtfsZip, readGtfsZip,
                         seekGtfsZip) == UNZ_OK &&
         gtfsZip.locateFile(name) == UNZ_OK &&
         gtfsZip.openCurrentFile() == UNZ_OK;
}

void closeGtfsEntry() {
  gtfsZip.closeCurrentFile();
  gtfsZip.closeZIP();
}

bool csvField(const char *line, uint8_t wanted, char *output, size_t outputSize) {
  uint8_t field = 0;
  bool quoted = false;
  size_t length = 0;
  for (const char *cursor = line;; ++cursor) {
    const char value = *cursor;
    if (value == '"') {
      quoted = !quoted;
    } else if ((value == ',' && !quoted) || value == '\0') {
      if (field == wanted) {
        output[length] = '\0';
        return true;
      }
      if (value == '\0') return false;
      ++field;
      length = 0;
    } else if (field == wanted && length + 1 < outputSize) {
      output[length++] = value;
    }
  }
}

bool serviceIsActive(const char *serviceId) {
  for (size_t index = 0; index < activeServiceCount; ++index) {
    if (strcmp(activeServices[index], serviceId) == 0) return true;
  }
  return false;
}

void setActiveService(const char *serviceId, bool active) {
  for (size_t index = 0; index < activeServiceCount; ++index) {
    if (strcmp(activeServices[index], serviceId) != 0) continue;
    if (!active) {
      strcpy(activeServices[index], activeServices[--activeServiceCount]);
    }
    return;
  }
  if (active && activeServiceCount < MAX_ACTIVE_SERVICES) {
    strlcpy(activeServices[activeServiceCount++], serviceId,
            sizeof(activeServices[0]));
  }
}

bool loadActiveServices(uint8_t dayOffset) {
  struct tm now;
  if (!getLocalTime(&now)) {
    detail = "Clock unavailable";
    return false;
  }
  time_t target = mktime(&now) + dayOffset * 86400;
  localtime_r(&target, &now);
  const int today = (now.tm_year + 1900) * 10000 + (now.tm_mon + 1) * 100 + now.tm_mday;
  const uint8_t weekdayColumn = now.tm_wday == 0 ? 7 : now.tm_wday;
  activeServiceCount = 0;
  char line[256], serviceId[32], weekday[4], startDate[12], endDate[12];

  if (!openGtfsEntry("calendar.txt")) {
    detail = "calendar.txt failed";
    return false;
  }
  ZipLineReader reader;
  reader.next(line, sizeof(line)); // header
  while (reader.next(line, sizeof(line))) {
    if (!csvField(line, 0, serviceId, sizeof(serviceId)) ||
        !csvField(line, weekdayColumn, weekday, sizeof(weekday)) ||
        !csvField(line, 8, startDate, sizeof(startDate)) ||
        !csvField(line, 9, endDate, sizeof(endDate))) continue;
    if (atoi(weekday) == 1 && today >= atoi(startDate) && today <= atoi(endDate)) {
      setActiveService(serviceId, true);
    }
  }
  closeGtfsEntry();

  if (!openGtfsEntry("calendar_dates.txt")) {
    detail = "calendar_dates failed";
    return false;
  }
  reader = ZipLineReader();
  reader.next(line, sizeof(line));
  char date[12], exception[4];
  while (reader.next(line, sizeof(line))) {
    if (!csvField(line, 0, serviceId, sizeof(serviceId)) ||
        !csvField(line, 1, date, sizeof(date)) ||
        !csvField(line, 2, exception, sizeof(exception))) continue;
    if (atoi(date) == today) setActiveService(serviceId, atoi(exception) == 1);
  }
  closeGtfsEntry();
  detail = String(activeServiceCount) + " active services";
  return activeServiceCount > 0;
}

uint32_t parseGtfsTime(const char *value) {
  int hours = 0, minutes = 0, seconds = 0;
  sscanf(value, "%d:%d:%d", &hours, &minutes, &seconds);
  return hours * 3600UL + minutes * 60UL + seconds;
}

TripCandidate *findCandidate(const char *tripId, bool create) {
  for (size_t index = 0; index < candidateCount; ++index) {
    if (candidates[index].dayOffset == buildingDayOffset &&
        strcmp(candidates[index].tripId, tripId) == 0) return &candidates[index];
  }
  if (!create || candidateCount == MAX_CANDIDATES) return nullptr;
  TripCandidate *candidate = &candidates[candidateCount++];
  *candidate = TripCandidate();
  candidate->dayOffset = buildingDayOffset;
  strlcpy(candidate->tripId, tripId, sizeof(candidate->tripId));
  return candidate;
}

bool loadStopCandidates() {
  if (!openGtfsEntry("stop_times.txt")) {
    detail = "stop_times failed";
    return false;
  }
  ZipLineReader reader;
  char line[256], tripId[24], arrival[12], departure[12], stopId[12], sequence[12];
  reader.next(line, sizeof(line));
  while (reader.next(line, sizeof(line))) {
    if (!csvField(line, 0, tripId, sizeof(tripId)) ||
        !csvField(line, 1, arrival, sizeof(arrival)) ||
        !csvField(line, 2, departure, sizeof(departure)) ||
        !csvField(line, 3, stopId, sizeof(stopId)) ||
        !csvField(line, 4, sequence, sizeof(sequence))) continue;
    if (strcmp(stopId, ORIGIN_STOP_ID) != 0 &&
        strcmp(stopId, DESTINATION_STOP_ID) != 0) continue;
    TripCandidate *candidate = findCandidate(tripId, true);
    if (!candidate) continue;
    if (strcmp(stopId, ORIGIN_STOP_ID) == 0) {
      candidate->hasOrigin = true;
      candidate->originSequence = atoi(sequence);
      candidate->departureSeconds = parseGtfsTime(departure);
    } else {
      candidate->hasDestination = true;
      candidate->destinationSequence = atoi(sequence);
      candidate->arrivalSeconds = parseGtfsTime(arrival);
    }
  }
  closeGtfsEntry();
  return candidateCount > 0;
}

bool resolveActiveTrips(uint8_t dayOffset) {
  if (!openGtfsEntry("trips.txt")) {
    detail = "trips.txt failed";
    return false;
  }
  ZipLineReader reader;
  char line[256], serviceId[32], tripId[24];
  reader.next(line, sizeof(line));
  while (reader.next(line, sizeof(line))) {
    if (!csvField(line, 1, serviceId, sizeof(serviceId)) ||
        !csvField(line, 2, tripId, sizeof(tripId))) continue;
    TripCandidate *candidate = findCandidate(tripId, false);
    if (candidate && candidate->dayOffset == dayOffset)
      candidate->activeToday = serviceIsActive(serviceId);
  }
  closeGtfsEntry();

  size_t valid = 0;
  for (size_t index = 0; index < candidateCount; ++index) {
    const TripCandidate &candidate = candidates[index];
    if (candidate.dayOffset == dayOffset && candidate.activeToday &&
        candidate.hasOrigin && candidate.hasDestination &&
        candidate.originSequence < candidate.destinationSequence &&
        candidate.arrivalSeconds >= candidate.departureSeconds) ++valid;
  }
  detail = String(valid) + " Melluzi-Riga";
  return valid > 0;
}

bool currentTimeSeconds(uint32_t *seconds) {
  struct tm now;
  if (!getLocalTime(&now)) return false;
  *seconds = simulatedClockSeconds >= 0
      ? static_cast<uint32_t>(simulatedClockSeconds)
      : now.tm_hour * 3600UL + now.tm_min * 60UL + now.tm_sec;
  return true;
}

uint32_t candidateDeparture(const TripCandidate &candidate) {
  return candidate.departureSeconds + candidate.dayOffset * 86400UL +
      (candidate.realtime ? max<int32_t>(0, candidate.delaySeconds) : 0);
}

bool isUpcomingCandidate(const TripCandidate &candidate, uint32_t currentSeconds) {
  return candidate.activeToday && candidate.hasOrigin && candidate.hasDestination &&
      candidate.originSequence < candidate.destinationSequence &&
      candidate.arrivalSeconds >= candidate.departureSeconds && !candidate.cancelled &&
      candidateDeparture(candidate) >= currentSeconds + 5 * 60UL;
}

// Returns the Nth future train without allocating or sorting an extra array.
// At most 1024 candidates are scanned, which is inexpensive for a button press.
const TripCandidate *upcomingDepartureAt(size_t ordinal, uint32_t currentSeconds) {
  const TripCandidate *selected = nullptr;
  uint32_t after = currentSeconds + 5 * 60UL;
  for (size_t rank = 0; rank <= ordinal; ++rank) {
    selected = nullptr;
    for (size_t index = 0; index < candidateCount; ++index) {
      const TripCandidate &candidate = candidates[index];
      const uint32_t departure = candidateDeparture(candidate);
      if (!isUpcomingCandidate(candidate, currentSeconds) || departure < after) continue;
      if (!selected || departure < candidateDeparture(*selected)) selected = &candidate;
    }
    if (!selected) return nullptr;
    after = candidateDeparture(*selected) + 1;
  }
  return selected;
}

const TripCandidate *nextDeparture(uint32_t *remaining) {
  uint32_t currentSeconds = 0;
  if (!currentTimeSeconds(&currentSeconds)) return nullptr;
  const TripCandidate *next = nullptr;
  for (size_t index = 0; index < candidateCount; ++index) {
    const TripCandidate &candidate = candidates[index];
    if (!isUpcomingCandidate(candidate, currentSeconds)) continue;
    if (!next || candidateDeparture(candidate) < candidateDeparture(*next)) next = &candidate;
  }
  if (next && remaining) *remaining = candidateDeparture(*next) - currentSeconds;
  return next;
}

void formatGtfsTime(uint32_t seconds, char *output, size_t outputSize) {
  snprintf(output, outputSize, "%02lu:%02lu", (seconds / 3600UL) % 24UL,
           (seconds / 60UL) % 60UL);
}

const char *weekdayLabel(uint8_t weekday) {
  static const char *const labels[] = {"Sv", "1d", "2d", "3d", "4d", "5d", "Se"};
  return labels[weekday % 7];
}

void drawUpcomingList() {
  uint32_t currentSeconds = 0;
  if (!currentTimeSeconds(&currentSeconds)) return;
  const TripCandidate *first = upcomingDepartureAt(upcomingListPage * 3, currentSeconds);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  struct tm today;
  uint8_t weekday = 0;
  if (getLocalTime(&today)) weekday = (today.tm_wday + (first ? first->dayOffset : 0)) % 7;
  display.setCursor(0, 0); display.print(weekdayLabel(weekday));
  display.setCursor(18, 0); display.print("MELLUZI");
  display.setCursor(104, 0); display.print("RIGA");

  bool any = false;
  for (uint8_t row = 0; row < 3; ++row) {
    const TripCandidate *candidate =
        upcomingDepartureAt(upcomingListPage * 3 + row, currentSeconds);
    if (!candidate) break;
    any = true;
    const uint32_t delay = candidate->realtime ? max<int32_t>(0, candidate->delaySeconds) : 0;
    char departure[6], arrival[6];
    formatGtfsTime(candidate->departureSeconds + delay, departure, sizeof(departure));
    formatGtfsTime(candidate->arrivalSeconds + delay, arrival, sizeof(arrival));
    display.setTextSize(2);
    display.setCursor(0, 12 + row * 17); display.print(departure);
    display.setCursor(68, 12 + row * 17); display.print(arrival);
  }
  if (!any) {
    display.setTextSize(1);
    centeredText("NO MORE TRAINS", 30);
  }
  // The unframed 2-pixel bar is the eight-second return-to-main-screen timer.
  const uint32_t elapsed = min<uint32_t>(millis() - lastListInteraction,
                                         LIST_VIEW_TIMEOUT_MS);
  const int progressWidth = static_cast<int>(
      ((LIST_VIEW_TIMEOUT_MS - elapsed) * SCREEN_WIDTH) / LIST_VIEW_TIMEOUT_MS);
  if (progressWidth > 0) display.fillRect(0, SCREEN_HEIGHT - 2, progressWidth, 2,
                                          SSD1306_WHITE);
  display.display();
}

void handleListButton() {
  static bool rawPrevious = HIGH;
  static bool stableState = HIGH;
  static uint32_t changedAt = 0;
  const bool rawState = digitalRead(LIST_BUTTON_PIN);
  const uint32_t now = millis();
  if (rawState != rawPrevious) {
    rawPrevious = rawState;
    changedAt = now;
  }
  if (rawState == stableState || now - changedAt < BUTTON_DEBOUNCE_MS) return;
  stableState = rawState;
  if (stableState != LOW) return;  // Act once on button press, not release.

  if (!showingUpcomingList) {
    showingUpcomingList = true;
    upcomingListPage = 0;
  } else {
    uint32_t currentSeconds = 0;
    const size_t nextPageFirst = (upcomingListPage + 1) * 3;
    if (currentTimeSeconds(&currentSeconds) &&
        upcomingDepartureAt(nextPageFirst, currentSeconds)) {
      ++upcomingListPage;
    } else {
      upcomingListPage = 0;
    }
  }
  lastListInteraction = now;
}

void drawTrainScreen() {
  uint32_t remaining = 0;
  const TripCandidate *next = nextDeparture(&remaining);
  if (!next) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    centeredText("NO MORE TRAINS", 25);
    display.display();
    return;
  }
  if (strcmp(displayedTripId, next->tripId) != 0) {
    strlcpy(displayedTripId, next->tripId, sizeof(displayedTripId));
    countdownRange = max<uint32_t>(1, remaining);
  }
  char departure[6], arrival[6], countdown[12];
  const uint32_t delay = next->realtime ? max<int32_t>(0, next->delaySeconds) : 0;
  formatGtfsTime(next->departureSeconds + delay, departure, sizeof(departure));
  formatGtfsTime(next->arrivalSeconds + delay, arrival, sizeof(arrival));
  const bool showHours = remaining > 99UL * 60UL;
  if (showHours) {
    const uint32_t hours = remaining / 3600UL;
    if (hours >= 9) {
      snprintf(countdown, sizeof(countdown), "%lu+h", hours);
    } else {
      snprintf(countdown, sizeof(countdown), "%luh:%02lu", hours,
               (remaining / 60UL) % 60UL);
    }
  } else {
    snprintf(countdown, sizeof(countdown), "%02lu:%02lu", remaining / 60UL,
             remaining % 60UL);
  }
  const char *shownCountdown = customCountdownText.isEmpty()
      ? countdown : customCountdownText.c_str();
  const DepartureGuideState guideState = departureGuideState(remaining);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0); display.print(originLabel);
  display.setCursor(68, 0); display.print(destinationLabel);
  if (realtimeStale) {
    display.setCursor(56, 0); display.print("!");
    display.setCursor(42, 0); display.print(lastRealtimeStatus);
  }
  display.setTextSize(2);
  if (customTimesText.isEmpty()) {
    display.setCursor(0, 9); display.print(departure);
    display.setCursor(68, 9); display.print(arrival);
  } else {
    // Ten characters at 2x default font use 120 px, which fits the full OLED.
    display.setCursor(0, 9); display.print(customTimesText);
  }
  // V3: left half is the movement guide; the large countdown is anchored to
  // the right edge so it is the first thing visible at a glance.
  drawDepartureGuideAnimation(display, guideState, 3, 32, millis());
  display.setTextSize(showHours ? 3 : 3);
  display.setCursor(38, 29);
  display.print(shownCountdown);
  constexpr int barX = 38;
  constexpr int barY = 57;
  constexpr int barWidth = 90;
  display.drawRect(barX, barY, barWidth, 5, SSD1306_WHITE);
  display.fillRect(barX + 1, barY + 1, barWidth - 2, 3, SSD1306_WHITE);
  const int emptied = constrain(
      static_cast<int>(((countdownRange - min(remaining, countdownRange)) *
                        static_cast<uint32_t>(barWidth - 2)) /
                       max<uint32_t>(1, countdownRange)),
      0, barWidth - 2);
  if (emptied) display.fillRect(barX + 1, barY + 1, emptied, 3, SSD1306_BLACK);
  display.display();
}

bool hasZipSignature(File &file) {
  uint8_t signature[4];
  file.seek(0);
  return file.read(signature, sizeof(signature)) == sizeof(signature) &&
         signature[0] == 'P' && signature[1] == 'K' &&
         (signature[2] == 3 || signature[2] == 5 || signature[2] == 7) &&
         (signature[3] == 4 || signature[3] == 6 || signature[3] == 8);
}

bool refreshStaticGtfs() {
  appState = AppState::downloadingStatic;
  detail = "Checking update";
  drawStatus();

  WiFiClientSecure client;
  // Certificate validation is added alongside NTP synchronization. Without a
  // trusted clock, validation would reject the first boot's HTTPS request.
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(15000);
  const char *headerNames[] = {"ETag", "Last-Modified"};
  http.collectHeaders(headerNames, 2);
  if (!http.begin(client, STATIC_GTFS_URL)) {
    detail = "HTTPS setup failed";
    return false;
  }

  const String etag = preferences.getString("gtfs_etag", "");
  const String lastModified = preferences.getString("gtfs_modified", "");
  if (!etag.isEmpty()) http.addHeader("If-None-Match", etag);
  if (!lastModified.isEmpty()) http.addHeader("If-Modified-Since", lastModified);

  const int status = http.GET();
  if (status == HTTP_CODE_NOT_MODIFIED) {
    http.end();
    detail = "Cache is current";
    return FFat.exists(STATIC_FILE);
  }
  if (status != HTTP_CODE_OK) {
    detail = "HTTP " + String(status);
    http.end();
    return false;
  }
  const String responseEtag = http.header("ETag");
  const String responseLastModified = http.header("Last-Modified");

  const int totalSize = http.getSize();
  if (totalSize <= 4 || totalSize > static_cast<int>(MAX_STATIC_GTFS_BYTES)) {
    detail = "GTFS size invalid";
    http.end();
    return false;
  }
  // 4 MB Super Mini: FFat holds only one GTFS copy, so drop the old one first.
  if (FFat.totalBytes() - FFat.usedBytes() < static_cast<size_t>(totalSize) &&
      FFat.exists(STATIC_FILE)) {
    FFat.remove(STATIC_FILE);
    preferences.remove("gtfs_etag");
    preferences.remove("gtfs_modified");
  }
  if (FFat.totalBytes() - FFat.usedBytes() < static_cast<size_t>(totalSize)) {
    detail = "Not enough flash";
    http.end();
    return false;
  }

  FFat.remove(STATIC_TEMP_FILE);
  File output = FFat.open(STATIC_TEMP_FILE, FILE_WRITE);
  if (!output) {
    detail = "Cannot write cache";
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buffer[DOWNLOAD_BUFFER_SIZE];
  size_t received = 0;
  uint32_t lastProgressAt = 0;
  while (http.connected() && received < static_cast<size_t>(totalSize)) {
    const size_t available = stream->available();
    if (available == 0) {
      delay(1);
      continue;
    }
    const size_t toRead = min(available, sizeof(buffer));
    const size_t read = stream->readBytes(buffer, toRead);
    if (read == 0 || output.write(buffer, read) != read) break;
    received += read;
    if (millis() - lastProgressAt >= 1000) {
      detail = String((received * 100UL) / totalSize) + "%";
      drawStatus();
      lastProgressAt = millis();
    }
  }
  output.close();
  http.end();

  File downloaded = FFat.open(STATIC_TEMP_FILE, FILE_READ);
  const bool valid = downloaded && received == static_cast<size_t>(totalSize) &&
                     downloaded.size() == received && hasZipSignature(downloaded);
  if (downloaded) downloaded.close();
  if (!valid) {
    FFat.remove(STATIC_TEMP_FILE);
    detail = "GTFS download failed";
    return false;
  }

  FFat.remove(STATIC_FILE);
  if (!FFat.rename(STATIC_TEMP_FILE, STATIC_FILE)) {
    detail = "Cache rename failed";
    return false;
  }
  preferences.putString("gtfs_etag", responseEtag);
  preferences.putString("gtfs_modified", responseLastModified);
  preferences.putULong("gtfs_updated", millis());
  detail = "GTFS cached";
  return true;
}

bool refreshRealtime() {
  if (WiFi.status() != WL_CONNECTED) return false;
  for (size_t index = 0; index < candidateCount; ++index) {
    candidates[index].realtime = false;
    candidates[index].cancelled = false;
    candidates[index].delaySeconds = 0;
  }
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(15000);
  if (!http.begin(client, REALTIME_URL)) { lastRealtimeStatus = -1; return false; }
  http.setUserAgent("MelluziTrainDisplay/2.0");
  const int status = http.GET();
  lastRealtimeStatus = status;
  const int advertisedSize = http.getSize();
  constexpr size_t MAX_REALTIME_BYTES = 512 * 1024;
  if (status != HTTP_CODE_OK || advertisedSize > static_cast<int>(MAX_REALTIME_BYTES)) {
    http.end();
    return false;
  }
  const size_t capacity = advertisedSize > 0 ? advertisedSize : MAX_REALTIME_BYTES;
  uint8_t *payload = static_cast<uint8_t *>(ps_malloc(capacity));
  if (!payload) payload = static_cast<uint8_t *>(malloc(capacity));
  if (!payload) { http.end(); lastRealtimeStatus = -2; return false; }
  WiFiClient *stream = http.getStreamPtr();
  size_t received = 0;
  uint32_t lastBytesAt = millis();
  while ((http.connected() || stream->available()) && received < capacity) {
    const size_t available = stream->available();
    if (!available) {
      if (millis() - lastBytesAt > 5000) break;
      delay(1);
      continue;
    }
    const size_t read = stream->readBytes(payload + received,
                                          min(available, capacity - received));
    if (!read) break;
    received += read;
    lastBytesAt = millis();
  }
  http.end();
  const bool complete = received > 0 &&
      (advertisedSize <= 0 || received == static_cast<size_t>(advertisedSize));
  if (complete) parseRealtimeFeed(payload, received);
  free(payload);
  realtimeStale = !complete;
  if (!complete) lastRealtimeStatus = -3;
  lastRealtimeRefresh = millis();
  return !realtimeStale;
}

void setup() {
  Serial.begin(115200);
  pinMode(LIST_BUTTON_PIN, INPUT_PULLUP);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    while (true) delay(1000);
  }

  if (!configureWifi()) {
    appState = AppState::error;
    detail = "Wi-Fi failed";
    drawStatus();
    return;
  }
  originLabel = preferences.getString("origin_label", "MEL");
  destinationLabel = preferences.getString("destination_label", "RIGA");
  simulatedClockSeconds = preferences.getInt("sim_clock", -1);
  customTimesText = preferences.getString("custom_times", "");
  customCountdownText = preferences.getString("custom_countdown", "");
  startConfigurationWeb();
  if (!synchronizeClock()) {
    appState = AppState::error;
    drawStatus();
    return;
  }
  if (!prepareCache()) {
    appState = AppState::error;
    drawStatus();
    return;
  }
  if (!refreshStaticGtfs() && !FFat.exists(STATIC_FILE)) {
    appState = AppState::error;
    drawStatus();
    return;
  }
  if (!inspectGtfsArchive()) {
    appState = AppState::error;
    drawStatus();
    return;
  }
  appState = AppState::parsingStatic;
  detail = "Finding services";
  drawStatus();
  candidateCount = 0;
  bool haveTrips = false;
  for (buildingDayOffset = 0; buildingDayOffset <= 1; ++buildingDayOffset) {
    if (!loadActiveServices(buildingDayOffset)) continue;
    detail = buildingDayOffset ? "Reading tomorrow" : "Reading today";
    drawStatus();
    haveTrips = loadStopCandidates() && resolveActiveTrips(buildingDayOffset) || haveTrips;
  }
  if (!haveTrips) {
    appState = AppState::error;
    drawStatus();
    return;
  }
  struct tm loadedTime;
  getLocalTime(&loadedTime);
  loadedYearDay = loadedTime.tm_yday;
  buildingDayOffset = 0;
  refreshRealtime();

  appState = AppState::ready;
  Serial.printf("Static GTFS: %s\nRealtime: %s\n", STATIC_GTFS_URL,
                REALTIME_URL);
}

void restartNightlyIfDue() {
  static uint32_t lastCheckAt = 0;
  if (millis() - lastCheckAt < 60000UL) return;
  lastCheckAt = millis();
  struct tm now;
  if (millis() < DAILY_RESTART_MIN_UPTIME_MS || !getLocalTime(&now, 0)) return;
  if (now.tm_hour != DAILY_RESTART_HOUR || now.tm_yday == loadedYearDay) return;
  appState = AppState::starting;
  drawStatus();
  delay(500);
  ESP.restart();
}

constexpr uint32_t RESTART_HOLD_MS = 15000;
constexpr uint32_t RESTART_HINT_MS = 3000;

// Returns true while the restart countdown owns the screen.
bool handleRestartHold() {
  static uint32_t pressedSince = 0;
  static bool holding = false;
  if (digitalRead(LIST_BUTTON_PIN) != LOW) {
    holding = false;
    return false;
  }
  const uint32_t now = millis();
  if (!holding) {
    holding = true;
    pressedSince = now;
  }
  const uint32_t held = now - pressedSince;
  if (held < RESTART_HINT_MS) return false;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  if (held >= RESTART_HOLD_MS) {
    centeredText("RESTARTS...", 28);
    display.display();
    delay(500);
    ESP.restart();
  }
  centeredText("TURI POGU", 8);
  centeredText("RESTARTS PEC " + String((RESTART_HOLD_MS - held + 999) / 1000) + "s", 24);
  centeredText("ATLAID = ATCELT", 38);
  display.drawRect(0, 52, SCREEN_WIDTH, 8, SSD1306_WHITE);
  display.fillRect(1, 53, ((held - RESTART_HINT_MS) * (SCREEN_WIDTH - 2)) /
                              (RESTART_HOLD_MS - RESTART_HINT_MS), 6, SSD1306_WHITE);
  display.display();
  return true;
}

void loop() {
  restartNightlyIfDue();
  webServer.handleClient();
  if (millis() - lastRealtimeRefresh >= REALTIME_REFRESH_MS) refreshRealtime();
  handleListButton();
  if (handleRestartHold()) {
    delay(20);
    return;
  }
  if (showingUpcomingList && millis() - lastListInteraction >= LIST_VIEW_TIMEOUT_MS) {
    showingUpcomingList = false;
  }
  if (appState == AppState::ready) {
    if (showingUpcomingList) drawUpcomingList();
    else drawTrainScreen();
  }
  delay(20);
}
