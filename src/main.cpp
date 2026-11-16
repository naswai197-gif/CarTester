#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include <INA226_WE.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <ElegantOTA.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

#include "web_page.h" // Наш HTML

// ===================== НАСТРОЙКИ =====================
const char* WIFI_SSID = "Car-Tester-S3";
const char* WIFI_PASS = "12345678";
const char* FW_VERSION = "v1.9.0"; // Версия прошивки
const char* GITHUB_REPO = "https://github.com/naswai197-gif/CarTester.git"; // Сюда вписать ссылку на репо!

const float SHUNT_OHMS = 0.0015; 
const float MAX_AMPS   = 60.0;   

// Переменные калибровки
float CURRENT_OFFSET_A   = 0.0000;
float CURRENT_CAL_FACTOR = 1.00;
float VOLTAGE_CAL_FACTOR = 1.00;

float batteryCapacityAh = 60.0;
float leakageLimitMa    = 70.0;

const unsigned long ACTIVE_LOG_MS   = 1000;
const unsigned long SLEEP_LOG_MS    = 10000;
const unsigned long SENSOR_READ_MS  = 500;
const int GRAPH_POINTS = 1200;

// Пины
const int PIN_SDA = 1;
const int PIN_SCL = 2;
const int PIN_RGB = 21;
const int PIN_BAT_ADC = 4; // Пин для делителя батареи

// ===================== СИСТЕМА =====================
#define INA_ADDRESS 0x40
INA226_WE ina226 = INA226_WE(INA_ADDRESS);
Adafruit_NeoPixel pixels(1, PIN_RGB, NEO_GRB + NEO_KHZ800);
AsyncWebServer server(80);
Preferences prefs;

float graphHistory[GRAPH_POINTS];
unsigned long graphTimes[GRAPH_POINTS];
int graphIndex = 0;
bool graphFull = false;
unsigned long totalSamplesEver = 0;

enum SessionState { STATE_IDLE = 0, STATE_RECORDING = 1, STATE_FINISHED = 2 };
SessionState sessionState = STATE_IDLE;

String carModel = "";
String carPlate = "";
String testDate = "";

unsigned long baseTimeSec = 0;
unsigned long syncMillis = 0;
bool timeSynced = false;
unsigned long sessionStartSec = 0;
String startTimeStr = "--:--:--";
String endTimeStr   = "--:--:--";

float statMinMa = 999999.0;
float statMaxMa = 0.0;
double statSumMa = 0.0;
unsigned long statSamples = 0;

const int SLEEP_WINDOW = 30;
float sleepBuf[SLEEP_WINDOW];
int sleepBufIdx = 0;
int sleepBufCount = 0;
bool isAsleep = false;
bool sleepEverDetected = false;
String sleepTimeStr = "";
String sleepDurationStr = "";
float sleepQuiescentMa = 0.0;

struct KeyEvent { String timeStr; String title; String details; };
const int MAX_EVENTS = 25;
KeyEvent keyEvents[MAX_EVENTS];
int keyEventCount = 0;

unsigned long lastSampleTime = 0;
unsigned long lastCsvWriteTime = 0;
bool memoryFullStop = false;

float cachedVoltage = 0.0;
float cachedCurrentA = 0.0;
float cachedCurrentmA = 0.0;
unsigned long lastSensorRead = 0;
float batteryPercent = 0.0;

// Защита от долгого тока
unsigned long highCurrentStart = 0;
bool highCurrentWarning = false;
unsigned long lastLedBlink = 0;
bool ledState = false;

// Сеть
String savedSsid = "";
String savedPass = "";
String currentIp = "";

File logFile;
bool logFileOpen = false;

// ===================== УТИЛИТЫ =====================
void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
  pixels.setPixelColor(0, pixels.Color(r, g, b));
  pixels.show();
}

unsigned long getCurrentDaySec() {
  unsigned long elapsedSec = (millis() - syncMillis) / 1000;
  return (baseTimeSec + elapsedSec) % 86400;
}

String formatSecToHMS(unsigned long totalSec) {
  totalSec %= 86400;
  int hh = (int)(totalSec / 3600);
  int mm = (int)((totalSec % 3600) / 60);
  int ss = (int)(totalSec % 60);
  char buf[12];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hh, mm, ss);
  return String(buf);
}

void jsonEscape(char* dst, size_t dstSize, const char* src) {
  size_t j = 0;
  for (size_t i = 0; src[i] != '\0' && j + 2 < dstSize; i++) {
    char c = src[i];
    if (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = c; }
    else if (c == '\n') { dst[j++] = '\\'; dst[j++] = 'n'; }
    else if (c == '\r') { dst[j++] = '\\'; dst[j++] = 'r'; }
    else if (c == '\t') { dst[j++] = '\\'; dst[j++] = 't'; }
    else dst[j++] = c;
  }
  dst[j] = '\0';
}

String sanitizeCsvText(String s) {
  s.replace(";", ","); s.replace("\r", " "); s.replace("\n", " "); s.trim();
  return s;
}

void addKeyEvent(const String& timeStr, const String& title, const String& details) {
  if (keyEventCount < MAX_EVENTS) {
    keyEvents[keyEventCount].timeStr = timeStr;
    keyEvents[keyEventCount].title = title;
    keyEvents[keyEventCount].details = details;
    keyEventCount++;
  }
}

// ===================== СЕТЬ (AP+STA) =====================
void setupWiFi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(WIFI_SSID, WIFI_PASS);
  
  prefs.begin("tester", true);
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  prefs.end();

  if (savedSsid.length() > 0) {
    Serial.println("Подключение к " + savedSsid);
    WiFi.begin(savedSsid.c_str(), savedPass.c_str());
    // Ждем подключения 10 секунд, не блокируя процесс
    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      currentIp = WiFi.localIP().toString();
      Serial.println("Подключено! IP: " + currentIp);
    } else {
      Serial.println("Не удалось подключиться к Wi-Fi (работаем только AP)");
    }
  }
}

// ===================== ЛОГИКА СЕССИИ =====================
void closeLogFile() {
  if (logFileOpen) { logFile.flush(); logFile.close(); logFileOpen = false; }
}

void openLogFile() {
  closeLogFile();
  logFile = LittleFS.open("/raw.csv", "a");
  logFileOpen = (bool)logFile;
}

void createRawDataFile() {
  closeLogFile();
  File f = LittleFS.open("/raw.csv", "w");
  if (f) { f.println("Время;Ток (мА);Напряжение (В);Ключевое событие"); f.close(); }
}

void startSession() {
  if (sessionState == STATE_RECORDING) return;
  
  closeLogFile();
  LittleFS.remove("/raw.csv");
  createRawDataFile();
  
  graphIndex = 0; graphFull = false; totalSamplesEver = 0;
  statMinMa = 999999.0; statMaxMa = 0.0; statSumMa = 0.0; statSamples = 0;
  sleepBufIdx = 0; sleepBufCount = 0;
  isAsleep = false; sleepEverDetected = false;
  sleepTimeStr = ""; sleepDurationStr = ""; sleepQuiescentMa = 0.0;
  keyEventCount = 0; endTimeStr = "--:--:--";

  unsigned long nowSec = getCurrentDaySec();
  sessionStartSec = nowSec;
  startTimeStr = formatSecToHMS(nowSec);
  sessionState = STATE_RECORDING;
  memoryFullStop = false;
  lastSampleTime = 0;
  lastCsvWriteTime = 0;
  
  openLogFile();
  addKeyEvent(startTimeStr, "СТАРТ ЗАМЕРА", "U=" + String(cachedVoltage, 2) + " В");
  Serial.println(">>> СТАРТ ЗАМЕРА <<<");
}

void stopSession() {
  if (sessionState != STATE_RECORDING) return;
  unsigned long nowSec = getCurrentDaySec();
  endTimeStr = formatSecToHMS(nowSec);
  sessionState = STATE_FINISHED;
  
  float avgMa = (statSamples > 0) ? (float)(statSumMa / statSamples) : 0.0;
  String verdict = (sleepEverDetected && sleepQuiescentMa <= leakageLimitMa) ? "НОРМА" : "ПРОВЕРИТЬ УТЕЧКУ";
  addKeyEvent(endTimeStr, "ФИНИШ ЗАМЕРА", "Итог: " + verdict + ", сред. ток " + String(avgMa, 0) + " мА");
  
  if (logFileOpen) {
    logFile.printf("%s;%.0f;%.2f;ФИНИШ (%s)\n", endTimeStr.c_str(), cachedCurrentmA, cachedVoltage, verdict.c_str());
    closeLogFile();
  }
}

void resetAllSessionData(bool fullReset) {
  closeLogFile();
  LittleFS.remove("/raw.csv");
  createRawDataFile();
  graphIndex = 0; graphFull = false; totalSamplesEver = 0;
  sessionState = STATE_IDLE; memoryFullStop = false;
  if (!timeSynced) { baseTimeSec = 0; syncMillis = millis(); }
  startTimeStr = "--:--:--"; endTimeStr = "--:--:--";
  statMinMa = 999999.0; statMaxMa = 0.0; statSumMa = 0.0; statSamples = 0;
  sleepBufIdx = 0; sleepBufCount = 0;
  isAsleep = false; sleepEverDetected = false;
  sleepTimeStr = ""; sleepDurationStr = ""; sleepQuiescentMa = 0.0;
  keyEventCount = 0;
  
  if (fullReset) {
    carModel = ""; carPlate = "";
    prefs.begin("tester", false);
    prefs.putString("model", "");
    prefs.putString("plate", "");
    prefs.end();
  }
}

// ===================== WEB ОБРАБОТЧИКИ =====================
void handleSetCalibration(AsyncWebServerRequest *request) {
  prefs.begin("tester", false);
  if (request->hasParam("vol_cal")) { VOLTAGE_CAL_FACTOR = request->getParam("vol_cal")->value().toFloat(); prefs.putFloat("vol_cal", VOLTAGE_CAL_FACTOR); }
  if (request->hasParam("cur_cal")) { CURRENT_CAL_FACTOR = request->getParam("cur_cal")->value().toFloat(); prefs.putFloat("cur_cal", CURRENT_CAL_FACTOR); }
  if (request->hasParam("cur_off")) { CURRENT_OFFSET_A = request->getParam("cur_off")->value().toFloat(); prefs.putFloat("cur_off", CURRENT_OFFSET_A); }
  prefs.end();
  request->send(200, "text/plain", "OK");
}

void handleSetWifi(AsyncWebServerRequest *request) {
  if (request->hasParam("ssid")) {
    String s = request->getParam("ssid")->value();
    String p = request->hasParam("pass") ? request->getParam("pass")->value() : "";
    prefs.begin("tester", false);
    prefs.putString("ssid", s);
    prefs.putString("pass", p);
    prefs.end();
    request->send(200, "text/plain", "OK");
    delay(1000);
    ESP.restart(); // Перезагрузка для применения нового Wi-Fi
  } else {
    request->send(400, "text/plain", "No SSID");
  }
}

void handleStart(AsyncWebServerRequest *request) { startSession(); request->send(200, "text/plain", "STARTED"); }
void handleFinish(AsyncWebServerRequest *request) { stopSession(); request->send(200, "text/plain", "FINISHED"); }
void handleClear(AsyncWebServerRequest *request) { resetAllSessionData(false); request->send(200, "text/plain", "CLEARED"); }
void handleFullReset(AsyncWebServerRequest *request) { resetAllSessionData(true); request->send(200, "text/plain", "FULLRESET"); }

void handleData(AsyncWebServerRequest *request) {
  unsigned long histFrom = 0;
  if (request->hasParam("hist_from")) histFrom = request->getParam("hist_from")->value().toInt();

  static char buf[16384];
  int n = 0;

  char nowTime[12]; 
  {
    unsigned long t = getCurrentDaySec() % 86400;
    snprintf(nowTime, sizeof(nowTime), "%02lu:%02lu:%02lu", t/3600, (t%3600)/60, t%60);
  }

  size_t totalBytes = LittleFS.totalBytes();
  size_t usedBytes  = LittleFS.usedBytes();
  int memPct = (totalBytes > 0) ? (int)((usedBytes * 100UL) / totalBytes) : 0;
  float avgMa = (statSamples > 0) ? (float)(statSumMa / statSamples) : 0.0;
  float minMaShow = (statSamples > 0) ? statMinMa : 0.0;

  n += snprintf(buf + n, sizeof(buf) - n, "{\"v\":%.2f,\"a\":%.2f,\"ma\":%.0f,\"state\":%d,", cachedVoltage, cachedCurrentA, cachedCurrentmA, (int)sessionState);
  n += snprintf(buf + n, sizeof(buf) - n, "\"cap\":%.0f,\"limit\":%.0f,", batteryCapacityAh, leakageLimitMa);
  n += snprintf(buf + n, sizeof(buf) - n, "\"vol_cal\":%.4f,\"cur_cal\":%.2f,\"cur_off\":%.4f,", VOLTAGE_CAL_FACTOR, CURRENT_CAL_FACTOR, CURRENT_OFFSET_A);
  
  n += snprintf(buf + n, sizeof(buf) - n, "\"now\":\"%s\",\"synced\":%s,", nowTime, timeSynced ? "true" : "false");
  n += snprintf(buf + n, sizeof(buf) - n, "\"tStart\":\"%s\",\"tEnd\":\"%s\",", startTimeStr.c_str(), endTimeStr.c_str());
  
  char esc[256];
  jsonEscape(esc, sizeof(esc), carModel.c_str());
  n += snprintf(buf + n, sizeof(buf) - n, "\"model\":\"%s\",", esc);
  jsonEscape(esc, sizeof(esc), carPlate.c_str());
  n += snprintf(buf + n, sizeof(buf) - n, "\"plate\":\"%s\",", esc);
  jsonEscape(esc, sizeof(esc), testDate.c_str());
  n += snprintf(buf + n, sizeof(buf) - n, "\"date\":\"%s\",", esc);
  
  n += snprintf(buf + n, sizeof(buf) - n, "\"memPct\":%d,\"bat\":%.0f,\"ip\":\"%s\",\"ssid\":\"%s\",", memPct, batteryPercent, currentIp.c_str(), savedSsid.c_str());
  n += snprintf(buf + n, sizeof(buf) - n, "\"warnCurrent\":%s,", highCurrentWarning ? "true" : "false");
  
  n += snprintf(buf + n, sizeof(buf) - n, "\"sMin\":%.0f,\"sMax\":%.0f,\"sAvg\":%.0f,", minMaShow, statMaxMa, avgMa);
  n += snprintf(buf + n, sizeof(buf) - n, "\"asleep\":%s,\"sleepFound\":%s,\"sleepTime\":\"%s\",", isAsleep ? "true" : "false", sleepEverDetected ? "true" : "false", sleepTimeStr.c_str());
  
  n += snprintf(buf + n, sizeof(buf) - n, "\"events\":[");
  for (int i = 0; i < keyEventCount; i++) {
    char et[16], ek[128];
    jsonEscape(et, sizeof(et), keyEvents[i].timeStr.c_str());
    jsonEscape(ek, sizeof(ek), keyEvents[i].title.c_str());
    n += snprintf(buf + n, sizeof(buf) - n, "{\"t\":\"%s\",\"k\":\"%s\"}%s", et, ek, (i < keyEventCount - 1) ? "," : "");
  }

  int totalCount = graphFull ? GRAPH_POINTS : graphIndex;
  unsigned long histFromIdx = (histFrom > totalSamplesEver) ? 0 : histFrom;
  int newCount = (int)(totalSamplesEver - histFromIdx);
  if (newCount > totalCount) newCount = totalCount;
  if (histFrom == 0) newCount = totalCount;

  n += snprintf(buf + n, sizeof(buf) - n, "],\"hist\":[");
  for (int i = 0; i < newCount; i++) {
    int idx = (graphFull) ? (graphIndex - newCount + i + GRAPH_POINTS) % GRAPH_POINTS : (graphIndex - newCount + i);
    n += snprintf(buf + n, sizeof(buf) - n, "[%lu,%.0f]%s", graphTimes[idx], graphHistory[idx], (i < newCount - 1) ? "," : "");
  }
  n += snprintf(buf + n, sizeof(buf) - n, "],\"hist_total\":%lu,\"hist_new\":%d}", totalSamplesEver, newCount);
  request->send(200, "application/json", buf);
}

void handleDownload(AsyncWebServerRequest *request) {
  // Умное имя файла с временем
  char timeBuf[20];
  unsigned long t = getCurrentDaySec();
  snprintf(timeBuf, sizeof(timeBuf), "%02lu-%02lu", t/3600, (t%3600)/60);
  
  String fDate = (testDate.length() > 0) ? testDate : "Report";
  String fCar = (carModel.length() > 0) ? carModel : "Car";
  String fileName = fDate + "_" + String(timeBuf) + "_" + fCar + ".csv";
  fileName.replace(" ", "_"); fileName.replace(":", "_"); // Sanitize

  if (!LittleFS.exists("/raw.csv")) { request->send(404, "text/plain", "Файл не найден"); return; }

  String hdr = "ОТЧЕТ ДИАГНОСТИКИ ТОКА УТЕЧКИ\r\nДата: " + fDate + "\r\nАвто: " + fCar + "\r\n\r\n";
  File file = LittleFS.open("/raw.csv", "r");
  size_t totalSize = hdr.length() + 3 + file.size();
  
  AsyncWebServerResponse *response = request->beginResponse("text/csv", totalSize, [file, hdr, totalSize](uint8_t *buffer, size_t maxLen, size_t index) mutable {
    size_t written = 0;
    if (index == 0 && maxLen >= 3) { buffer[0] = 0xEF; buffer[1] = 0xBB; buffer[2] = 0xBF; written = 3; index += 3; }
    size_t hdrLen = hdr.length();
    if (index < hdrLen + 3) {
      size_t toWrite = min(maxLen - written, (hdrLen + 3) - index);
      memcpy(buffer + written, hdr.c_str() + (index - 3), toWrite);
      written += toWrite; index += toWrite;
    }
    if (index >= hdrLen + 3 && written < maxLen) {
      file.seek(index - (hdrLen + 3));
      size_t toWrite = min(maxLen - written, file.size() - (index - (hdrLen + 3)));
      written += file.read(buffer + written, toWrite);
    }
    return written;
  });
  response->addHeader("Content-Disposition", "attachment; filename=\"" + fileName + "\"");
  request->send(response);
}

void handleRoot(AsyncWebServerRequest *request) {
  request->send_P(200, "text/html", HTML_PAGE);
}

// Проверка обновлений GitHub
void handleCheckUpdate(AsyncWebServerRequest *request) {
  if (WiFi.status() != WL_CONNECTED) {
    request->send(200, "text/plain", "Ошибка: Нет подключения к интернету (подключите Wi-Fi в настройках).");
    return;
  }
  
  HTTPClient http;
  String url = "https://api.github.com/repos/" + String(GITHUB_REPO) + "/releases/latest";
  http.begin(url);
  http.addHeader("User-Agent", "ESP32-CarTester");
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      String latestVersion = doc["tag_name"].as<String>();
      if (latestVersion != FW_VERSION) {
        request->send(200, "text/plain", "Доступна новая версия: " + latestVersion + "! Скачайте вручную через /update");
      } else {
        request->send(200, "text/plain", "У вас последняя версия (" + String(FW_VERSION) + ").");
      }
    } else {
      request->send(200, "text/plain", "Ошибка чтения ответа GitHub.");
    }
  } else {
    request->send(200, "text/plain", "Ошибка соединения с GitHub (код: " + String(httpCode) + ").");
  }
  http.end();
}

// ===================== ОСНОВНОЙ ЦИКЛ =====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== BOOT v1.9 ===");

  pixels.begin();
  setLedColor(50, 50, 50);

  prefs.begin("tester", true);
  batteryCapacityAh  = prefs.getFloat("cap", 60.0);
  leakageLimitMa     = prefs.getFloat("limit", 70.0);
  VOLTAGE_CAL_FACTOR = prefs.getFloat("vol_cal", 1.00);
  CURRENT_CAL_FACTOR = prefs.getFloat("cur_cal", 1.00);
  CURRENT_OFFSET_A   = prefs.getFloat("cur_off", 0.0000);
  carModel           = prefs.getString("model", "");
  carPlate           = prefs.getString("plate", "");
  testDate           = prefs.getString("date", "");
  prefs.end();

  if (!LittleFS.begin(true)) Serial.println("Ошибка LittleFS!");
  if (!LittleFS.exists("/raw.csv")) createRawDataFile();

  Wire.begin(PIN_SDA, PIN_SCL);
  if (!ina226.init()) {
    Serial.println("Датчик INA226 не найден!");
    setLedColor(150, 0, 150);
    while (1) { delay(100); }
  }
  ina226.setResistorRange(SHUNT_OHMS, MAX_AMPS);
  ina226.setAverage(INA226_AVERAGE_16);

  setupWiFi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/setcal", HTTP_GET, handleSetCalibration);
  server.on("/setwifi", HTTP_GET, handleSetWifi);
  server.on("/start", HTTP_GET, handleStart);
  server.on("/finish", HTTP_GET, handleFinish);
  server.on("/clear", HTTP_GET, handleClear);
  server.on("/fullreset", HTTP_GET, handleFullReset);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/checkupdate", HTTP_GET, handleCheckUpdate);

  ElegantOTA.begin(&server); // Ручное OTA
  server.begin();

  Serial.println("Сервер запущен.");
}

void loop() {
  ElegantOTA.loop();

  // 1. Опрос датчика и батареи (раз в 500 мс)
  if (millis() - lastSensorRead >= SENSOR_READ_MS) {
    lastSensorRead = millis();
    
    cachedVoltage = ina226.getBusVoltage_V() * VOLTAGE_CAL_FACTOR;
    float rawCurrent = ina226.getCurrent_A() - CURRENT_OFFSET_A;
    if (rawCurrent < 0) rawCurrent = 0;
    cachedCurrentA = rawCurrent * CURRENT_CAL_FACTOR;
    cachedCurrentmA = cachedCurrentA * 1000.0;

    // Чтение батареи прибора (делитель 100к/100к на GPIO4)
    int rawAdc = analogRead(PIN_BAT_ADC);
    float batVoltage = (rawAdc / 4095.0) * 3.3 * 2.0; // Коэффициент 2.0 для делителя
    batteryPercent = constrain(map(batVoltage * 100, 330, 420, 0, 100), 0, 100); // От 3.3В до 4.2В
    
    // Защита от долгого тока
    if (cachedCurrentA > 5.0) {
      if (highCurrentStart == 0) highCurrentStart = millis();
      if (millis() - highCurrentStart > 60000) highCurrentWarning = true;
    } else {
      highCurrentStart = 0;
      highCurrentWarning = false;
    }
  }

  // 2. Индикация RGB
  if (highCurrentWarning) {
    if (millis() - lastLedBlink > 250) {
      lastLedBlink = millis();
      ledState = !ledState;
      setLedColor(ledState ? 150 : 0, 0, 0); // Мигаем красным
    }
  } else {
    if (cachedCurrentA >= 1.0) setLedColor(0, 0, 150);
    else if (cachedCurrentmA > leakageLimitMa) setLedColor(150, 0, 0);
    else setLedColor(0, 150, 0);
  }

  // 3. Логика записи (раз в 1000 мс)
  if (millis() - lastSampleTime >= 1000) {
    lastSampleTime = millis();
    unsigned long nowSec = getCurrentDaySec();
    String timeNowStr = formatSecToHMS(nowSec);

    if (sessionState != STATE_FINISHED) {
      graphHistory[graphIndex] = cachedCurrentmA;
      graphTimes[graphIndex] = nowSec;
      graphIndex++;
      totalSamplesEver++;
      if (graphIndex >= GRAPH_POINTS) { graphIndex = 0; graphFull = true; }
    }

    if (sessionState == STATE_RECORDING) {
      String eventTag;
      if (statSamples == 0) eventTag = "СТАРТ";
      if (cachedCurrentmA < statMinMa) statMinMa = cachedCurrentmA;
      if (cachedCurrentmA > statMaxMa) statMaxMa = cachedCurrentmA;
      statSumMa += cachedCurrentmA;
      statSamples++;

      // Логика сна
      sleepBuf[sleepBufIdx] = cachedCurrentmA;
      sleepBufIdx = (sleepBufIdx + 1) % SLEEP_WINDOW;
      if (sleepBufCount < SLEEP_WINDOW) sleepBufCount++;

      if (sleepBufCount == SLEEP_WINDOW) {
        float wMin = 999999.0, wMax = 0.0, wSum = 0.0;
        for (int i = 0; i < SLEEP_WINDOW; i++) {
          if (sleepBuf[i] < wMin) wMin = sleepBuf[i];
          if (sleepBuf[i] > wMax) wMax = sleepBuf[i];
          wSum += sleepBuf[i];
        }
        float wAvg = wSum / SLEEP_WINDOW;

        if (!isAsleep && wAvg < 150.0 && (wMax - wMin) <= 6.0) {
          isAsleep = true;
          if (!sleepEverDetected) {
            sleepEverDetected = true;
            sleepTimeStr = timeNowStr;
            unsigned long dur = (nowSec >= sessionStartSec) ? (nowSec - sessionStartSec) : (86400 - sessionStartSec + nowSec);
            sleepDurationStr = String(dur / 60) + " мин. " + String(dur % 60) + " сек.";
            sleepQuiescentMa = wAvg;
            addKeyEvent(timeNowStr, "БЛОКИ УСНУЛИ", "покой " + String(wAvg, 0) + " мА");
            eventTag = "БЛОКИ УСНУЛИ";
          }
        } else if (isAsleep && cachedCurrentmA > (sleepQuiescentMa + 35.0)) {
          isAsleep = false;
          addKeyEvent(timeNowStr, "ПРОБУЖДЕНИЕ!", "скачок до " + String(cachedCurrentmA, 0) + " мА");
          eventTag = "ПРОБУЖДЕНИЕ!";
        }
      }

      unsigned long targetInterval = isAsleep ? SLEEP_LOG_MS : ACTIVE_LOG_MS;
      bool forceWrite = (eventTag.length() > 0);

      if (forceWrite || (millis() - lastCsvWriteTime >= targetInterval)) {
        lastCsvWriteTime = millis();
        if (logFileOpen) {
          logFile.printf("%s;%.0f;%.2f;%s\n", timeNowStr.c_str(), cachedCurrentmA, cachedVoltage, eventTag.c_str());
          logFile.flush();
        }
      }
    }
  }
  
  delay(10);
}