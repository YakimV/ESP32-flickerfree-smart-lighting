#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <Update.h>
#include <WireGuard-ESP32.h>

// ================= HARDWARE CONFIGURATION =================
#define NUM_CHANNELS 6

// PWM pins for PSU feedback control
const int pwmPins[NUM_CHANNELS]   = { 5,  6,  7, 15, 17, 18 }; 
// Digital pins for AC mains relays
const int relayPins[NUM_CHANNELS] = { 4,  8,  9, 10, 11, 12 }; 

const int frequency  = 2000;   // 2 kHz
const int resolution = 10;     // 10-bit (0-1023)

// Shared state between cores (protected by stateMutex)
float currentPwmPct[NUM_CHANNELS] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
bool updateRequired[NUM_CHANNELS] = { true, true, true, true, true, true };
uint8_t stateIndex[NUM_CHANNELS]  = { 0, 0, 0, 0, 0, 0 };
float pwmPresets[4]               = { 25.0f, 50.0f, 75.0f, 100.0f };

// Relay turn-off delay (Core 1)
const uint32_t RELAY_OFF_DELAY_MS = 10000; 
uint32_t relayOffTimestamp[NUM_CHANNELS] = { 0, 0, 0, 0, 0, 0 };
bool relayPendingOff[NUM_CHANNELS]       = { false, false, false, false, false, false };

// ================= RF 433 MHz CONFIGURATION =================
#define RF_RX_PIN 16

const uint32_t RF_SWITCHES[NUM_CHANNELS] = {
  0x111111, // Switch 1 -> Ch 1
  0x222222, // Switch 2 -> Ch 2
  0x333333, // Switch 3 -> Ch 3
  0x444444, // Switch 4 -> Ch 4
  0x555555, // Switch 5 -> Ch 5
  0x666666  // Switch 6 -> Ch 6
};

volatile uint32_t shiftRegister = 0;
volatile uint8_t bitCount = 0;
volatile uint32_t lastChangeTime = 0;
volatile uint32_t capturedCode = 0;
volatile bool newCodeReady = false;
volatile bool isSynced = false;

uint32_t candidateCode = 0;
uint32_t candidateTime = 0;
uint32_t lastConfirmedCode = 0;
uint32_t lastConfirmedTime = 0;

// ================= NETWORK & MQTT CONFIGURATION =================
const char* ssid      = "YOUR_WIFI_SSID";
const char* pass      = "YOUR_WIFI_PASSWORD";

const char* mqtt_srv  = "10.0.0.1";  
const int   mqtt_port = 1883;
const char* mqtt_user = "YOUR_MQTT_USER";  
const char* mqtt_pass = "YOUR_MQTT_PASSWORD";   

String topicStatus[NUM_CHANNELS];
String topicCommands[NUM_CHANNELS];

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// WireGuard VPN
const IPAddress wg_local_ip(10, 0, 0, 2); 
const char* wg_private_key = "CLIENT_PRIVATE_KEY_BASE64="; 
const char* wg_public_key  = "SERVER_PUBLIC_KEY_BASE64="; 
const char* wg_endpoint    = "YOUR_VPN_ENDPOINT_OR_DOMAIN"; 
const int   wg_port        = 51820;        

WireGuard wg;
bool vpnConnected = false; 

// Web OTA
WebServer server(80);
volatile bool isUpdating = false;

// ================= FREERTOS SYNCHRONIZATION =================
SemaphoreHandle_t stateMutex = NULL;
QueueHandle_t statusQueue = NULL; // Channel index queue (-1 = all channels)

// ================= RF 433 MHz ISR =================
void IRAM_ATTR rfISR() {
  uint32_t now = micros();
  uint32_t duration = now - lastChangeTime;
  lastChangeTime = now;

  if (duration < 100) return; 

  bool currentState = digitalRead(RF_RX_PIN);

  if (currentState == HIGH) {
    if (duration >= 2500 && duration <= 12000) {
      isSynced = true;
      bitCount = 0;
      shiftRegister = 0;
    } else if (duration > 12000) {
      isSynced = false;
    }
  } else {
    if (isSynced) {
      if (duration >= 150 && duration <= 1000) {
        shiftRegister <<= 1;
        if (duration > 480) {
          shiftRegister |= 1;
        }
        bitCount++;

        if (bitCount == 24) {
          capturedCode = shiftRegister;
          newCodeReady = true;
          isSynced = false;
        }
      } else {
        isSynced = false;
        bitCount = 0;
      }
    }
  }
}

// ================= STATUS PUBLISHING (CORE 0) =================
void publishStatus(int ch) {
  if (isUpdating || ch < 0 || ch >= NUM_CHANNELS) return; 

  float pwmVal = 0.0f;
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    pwmVal = currentPwmPct[ch];
    xSemaphoreGive(stateMutex);
  }

  JsonDocument d; 
  d["pwm_pct"] = round(pwmVal * 10.0f) / 10.0f;
  d["relay"]   = (digitalRead(relayPins[ch]) == HIGH);

  String out;
  serializeJson(d, out);
  
  if (mqttClient.publish(topicStatus[ch].c_str(), out.c_str(), false)) {
    Serial.printf("📤 [MQTT OUT] Status Led%d -> %s\n", ch + 1, out.c_str());
  } else {
    Serial.printf("❌ [MQTT OUT] Publish error for Led%d!\n", ch + 1);
  }
}

void publishAllStatuses() {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    publishStatus(i);
  }
}

// ================= MQTT CALLBACK (CORE 0) =================
void callback(char* topic, byte* payload, unsigned int len) {
  if (isUpdating) return; 

  String msg = "";
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  String topicStr = String(topic);

  for (int i = 0; i < NUM_CHANNELS; i++) {
    if (topicStr == topicCommands[i]) {
      JsonDocument d;
      DeserializationError err = deserializeJson(d, msg);

      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (!err) {
          if (!d["set_pwm_1"].isNull()) pwmPresets[0] = d["set_pwm_1"].as<float>();
          if (!d["set_pwm_2"].isNull()) pwmPresets[1] = d["set_pwm_2"].as<float>();
          if (!d["set_pwm_3"].isNull()) pwmPresets[2] = d["set_pwm_3"].as<float>();
          if (!d["set_pwm_4"].isNull()) pwmPresets[3] = d["set_pwm_4"].as<float>();

          if (!d["pwm_pct"].isNull()) {
            float pct = d["pwm_pct"].as<float>();
            if (pct >= 0.0f && pct <= 100.0f) {
              currentPwmPct[i] = pct;
              updateRequired[i] = true;
            }
          }
        } else {
          float pct = msg.toFloat();
          if (pct >= 0.0f && pct <= 100.0f) {
            currentPwmPct[i] = pct;
            updateRequired[i] = true;
          }
        }
        xSemaphoreGive(stateMutex);

        int ch = i;
        xQueueSend(statusQueue, &ch, 0);
      }
      break;
    }
  }
}

// ================= WEB OTA SERVER =================
const char* serverIndex = R"html(
<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>
<style>
body{font-family:sans-serif;background:#f0f2f5;display:flex;justify-content:center;align-items:center;height:100vh;margin:0;}
.card{background:white;padding:30px 20px;border-radius:16px;box-shadow:0 4px 12px rgba(0,0,0,0.1);max-width:380px;width:100%;text-align:center;}
input[type=file]{width:100%;padding:12px;margin-bottom:20px;border:1px solid #ccc;border-radius:8px;box-sizing:border-box;}
input[type=submit]{width:100%;background:#28a745;color:white;border:none;padding:15px;border-radius:8px;font-size:16px;font-weight:bold;cursor:pointer;}
</style></head><body><div class='card'><h2>OTA Firmware Update</h2>
<form method='POST' action='/update' enctype='multipart/form-data'>
<input type='file' name='update' required accept='.bin'><input type='submit' value='Update'>
</form></div></body></html>)html";

void setupWebOTA() {
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", serverIndex);
  });

  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    String responseHtml = Update.hasError() ? "<h2>Update Failed!</h2>" : "<h2>Update Successful! Rebooting...</h2>";
    server.send(200, "text/html", responseHtml);
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      isUpdating = true;
      Serial.println("\n📥 [OTA] Receiving firmware binary...");
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) Serial.println("✅ [OTA] Firmware successfully written!");
      else Update.printError(Serial);
      isUpdating = false;
    }
  });
  server.begin();
  Serial.println("🌐 [CORE 0] Web OTA running on port 80.");
}

// ================= NETWORK TASK (CORE 0) =================
void networkTask(void* pvParameters) {
  Serial.println("🌐 [CORE 0] Network task started.");
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  setupWebOTA();
  mqttClient.setServer(mqtt_srv, mqtt_port);
  mqttClient.setCallback(callback);
  mqttClient.setBufferSize(1024);

  uint32_t lastMqttReconnectAttempt = 0;
  uint32_t lastPeriodicStatus = 0;
  bool timeSynchronized = false;

  for (;;) {
    server.handleClient();

    if (isUpdating) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    uint32_t now = millis();

    // 1. Wi-Fi connection check
    if (WiFi.status() != WL_CONNECTED) {
      vpnConnected = false;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // 2. NTP time synchronization for WireGuard
    if (!timeSynchronized) {
      configTime(2 * 3600, 0, "pool.ntp.org", "time.nist.gov");
      if (time(nullptr) > 1000000000l) {
        timeSynchronized = true;
        Serial.printf("✅ [CORE 0] NTP synchronized: %ld\n", time(nullptr));
      } else {
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }
    }

    // 3. WireGuard tunnel initialization
    if (!vpnConnected) {
      Serial.println("🔒 [CORE 0] Initializing WireGuard...");
      if (wg.begin(wg_local_ip, wg_private_key, wg_endpoint, wg_public_key, wg_port)) {
        Serial.println("✅ [CORE 0] WireGuard tunnel established.");
        vpnConnected = true;
      } else {
        Serial.println("❌ [CORE 0] Failed to start WireGuard. Retrying in 3s...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        continue;
      }
    }

    // 4. Non-blocking MQTT reconnect loop
    if (!mqttClient.connected()) {
      if (now - lastMqttReconnectAttempt > 3000) {
        lastMqttReconnectAttempt = now;
        String clientId = "ESP32-LED-" + String((uint32_t)ESP.getEfuseMac(), HEX);
        
        Serial.printf("🔄 [CORE 0] Connecting to MQTT (%s)...\n", mqtt_srv);
        if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass, topicStatus[0].c_str(), 0, false, "{\"state\":\"offline\"}")) {
          Serial.println("🚀 [CORE 0] MQTT connected.");
          for (int i = 0; i < NUM_CHANNELS; i++) {
            mqttClient.subscribe(topicCommands[i].c_str());
          }
          int all = -1;
          xQueueSend(statusQueue, &all, 0);
        }
      }
    } else {
      mqttClient.loop();
    }

    // 5. Process outbound MQTT queue
    int chToPublish;
    while (xQueueReceive(statusQueue, &chToPublish, 0) == pdTRUE) {
      if (mqttClient.connected()) {
        if (chToPublish == -1) {
          publishAllStatuses();
        } else {
          publishStatus(chToPublish);
        }
      }
    }

    // 6. Periodic status heartbeat (10s)
    if (now - lastPeriodicStatus >= 10000) {
      lastPeriodicStatus = now;
      if (mqttClient.connected()) {
        publishAllStatuses();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ================= SETUP (CORE 1) =================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== SYSTEM BOOT: DUAL-CORE RTOS FLICKER-FREE CONTROLLER ===");

  stateMutex = xSemaphoreCreateMutex();
  statusQueue = xQueueCreate(16, sizeof(int));

  for (int i = 0; i < NUM_CHANNELS; i++) {
    topicStatus[i]   = "smartHome/user1/Led" + String(i + 1) + "/status";
    topicCommands[i] = "smartHome/user1/Led" + String(i + 1) + "/commands";

    ledcSetup(i, frequency, resolution);
    ledcAttachPin(pwmPins[i], i);

    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
  }

  pinMode(RF_RX_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(RF_RX_PIN), rfISR, CHANGE);

  xTaskCreatePinnedToCore(
    networkTask,
    "NetworkTask",
    8192,
    NULL,
    1,
    NULL,
    0
  );

  Serial.println("⚡ [CORE 1] Hardware loop active.");
}

// ================= RF SWITCH HANDLER (CORE 1) =================
void processRfSwitchClick(uint32_t code) {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    if (code == RF_SWITCHES[i]) {
      if (xSemaphoreTake(stateMutex, portMAX_DELAY) == pdTRUE) {
        stateIndex[i] = (stateIndex[i] + 1) % 5;
        currentPwmPct[i] = (stateIndex[i] == 0) ? 0.0f : pwmPresets[stateIndex[i] - 1];
        updateRequired[i] = true;
        xSemaphoreGive(stateMutex);

        Serial.printf("🔘 [RF CLICK] Ch %d -> State: %d | Duty: %.1f%%\n", 
                      i + 1, stateIndex[i], currentPwmPct[i]);

        int ch = i;
        xQueueSend(statusQueue, &ch, 0);
      }
      return;
    }
  }
}

// ================= MAIN LOOP (CORE 1 - HARDWARE ONLY) =================
void loop() {
  uint32_t now = millis();

  // 1. Process decoded RF signals
  if (newCodeReady) {
    uint32_t code = capturedCode;
    newCodeReady = false;

    // 2-match filter debounce
    if (code == candidateCode && (now - candidateTime < 350)) {
      if (code != lastConfirmedCode || (now - lastConfirmedTime > 400)) {
        processRfSwitchClick(code);
        lastConfirmedCode = code;
        lastConfirmedTime = now;
      }
    } else {
      candidateCode = code;
      candidateTime = now;
    }
  }

  // 2. Hardware PWM and relay control logic
  for (int i = 0; i < NUM_CHANNELS; i++) {
    bool needsUpdate = false;
    float targetPct = 0.0f;

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      if (updateRequired[i]) {
        needsUpdate = true;
        updateRequired[i] = false;
        targetPct = currentPwmPct[i];
      }
      xSemaphoreGive(stateMutex);
    }

    if (needsUpdate) {
      int targetDuty = (int)((targetPct / 100.0f) * 1023.0f);
      targetDuty = constrain(targetDuty, 0, 1023);
      ledcWrite(i, targetDuty);

      if (targetPct > 0.0f) {
        digitalWrite(relayPins[i], HIGH);
        relayPendingOff[i] = false;
      } else {
        if (digitalRead(relayPins[i]) == HIGH && !relayPendingOff[i]) {
          relayPendingOff[i] = true;
          relayOffTimestamp[i] = now;
          Serial.printf("⏳ [CH%d] Duty 0%%. Starting 10s relay delay...\n", i + 1);
        }
      }
    }

    // 3. Standby relay turn-off check
    if (relayPendingOff[i] && (now - relayOffTimestamp[i] >= RELAY_OFF_DELAY_MS)) {
      relayPendingOff[i] = false;
      digitalWrite(relayPins[i], LOW);
      Serial.printf("🔌 [CH%d] Relay opened (0W Standby).\n", i + 1);

      int ch = i;
      xQueueSend(statusQueue, &ch, 0);
    }
  }

  delay(2);
}
