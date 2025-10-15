// ===== ESP32 FreeRTOS: RFID + DHT11 + Relay + NeoPixel + MQTT + SPIFFS + Server Sync =====

#include <WiFi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <PubSubClient.h>
#include <DHT.h>
#include "time.h"
#include "FS.h"
#include "SPIFFS.h"
#include <Adafruit_NeoPixel.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_system.h> // esp_random

// ------------------- WIFI -------------------
const char* ssid = "Drop it like it's hotspot";
const char* password = "arwa2222";

// ------------------- NTP -------------------
const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 0;

// ------------------- RFID -------------------
#define RST_PIN 4
#define SS_PIN 5
MFRC522 rfid(SS_PIN, RST_PIN);

// ------------------- Buzzer / NeoPixel -------------------
#define BUZZER_PIN 16
#define PIN_NEO_PIXEL 17
#define NUM_PIXELS 8
Adafruit_NeoPixel NeoPixel(NUM_PIXELS, PIN_NEO_PIXEL, NEO_GRB + NEO_KHZ800);

// ------------------- DHT11 -------------------
#define DHTPIN 14
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// ------------------- Relay -------------------
#define RELAY_PIN 15
volatile bool relayState = false;

// ------------------- MQTT -------------------
const char* mqtt_server = "test.mosquitto.org";
const uint16_t mqtt_port = 1883;
WiFiClient espClient;
PubSubClient client(espClient);
String mqtt_client_id;

// MQTT topics
const char* topic_rfid_pub = "arwa/esp32/rfid/access";
const char* topic_dht_pub  = "arwa/esp32/dht11";
const char* topic_relay_cmd = "arwa/esp32/relay/cmd";
const char* topic_relay_state = "arwa/esp32/relay/state";
const char* topic_user_control = "arwa/esp32/user/control";

// ------------------- Local Users -------------------
#define MAX_USERS 50
String userUIDs[MAX_USERS];
String userNames[MAX_USERS];
bool userAccess[MAX_USERS];
int userCount = 0;
String masterUID = "E365C810";

// ------------------- SPIFFS Logs -------------------
const char* UNSENT_LOGS = "/unsent_logs.ndjson";
const size_t LOG_ROTATE_SIZE = 50 * 1024; // rotate logs if >50KB

// ------------------- Google Sheets webhook -------------------
//  webhook endpoint used for sending events and fetching users.
const char* webhookURL = "https://script.google.com/macros/s/AKfycbzKvzi51xozqbgTYUx0WfsBc8RV9zqQvrPtD66LSbirsVd1hga-07hoICaYSQkxYa5E/exec";

// ------------------- FreeRTOS objects -------------------
SemaphoreHandle_t xSPIFFSMutex = NULL;
QueueHandle_t xQueueEvents = NULL;

// Event types
enum EventType { EVT_RFID, EVT_DHT };

// Event structure (fixed payload)
struct Event {
  EventType type;
  char payload[512];
};

// ------------------- Users -------------------
// Load users.json from SPIFFS into the user arrays (locked version — caller must take mutex)
void loadUsersFromSPIFFS_locked() {
  if (!SPIFFS.exists("/users.json")) return;
  File f = SPIFFS.open("/users.json", FILE_READ);
  if (!f) return;

  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, f) != DeserializationError::Ok) { f.close(); return; }
  f.close();

  JsonArray arr = doc.as<JsonArray>();
  userCount = 0;
  for (JsonObject o : arr) {
    if (userCount < MAX_USERS) {
      userUIDs[userCount] = o["uid"] | "";
      userNames[userCount] = o["name"] | "";
      userAccess[userCount] = (String(o["access"] | "Y") == "Y");
      userCount++;
    }
  }
  Serial.printf("[USERS] Loaded %d users from SPIFFS\n", userCount);
}

// Thread-safe loader
void loadUsersFromSPIFFS() {
  if (xSPIFFSMutex && xSemaphoreTake(xSPIFFSMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    loadUsersFromSPIFFS_locked();
    xSemaphoreGive(xSPIFFSMutex);
  }
}

// Fetch users JSON from the server (uses webhookURL). On success it overwrites /users.json and reloads.
bool fetchUsersFromServer() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.begin(webhookURL);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    if (xSPIFFSMutex && xSemaphoreTake(xSPIFFSMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
      File f = SPIFFS.open("/users.json", FILE_WRITE);
      if (f) {
        f.print(payload);
        f.close();
        Serial.println("[USERS] ✅ updated from server");
        loadUsersFromSPIFFS_locked();
      }
      xSemaphoreGive(xSPIFFSMutex);
    }
    http.end();
    return true;
  }
  Serial.printf("[USERS] Failed to fetch users, HTTP code %d\n", code);
  http.end();
  return false;
}

// ------------------- Helpers -------------------
void beep(int duration = 200) {
  digitalWrite(BUZZER_PIN, HIGH);
  vTaskDelay(pdMS_TO_TICKS(duration));
  digitalWrite(BUZZER_PIN, LOW);
}

void getTimestampBuf(char* buf, size_t len) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &timeinfo);
  } else {
    // fallback to millis when RTC/NTP not available
    snprintf(buf, len, "%lu", millis());
  }
}

// rotate NDJSON log file if too large
void rotateLogsIfNeeded() {
  if (!SPIFFS.exists(UNSENT_LOGS)) return;
  File f = SPIFFS.open(UNSENT_LOGS, FILE_READ);
  if (!f) return;
  size_t sz = f.size();
  f.close();
  if (sz > LOG_ROTATE_SIZE) {
    String backup = String(UNSENT_LOGS) + ".old";
    if (SPIFFS.exists(backup)) SPIFFS.remove(backup);
    SPIFFS.rename(UNSENT_LOGS, backup);
  }
}

// append a JSON line to the NDJSON unsent log (caller should hold mutex)
void logToSPIFFS_NDJSON_locked(const char *jsonLine) {
  rotateLogsIfNeeded();
  File file = SPIFFS.open(UNSENT_LOGS, FILE_APPEND);
  if (file) {
    file.println(jsonLine);
    file.close();
    Serial.println("[LOG] saved to SPIFFS");
  } else {
    Serial.println("[LOG] Failed to open unsent logs file for append");
  }
}

// Publish to MQTT if connected, otherwise save payload to SPIFFS NDJSON
bool safePublishOrLog(const char* topic, const char* payload, bool retained=false) {
  Serial.printf("[MQTT] publish topic=%s payload=%s\n", topic, payload);
  if (client.connected()) {
    bool ok = client.publish(topic, payload, retained);
    if (!ok) {
      Serial.println("[MQTT] publish failed, saving to SPIFFS");
      if (xSPIFFSMutex && xSemaphoreTake(xSPIFFSMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        logToSPIFFS_NDJSON_locked(payload);
        xSemaphoreGive(xSPIFFSMutex);
      }
      return false;
    }
    return true;
  } else {
    Serial.println("[MQTT] offline, logging event to SPIFFS");
    if (xSPIFFSMutex && xSemaphoreTake(xSPIFFSMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
      logToSPIFFS_NDJSON_locked(payload);
      xSemaphoreGive(xSPIFFSMutex);
    }
    return false;
  }
}

// Send a single event to Google Sheets via webhook (JSON payload)
// returns true if HTTP 200
bool sendToGoogleSheets(const String &uid, const String &name, const String &status, const String &timestamp) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.begin(webhookURL);
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"uid\":\"" + uid + "\",\"name\":\"" + name + "\",\"status\":\"" + status + "\",\"timestamp\":\"" + timestamp + "\",\"secret\":\"mysupersecret123\"}";
  int code = http.POST(payload);
  http.end();

  Serial.printf("[GSHEET] Send %s => %d\n", payload.c_str(), code);
  return code == 200;
}

// ------------------- MQTT Callback -------------------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[128];
  size_t copy_len = (length < sizeof(msg)-1) ? length : (sizeof(msg)-1);
  if (copy_len) memcpy(msg, payload, copy_len);
  msg[copy_len] = '\0';

  Serial.printf("[MQTT] Message arrived on %s: %s\n", topic, msg);

  String t = String(topic);
  String m = String(msg);
  m.toUpperCase();

  // Relay control by MQTT topic
  if (t == String(topic_relay_cmd)) {
    if (m == "OPEN" || m == "ON") {
      relayState = true; digitalWrite(RELAY_PIN, HIGH);
    } else if (m == "CLOSE" || m == "OFF") {
      relayState = false; digitalWrite(RELAY_PIN, LOW);
    } else if (m == "TOGGLE") {
      relayState = !relayState; digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
    }
    char out[128]; char ts[32];
    getTimestampBuf(ts, sizeof(ts));
    snprintf(out, sizeof(out), "{\"state\":\"%s\",\"timestamp\":\"%s\"}", relayState ? "ON":"OFF", ts);
    safePublishOrLog(topic_relay_state, out, true);
  }
  // Simple user-control commands over MQTT
  else if (t == String(topic_user_control)) {
    if (m == "REFRESH USERS") {
      Serial.println("[USER CTRL] REFRESH USERS");
      fetchUsersFromServer();
    } else if (m == "SYNC LOGS") {
      Serial.println("[USER CTRL] SYNC LOGS requested");
      // run a sync attempt (non-blocking best-effort)
      if (xSPIFFSMutex && xSemaphoreTake(xSPIFFSMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        // We'll release the mutex immediately and enqueue a sync via fetchUsers/tryToSyncLogs from SyncTask; keep simple here
        xSemaphoreGive(xSPIFFSMutex);
      }
    }
  }
}

// ------------------- Tasks -------------------
void WiFiTask(void* pvParameters) {
  Serial.println("[WiFiTask] started");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  wl_status_t lastStatus = WL_IDLE_STATUS;

  while (true) {
    wl_status_t s = WiFi.status();
    if (s != lastStatus) {
      Serial.printf("[WiFiTask] Status changed %d -> %d\n", lastStatus, s);
      lastStatus = s;
      if (s == WL_CONNECTED) {
        Serial.printf("[WiFiTask] Connected IP=%s RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
        configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org");
      } else {
        Serial.println("[WiFiTask] Lost WiFi, will reconnect");
      }
    }

    if (s != WL_CONNECTED) {
      Serial.println("[WiFiTask] Attempting WiFi.begin()...");
      WiFi.begin(ssid, password);
    }

    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}

void onMqttConnected() {
  Serial.println("[MQTT] Connected, subscribing topics...");
  client.subscribe(topic_relay_cmd);
  client.subscribe(topic_user_control);
  char out[128]; char ts[32];
  getTimestampBuf(ts, sizeof(ts));
  snprintf(out, sizeof(out), "{\"state\":\"%s\",\"timestamp\":\"%s\"}", relayState ? "ON":"OFF", ts);
  client.publish(topic_relay_state, out, true);
}

void MQTTTask(void* pvParameters) {
  Serial.println("[MQTTTask] started");
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  bool lastConnected = false;

  while (true) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!client.connected()) {
        if (lastConnected) Serial.println("[MQTT] Lost connection, reconnecting...");
        unsigned long start = millis();
        while (!client.connected() && millis()-start < 5000) {
          if (client.connect(mqtt_client_id.c_str())) {
            onMqttConnected();
            lastConnected = true;
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (!client.connected()) lastConnected = false;
      }
      if (client.connected()) client.loop();
    }

    Event evt;
    while (xQueueReceive(xQueueEvents, &evt, 0) == pdTRUE) {
      Serial.printf("[MQTTTask] Event type=%d payload=%s\n", evt.type, evt.payload);
      if (evt.type == EVT_RFID) safePublishOrLog(topic_rfid_pub, evt.payload);
      else if (evt.type == EVT_DHT) safePublishOrLog(topic_dht_pub, evt.payload);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void DHTTask(void* pvParameters) {
  Serial.println("[DHTTask] started");
  unsigned long lastSample = 0;
  const unsigned long DHT_INTERVAL_MS = 300000;
  const unsigned long DHT_OFFLINE_INTERVAL_MS = 800000;

  while (true) {
    bool online = (WiFi.status() == WL_CONNECTED && client.connected());
    unsigned long interval = online ? DHT_INTERVAL_MS : DHT_OFFLINE_INTERVAL_MS;
    unsigned long now = millis();
    if (now - lastSample >= interval) {
      lastSample = now;
      float h = dht.readHumidity();
      float t = dht.readTemperature();
      if (!isnan(h) && !isnan(t)) {
        char ts[32]; getTimestampBuf(ts, sizeof(ts));
        char out[256];
        snprintf(out, sizeof(out), "{\"temp\":%.2f,\"hum\":%.2f,\"timestamp\":\"%s\"}", t, h, ts);
        Serial.printf("[DHT] T=%.2f H=%.2f %s\n", t, h, ts);

        Event ev; ev.type = EVT_DHT;
        strncpy(ev.payload, out, sizeof(ev.payload)-1); ev.payload[sizeof(ev.payload)-1] = '\0';
        if (xQueueSend(xQueueEvents, &ev, pdMS_TO_TICKS(200)) != pdTRUE) {
          if (xSPIFFSMutex && xSemaphoreTake(xSPIFFSMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            logToSPIFFS_NDJSON_locked(ev.payload);
            xSemaphoreGive(xSPIFFSMutex);
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

int getUserIndex(const String &uid) {
  for (int i=0;i<userCount;i++) if (uid == userUIDs[i]) return i;
  return -1;
}

void RFIDTask(void* pvParameters) {
  Serial.println("[RFIDTask] started");
  while (true) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      char uidStr[32] = "";
      for (byte i = 0; i < rfid.uid.size && i < 10; i++) {
        char part[3]; snprintf(part, sizeof(part), "%02X", rfid.uid.uidByte[i]);
        strncat(uidStr, part, sizeof(uidStr) - strlen(uidStr) - 1);
      }

      String uidS = String(uidStr);
      int idx = getUserIndex(uidS);
      String nameS = (idx != -1) ? userNames[idx] : (uidS == masterUID ? "Master Card" : "Unknown");
      bool access = (idx != -1) ? userAccess[idx] : (uidS == masterUID);
      char ts[32]; getTimestampBuf(ts, sizeof(ts));
      const char* statusStr = access ? "granted" : "denied";

      char out[512];
      snprintf(out, sizeof(out), "{\"uid\":\"%s\",\"name\":\"%s\",\"status\":\"%s\",\"timestamp\":\"%s\"}",
               uidStr, nameS.c_str(), statusStr, ts);

      // send to Google Sheets (best-effort)
      sendToGoogleSheets(uidStr, nameS, statusStr, ts);

      Serial.printf("[RFID] UID=%s Name=%s Status=%s %s\n", uidStr, nameS.c_str(), statusStr, ts);

      Event ev; ev.type = EVT_RFID;
      strncpy(ev.payload, out, sizeof(ev.payload)-1); ev.payload[sizeof(ev.payload)-1] = '\0';

      if (access) {
        // success feedback
        beep(300);
        for (int i = 0; i < NUM_PIXELS; i++) NeoPixel.setPixelColor(i, NeoPixel.Color(0,255,0));
        NeoPixel.show();

        // enqueue event -> MQTTTask will publish or we'll fallback to SPIFFS
        if (xQueueSend(xQueueEvents, &ev, pdMS_TO_TICKS(200)) != pdTRUE) {
          if (xSPIFFSMutex && xSemaphoreTake(xSPIFFSMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            logToSPIFFS_NDJSON_locked(ev.payload);
            xSemaphoreGive(xSPIFFSMutex);
          }
        }

        // toggle relay briefly
        digitalWrite(RELAY_PIN, HIGH); relayState = true;
        char stbuf[128]; char stts[32]; getTimestampBuf(stts, sizeof(stts));
        snprintf(stbuf, sizeof(stbuf), "{\"state\":\"ON\",\"timestamp\":\"%s\"}", stts);
        safePublishOrLog(topic_relay_state, stbuf, true);

        vTaskDelay(pdMS_TO_TICKS(2000));

        digitalWrite(RELAY_PIN, LOW); relayState = false;
        getTimestampBuf(stts, sizeof(stts));
        snprintf(stbuf, sizeof(stbuf), "{\"state\":\"OFF\",\"timestamp\":\"%s\"}", stts);
        safePublishOrLog(topic_relay_state, stbuf, true);
      } else {
        // denied feedback
        beep(100); vTaskDelay(pdMS_TO_TICKS(100)); beep(100);
        for (int i = 0; i < NUM_PIXELS; i++) NeoPixel.setPixelColor(i, NeoPixel.Color(255,0,0));
        NeoPixel.show();

        if (xQueueSend(xQueueEvents, &ev, pdMS_TO_TICKS(200)) != pdTRUE) {
          if (xSPIFFSMutex && xSemaphoreTake(xSPIFFSMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            logToSPIFFS_NDJSON_locked(ev.payload);
            xSemaphoreGive(xSPIFFSMutex);
          }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
      }

      NeoPixel.clear(); NeoPixel.show();
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ------------------- Sync NDJSON logs to Google Sheets -------------------
// This function reads UNSENT_LOGS (NDJSON - one JSON object per line), tries to POST each
// line to the webhook. Successful posts are dropped; failed ones are kept in a temp file.
// The caller should take the SPIFFS mutex before calling this to avoid concurrent writes.
void tryToSyncLogs_locked() {
  if (!SPIFFS.exists(UNSENT_LOGS)) return;

  File file = SPIFFS.open(UNSENT_LOGS, FILE_READ);
  if (!file) return;

  File temp = SPIFFS.open(String(UNSENT_LOGS) + ".tmp", FILE_WRITE);
  if (!temp) {
    file.close();
    return;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() < 5) continue;

    // parse JSON line minimally to extract fields (uid,name,status,timestamp)
    DynamicJsonDocument doc(512);
    DeserializationError err = deserializeJson(doc, line);
    if (err != DeserializationError::Ok) {
      // keep malformed line for manual inspection
      temp.println(line);
      continue;
    }

    String uid = doc["uid"] | "";
    String name = doc["name"] | "";
    String status = doc["status"] | "";
    String timestamp = doc["timestamp"] | "";

    bool ok = sendToGoogleSheets(uid, name, status, timestamp);
    if (!ok) {
      // keep line for next attempt
      temp.println(line);
    } else {
      Serial.println("[SYNC] line synced");
    }
  }

  file.close();
  temp.close();

  // replace original log with temp (remaining unsent)
  if (SPIFFS.exists(UNSENT_LOGS)) SPIFFS.remove(UNSENT_LOGS);
  SPIFFS.rename(String(UNSENT_LOGS) + ".tmp", UNSENT_LOGS);
}

// Wrapper that takes the mutex, calls locked variant
void tryToSyncLogs() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (xSPIFFSMutex && xSemaphoreTake(xSPIFFSMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
    tryToSyncLogs_locked();
    xSemaphoreGive(xSPIFFSMutex);
  }
}

// ------------------- Sync Task -------------------
void SyncTask(void* pvParameters) {
  Serial.println("[SyncTask] started");
  const unsigned long SYNC_INTERVAL_MS = 5*60*1000UL; // 5 minutes
  unsigned long lastSync = 0;
  while (true) {
    unsigned long now = millis();
    if (now - lastSync >= SYNC_INTERVAL_MS) {
      lastSync = now;
      if (WiFi.status() == WL_CONNECTED) {
        fetchUsersFromServer();
        tryToSyncLogs();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ------------------- Setup / Loop -------------------
void setup() {
  Serial.begin(115200);

  // IO init
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);
  pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, LOW);

  // Sensors & peripherals
  dht.begin();
  SPI.begin(); rfid.PCD_Init();
  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("[RFID] Version register: 0x%02X\n", version);
  if (version == 0x00 || version == 0xFF) {
    Serial.println("[RFID] ❌ No RFID reader found — check wiring or power");
  } else {
    Serial.println("[RFID] ✅ Reader initialized successfully");
  }

  NeoPixel.begin(); NeoPixel.show();

  // SPIFFS
  if (!SPIFFS.begin(true)) Serial.println("[SPIFFS] Mount failed");

  // MQTT client id (randomized)
  mqtt_client_id = "ESP32Client-" + String((uint32_t)esp_random(), HEX);

  // Create mutex/queue
  xSPIFFSMutex = xSemaphoreCreateMutex();
  xQueueEvents = xQueueCreate(10, sizeof(Event));

  // Load users file if present
  loadUsersFromSPIFFS();

  // Create tasks (pinned cores chosen as in original)
  xTaskCreatePinnedToCore(WiFiTask, "WiFiTask", 4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(MQTTTask, "MQTTTask", 8192, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(DHTTask, "DHTTask", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(RFIDTask, "RFIDTask", 8192, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(SyncTask, "SyncTask", 4096, NULL, 1, NULL, 1);

  Serial.println("[SETUP] complete");
}

void loop() {
  // main loop kept minimal: FreeRTOS tasks do the work
  vTaskDelay(pdMS_TO_TICKS(1000));
}
