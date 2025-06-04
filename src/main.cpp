// ==========================================================================
// == ESP32 温湿度气体监测器 ==
// ==========================================================================

// == 核心库 ==
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h> // Using esphome/ESPAsyncWebServer-esphome
#include <WebSocketsServer.h>  // links2004/WebSockets
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <Adafruit_NeoPixel.h>
#include <DHT.h>

// == 自定义配置文件 ==
#include "config.h"

// ==========================================================================
// == 全局对象和变量 ==
// ==========================================================================

AsyncWebServer server(80);
WebSocketsServer webSocket(81); // WebSocket server on port 81
DHT dht(DHT_PIN, DHT_TYPE);

float currentTemperature = NAN;
float currentHumidity = NAN;
int currentGasValue = -1;

Adafruit_NeoPixel pixels = Adafruit_NeoPixel(NEOPIXEL_NUM, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// RGB LED 颜色常量
uint32_t COLOR_GREEN_VAL;
uint32_t COLOR_RED_VAL;
uint32_t COLOR_BLUE_VAL;
uint32_t COLOR_YELLOW_VAL;
uint32_t COLOR_OFF_VAL;

struct AlarmThresholds {
    float tempMin, tempMax;
    float humMin, humMax;
    int gasMin, gasMax;
};
AlarmThresholds thresholds;

enum SensorStatusVal { SS_NORMAL, SS_WARNING, SS_DISCONNECTED };
SensorStatusVal tempStatus = SS_DISCONNECTED;
SensorStatusVal humStatus = SS_DISCONNECTED;
SensorStatusVal gasStatus = SS_DISCONNECTED;

// WiFi Connection State Management
enum WifiConnectProgress { WIFI_CP_IDLE, WIFI_CP_DISCONNECTING, WIFI_CP_CONNECTING, WIFI_CP_FAILED };
WifiConnectProgress wifiConnectState = WIFI_CP_IDLE;
String wifiSsidToTry = "";
String wifiPasswordToTry = "";
unsigned long wifiConnectAttemptStartTime = 0;
uint8_t wifiConnectInitiatorClientNum = 255;

// WiFi Scan State Management
bool isScanningWifi = false;
uint8_t wifiScanRequesterClientNum = 255;
unsigned long wifiScanStartTime = 0;
const unsigned long WIFI_SCAN_TIMEOUT_MS = 20000; // 20秒扫描超时

String currentSsidForSettings = ""; // Used to store SSID for saving in settings

struct SensorDataPoint {
    unsigned long timestamp;
    float temp;
    float hum;
    int gas;
    char timeStr[9];
};
std::vector<SensorDataPoint> historicalData;

unsigned long lastSensorReadTime = 0;
unsigned long lastWebSocketUpdateTime = 0;
unsigned long lastHistoricalDataSaveTime = 0;

bool buzzerShouldBeActive = false;
unsigned long buzzerStopTime = 0;
int buzzerBeepCount = 0;

bool ledBlinkState = false;
unsigned long lastBlinkTime = 0;

// ==========================================================================
// == 函数声明 ==
// ==========================================================================
void initSPIFFS();
void loadSettings();
void saveSettings();
void loadHistoricalData();
void saveHistoricalData();
void initWiFi();
void processWiFiConnection();
void processWifiScan(); // 新增：处理异步WiFi扫描
void initNeoPixelAndBuzzer();
void initSensors();
void readSensors();
void checkAlarms();
void updateLedStatus();
void controlBuzzer();
void onWebSocketEvent(uint8_t clientNum, WStype_t type, uint8_t * payload, size_t length);
void sendSensorDataToClients(uint8_t specificClientNum = 255);
void sendWifiStatusToClients(uint8_t specificClientNum = 255);
void sendHistoricalDataToClient(uint8_t clientNum);
void sendCurrentSettingsToClient(uint8_t clientNum);
void handleWebSocketMessage(uint8_t clientNum, uint8_t * payload, size_t length);
void configureWebServer();
void resetAllSettingsToDefault();
void generateTimeStr(unsigned long current_timestamp, char* buffer);
void addHistoricalDataPoint(float temp, float hum, float gas_val);
void serveStaticFile(AsyncWebServerRequest *request, const char* path, const char* contentType);


// ==========================================================================
// == Arduino `setup()` 函数 ==
// ==========================================================================
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000);
    P_PRINTLN("\n[SETUP] 系统启动中...");

    initNeoPixelAndBuzzer();
    COLOR_GREEN_VAL  = pixels.Color(0, 120, 0);
    COLOR_RED_VAL    = pixels.Color(120, 0, 0);
    COLOR_BLUE_VAL   = pixels.Color(0, 0, 120);
    COLOR_YELLOW_VAL = pixels.Color(120, 120, 0);
    COLOR_OFF_VAL    = pixels.Color(0, 0, 0);

    pixels.setPixelColor(0, COLOR_BLUE_VAL);
    pixels.show();

    initSPIFFS();
    loadSettings();
    loadHistoricalData();
    initSensors();
    initWiFi();

    configureWebServer();
    server.begin();
    P_PRINTLN("[SETUP] HTTP服务器已启动 (端口 80)");

    webSocket.begin();
    webSocket.onEvent(onWebSocketEvent);
    P_PRINTLN("[SETUP] WebSocket服务器已启动 (端口 81)");

    P_PRINTLN("[SETUP] 初始化完成, 系统运行中.");
}

// ==========================================================================
// == Arduino `loop()` 函数 ==
// ==========================================================================
void loop() {
    webSocket.loop();
    processWiFiConnection();
    processWifiScan(); // 新增：处理异步WiFi扫描状态

    unsigned long currentTime = millis();

    if (currentTime - lastSensorReadTime >= SENSOR_READ_INTERVAL_MS) {
        lastSensorReadTime = currentTime;
        readSensors();
        checkAlarms();
        if (tempStatus != SS_DISCONNECTED && humStatus != SS_DISCONNECTED && gasStatus != SS_DISCONNECTED) {
            addHistoricalDataPoint(currentTemperature, currentHumidity, currentGasValue);
        }
    }

    updateLedStatus();
    controlBuzzer();

    if (currentTime - lastWebSocketUpdateTime >= WEBSOCKET_UPDATE_INTERVAL_MS) {
        lastWebSocketUpdateTime = currentTime;
        if (wifiConnectState == WIFI_CP_IDLE || wifiConnectState == WIFI_CP_FAILED) {
             if (!isScanningWifi) { // 仅当不扫描WiFi时发送常规数据
                sendSensorDataToClients();
                sendWifiStatusToClients();
            }
        }
    }

    if (currentTime - lastHistoricalDataSaveTime >= 300000) {
        lastHistoricalDataSaveTime = currentTime;
        saveHistoricalData();
    }
}


// ==========================================================================
// == 初始化函数实现 (与之前版本相同，此处省略以减少篇幅) ==
// ... (initSPIFFS, loadSettings, saveSettings, loadHistoricalData, saveHistoricalData)
// ... (initNeoPixelAndBuzzer, initSensors)
// ==========================================================================
void initSPIFFS() {
    if (!SPIFFS.begin(true)) {
        P_PRINTLN("[SPIFFS] 初始化失败!");
        return;
    }
    P_PRINTLN("[SPIFFS] 文件系统已挂载.");
}

void loadSettings() {
    P_PRINTLN("[SETTINGS] 正在加载设置...");
    if (SPIFFS.exists(SETTINGS_FILE)) {
        File file = SPIFFS.open(SETTINGS_FILE, "r");
        if (file && file.size() > 0) {
            DynamicJsonDocument doc(1024);
            DeserializationError error = deserializeJson(doc, file);
            if (error) {
                P_PRINTF("[SETTINGS] JSON反序列化失败: %s\n", error.c_str());
                resetAllSettingsToDefault();
            } else {
                thresholds.tempMin = doc["thresholds"]["tempMin"] | DEFAULT_TEMP_MIN;
                thresholds.tempMax = doc["thresholds"]["tempMax"] | DEFAULT_TEMP_MAX;
                thresholds.humMin  = doc["thresholds"]["humMin"]  | DEFAULT_HUM_MIN;
                thresholds.humMax  = doc["thresholds"]["humMax"]  | DEFAULT_HUM_MAX;
                thresholds.gasMin  = doc["thresholds"]["gasMin"]  | DEFAULT_GAS_MIN;
                thresholds.gasMax  = doc["thresholds"]["gasMax"]  | DEFAULT_GAS_MAX;
                currentSsidForSettings = doc["wifi"]["ssid"].as<String>();
                P_PRINTLN("[SETTINGS] 设置加载成功.");
            }
        } else {
            P_PRINTLN(file ? "[SETTINGS] 设置文件为空." : "[SETTINGS] 打开设置文件失败.");
            resetAllSettingsToDefault();
        }
        if(file) file.close();
    } else {
        P_PRINTLN("[SETTINGS] 设置文件不存在, 使用默认值并保存.");
        resetAllSettingsToDefault();
        saveSettings();
    }
    P_PRINTF("  Loaded Thresholds - Temp: %.1f-%.1f, Hum: %.1f-%.1f, Gas: %d-%d\n",
                   thresholds.tempMin, thresholds.tempMax, thresholds.humMin, thresholds.humMax, thresholds.gasMin, thresholds.gasMax);
    P_PRINTF("  Loaded WiFi SSID for auto-connect: %s\n", currentSsidForSettings.c_str());
}

void saveSettings() {
    P_PRINTLN("[SETTINGS] 正在保存设置...");
    File file = SPIFFS.open(SETTINGS_FILE, "w");
    if (file) {
        DynamicJsonDocument doc(1024);
        JsonObject thresholdsObj = doc.createNestedObject("thresholds");
        thresholdsObj["tempMin"] = thresholds.tempMin;
        thresholdsObj["tempMax"] = thresholds.tempMax;
        thresholdsObj["humMin"] = thresholds.humMin;
        thresholdsObj["humMax"] = thresholds.humMax;
        thresholdsObj["gasMin"] = thresholds.gasMin;
        thresholdsObj["gasMax"] = thresholds.gasMax;

        JsonObject wifiObj = doc.createNestedObject("wifi");
        wifiObj["ssid"] = WiFi.isConnected() ? WiFi.SSID() : currentSsidForSettings;

        if (serializeJson(doc, file) == 0) {
            P_PRINTLN("[SETTINGS] 写入设置文件失败.");
        } else {
            P_PRINTLN("[SETTINGS] 设置保存成功.");
        }
        file.close();
    } else {
        P_PRINTLN("[SETTINGS] 创建/打开设置文件用于写入失败.");
    }
}

void loadHistoricalData() {
    P_PRINTLN("[HISTORY] 正在加载历史数据...");
    historicalData.clear();
    if (SPIFFS.exists(HISTORICAL_DATA_FILE)) {
        File file = SPIFFS.open(HISTORICAL_DATA_FILE, "r");
        if (file && file.size() > 0) {
            const size_t capacity = JSON_ARRAY_SIZE(HISTORICAL_DATA_POINTS) + HISTORICAL_DATA_POINTS * JSON_OBJECT_SIZE(5);
            DynamicJsonDocument doc(min((size_t)(1024 * 12), capacity));
            DeserializationError error = deserializeJson(doc, file);
            if (error) {
                P_PRINTF("[HISTORY] JSON反序列化历史数据失败: %s\n", error.c_str());
            } else {
                JsonArray arr = doc.as<JsonArray>();
                for (JsonObject obj : arr) {
                    if (historicalData.size() >= HISTORICAL_DATA_POINTS) break;
                    SensorDataPoint dp;
                    dp.timestamp = obj["ts"]; dp.temp = obj["t"]; dp.hum = obj["h"]; dp.gas = obj["g"];
                    generateTimeStr(dp.timestamp, dp.timeStr);
                    historicalData.push_back(dp);
                }
                P_PRINTF("[HISTORY] 加载了 %u 条历史数据.\n", historicalData.size());
            }
        } else { P_PRINTLN(file ? "[HISTORY] 文件为空." : "[HISTORY] 打开文件失败."); }
        if(file) file.close();
    } else { P_PRINTLN("[HISTORY] 文件不存在."); }
    while (historicalData.size() > HISTORICAL_DATA_POINTS) { historicalData.erase(historicalData.begin()); }
}

void saveHistoricalData() {
    P_PRINTLN("[HISTORY] 正在保存历史数据...");
    File file = SPIFFS.open(HISTORICAL_DATA_FILE, "w");
    if (file) {
        const size_t capacity = JSON_ARRAY_SIZE(historicalData.size()) + historicalData.size() * JSON_OBJECT_SIZE(4);
        DynamicJsonDocument doc(min((size_t)(1024 * 12), capacity));
        JsonArray arr = doc.to<JsonArray>();
        for (const auto& dp : historicalData) {
            JsonObject obj = arr.createNestedObject();
            obj["ts"] = dp.timestamp; obj["t"] = dp.temp; obj["h"] = dp.hum; obj["g"] = dp.gas;
        }
        size_t bytesWritten = serializeJson(doc, file);
        if (bytesWritten == 0 && historicalData.size() > 0) P_PRINTLN("[HISTORY] 写入失败.");
        else P_PRINTF("[HISTORY] %u 条 (%lu B) 已保存.\n", historicalData.size(), bytesWritten);
        file.close();
    } else { P_PRINTLN("[HISTORY] 创建文件失败."); }
}

void initNeoPixelAndBuzzer() {
    pixels.begin();
    pixels.setBrightness(60);
    pixels.clear();
    pixels.show();
    P_PRINTLN("[LED] NeoPixel RGB LED已初始化.");
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    P_PRINTLN("[BUZZER] 蜂鸣器已初始化.");
}

void initSensors() {
    dht.begin();
    P_PRINTLN("[SENSORS] DHT传感器已初始化.");
    pinMode(MQ2_PIN, INPUT);
    P_PRINTLN("[SENSORS] MQ-2传感器引脚已设置为输入.");
}


void initWiFi() {
    WiFi.mode(WIFI_AP_STA);
    P_PRINTLN("[WIFI] 设置为AP+STA模式.");
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD, WIFI_AP_CHANNEL, 0, WIFI_AP_MAX_CONNECTIONS);
    IPAddress apIP = WiFi.softAPIP();
    P_PRINTF("[WIFI] AP模式已启动. SSID: %s, IP: %s\n", WIFI_AP_SSID, apIP.toString().c_str());

    if (currentSsidForSettings.length() > 0) {
        P_PRINTF("[WIFI] 检测到保存的SSID: %s, 将在后台尝试连接...\n", currentSsidForSettings.c_str());
        wifiSsidToTry = currentSsidForSettings;
        wifiPasswordToTry = "";
        wifiConnectInitiatorClientNum = 255;
        wifiConnectState = WIFI_CP_DISCONNECTING;
        wifiConnectAttemptStartTime = millis();
        WiFi.disconnect(false);
    } else {
        P_PRINTLN("[WIFI] 没有已保存的STA SSID可供自动连接.");
    }
}

void processWiFiConnection() {
    if (wifiConnectState == WIFI_CP_IDLE || wifiConnectState == WIFI_CP_FAILED) {
        return;
    }

    DynamicJsonDocument responseDoc(256);
    responseDoc["type"] = "connectWifiStatus";

    if (wifiConnectState == WIFI_CP_DISCONNECTING) {
        if (WiFi.status() == WL_DISCONNECTED || millis() - wifiConnectAttemptStartTime > 2000) {
            P_PRINTF("[WIFI_PROC] 已断开或超时. 尝试连接到 %s\n", wifiSsidToTry.c_str());
            WiFi.begin(wifiSsidToTry.c_str(), wifiPasswordToTry.c_str());
            wifiConnectState = WIFI_CP_CONNECTING;
            wifiConnectAttemptStartTime = millis();
        }
        return;
    }

    if (wifiConnectState == WIFI_CP_CONNECTING) {
        wl_status_t status = WiFi.status();
        if (status == WL_CONNECTED) {
            currentSsidForSettings = wifiSsidToTry;
            saveSettings();
            P_PRINTF("[WIFI_PROC] 连接成功: SSID=%s, IP=%s\n", wifiSsidToTry.c_str(), WiFi.localIP().toString().c_str());
            responseDoc["success"] = true;
            responseDoc["message"] = "WiFi connected successfully to " + wifiSsidToTry;
            responseDoc["ip"] = WiFi.localIP().toString();
            wifiConnectState = WIFI_CP_IDLE;
        } else if (millis() - wifiConnectAttemptStartTime > 20000) { // 20秒连接超时
            P_PRINTF("[WIFI_PROC] 连接超时: SSID=%s. WiFi Status: %d\n", wifiSsidToTry.c_str(), status);
            WiFi.disconnect(false);
            responseDoc["success"] = false;
            responseDoc["message"] = "Failed to connect to " + wifiSsidToTry + " (Timeout, Status: " + String(status) + ")";
            wifiConnectState = WIFI_CP_FAILED;
        } else if (status == WL_NO_SSID_AVAIL || status == WL_CONNECT_FAILED || status == WL_CONNECTION_LOST ) {
            P_PRINTF("[WIFI_PROC] 连接失败: SSID=%s. WiFi Status: %d\n", wifiSsidToTry.c_str(), status);
            WiFi.disconnect(false);
            responseDoc["success"] = false;
            responseDoc["message"] = "Failed to connect to " + wifiSsidToTry + " (Error, Status: " + String(status) + ")";
            wifiConnectState = WIFI_CP_FAILED;
        }
        else { return; }

        if (wifiConnectInitiatorClientNum != 255 && wifiConnectInitiatorClientNum < webSocket.connectedClients()) {
            String responseStr;
            serializeJson(responseDoc, responseStr);
            webSocket.sendTXT(wifiConnectInitiatorClientNum, responseStr);
        }
        sendWifiStatusToClients();
        wifiConnectInitiatorClientNum = 255;
    }
}

void processWifiScan() {
    if (!isScanningWifi) {
        return;
    }

    int8_t scanResult = WiFi.scanComplete();

    if (scanResult == WIFI_SCAN_RUNNING) { // -1, 扫描仍在进行中
        if (millis() - wifiScanStartTime > WIFI_SCAN_TIMEOUT_MS) {
            P_PRINTLN("[WIFI_SCAN_PROC] 扫描超时!");
            WiFi.scanDelete(); // 尝试清除可能存在的扫描数据
            isScanningWifi = false;
            DynamicJsonDocument scanTimeoutDoc(128);
            scanTimeoutDoc["type"] = "wifiScanResults";
            scanTimeoutDoc["error"] = "Scan timed out.";
            JsonArray emptyNetworks = scanTimeoutDoc.createNestedArray("networks");
            String timeoutStr;
            serializeJson(scanTimeoutDoc, timeoutStr);
            if (wifiScanRequesterClientNum != 255 && wifiScanRequesterClientNum < webSocket.connectedClients()) {
                webSocket.sendTXT(wifiScanRequesterClientNum, timeoutStr);
            }
            wifiScanRequesterClientNum = 255; // 重置请求者
        }
        return; // 继续等待
    }

    P_PRINTF("[WIFI_SCAN_PROC] 异步扫描完成. 结果: %d\n", scanResult);
    isScanningWifi = false; // 标记扫描结束

    DynamicJsonDocument doc(scanResult > 0 ? JSON_ARRAY_SIZE(scanResult) + scanResult * JSON_OBJECT_SIZE(3) + 256 : 256);
    doc["type"] = "wifiScanResults";
    JsonArray networks = doc.createNestedArray("networks");

    if (scanResult > 0) {
        for (int i = 0; i < scanResult; ++i) {
            JsonObject net = networks.createNestedObject();
            net["ssid"] = WiFi.SSID(i);
            net["rssi"] = WiFi.RSSI(i);
            switch (WiFi.encryptionType(i)) {
                case WIFI_AUTH_OPEN: net["encryption"] = "Open"; break;
                case WIFI_AUTH_WEP: net["encryption"] = "WEP"; break;
                case WIFI_AUTH_WPA_PSK: net["encryption"] = "WPA PSK"; break;
                case WIFI_AUTH_WPA2_PSK: net["encryption"] = "WPA2 PSK"; break;
                case WIFI_AUTH_WPA_WPA2_PSK: net["encryption"] = "WPA/WPA2 PSK"; break;
                default: net["encryption"] = "Protected";
            }
        }
    } else if (scanResult == WIFI_SCAN_FAILED) { // -2, 扫描失败
         doc["error"] = "Scan failed.";
         P_PRINTLN("[WIFI_SCAN_PROC] WiFi扫描失败.");
    } else { // 0 networks found
         P_PRINTLN("[WIFI_SCAN_PROC] 未发现WiFi网络.");
    }

    String responseStr;
    serializeJson(doc, responseStr);
    if (wifiScanRequesterClientNum != 255 && wifiScanRequesterClientNum < webSocket.connectedClients()) {
        webSocket.sendTXT(wifiScanRequesterClientNum, responseStr);
    } else {
        // 如果没有特定请求者 (例如系统启动时的扫描)，则不发送，或根据需要广播
        // webSocket.broadcastTXT(responseStr);
    }

    WiFi.scanDelete(); // 清除扫描结果以释放内存
    wifiScanRequesterClientNum = 255; // 重置请求者
}


// ==========================================================================
// == 传感器读取与处理实现 (与之前版本相同，此处省略) ==
// ... (readSensors, checkAlarms, updateLedStatus, controlBuzzer)
// ==========================================================================
void readSensors() {
    float newTemp = dht.readTemperature();
    float newHum = dht.readHumidity();
    if (isnan(newTemp) || isnan(newHum)) {
        tempStatus = SS_DISCONNECTED;
        humStatus = SS_DISCONNECTED;
    } else {
        currentTemperature = newTemp;
        currentHumidity = newHum;
        if(tempStatus == SS_DISCONNECTED) tempStatus = SS_NORMAL;
        if(humStatus == SS_DISCONNECTED) humStatus = SS_NORMAL;
    }
    currentGasValue = analogRead(MQ2_PIN);
    if (currentGasValue < 0 || currentGasValue > 4095) {
        gasStatus = SS_DISCONNECTED;
        currentGasValue = -1;
    } else {
        if(gasStatus == SS_DISCONNECTED) gasStatus = SS_NORMAL;
    }
}

void checkAlarms() {
    if (tempStatus != SS_DISCONNECTED) {
        if (currentTemperature < thresholds.tempMin || currentTemperature > thresholds.tempMax) {
            if (tempStatus == SS_NORMAL) P_PRINTF("[ALARM] 温度超限! %.1f°C (范围: %.1f-%.1f)\n", currentTemperature, thresholds.tempMin, thresholds.tempMax);
            tempStatus = SS_WARNING;
        } else {
            tempStatus = SS_NORMAL;
        }
    }
    if (humStatus != SS_DISCONNECTED) {
        if (currentHumidity < thresholds.humMin || currentHumidity > thresholds.humMax) {
            if (humStatus == SS_NORMAL) P_PRINTF("[ALARM] 湿度超限! %.1f%% (范围: %.1f-%.1f)\n", currentHumidity, thresholds.humMin, thresholds.humMax);
            humStatus = SS_WARNING;
        } else {
            humStatus = SS_NORMAL;
        }
    }
    if (gasStatus != SS_DISCONNECTED) {
        if (currentGasValue < thresholds.gasMin || currentGasValue > thresholds.gasMax) {
            if (gasStatus == SS_NORMAL) P_PRINTF("[ALARM] 气体浓度超限! %d (范围: %d-%d)\n", currentGasValue, thresholds.gasMin, thresholds.gasMax);
            gasStatus = SS_WARNING;
        } else {
            gasStatus = SS_NORMAL;
        }
    }

    if (tempStatus == SS_WARNING || humStatus == SS_WARNING || gasStatus == SS_WARNING) {
        if (!buzzerShouldBeActive) {
            buzzerShouldBeActive = true;
            buzzerBeepCount = 0;
            P_PRINTLN("[ALARM] 蜂鸣器激活!");
        }
    } else {
        if (buzzerShouldBeActive) {
            buzzerShouldBeActive = false;
            digitalWrite(BUZZER_PIN, LOW);
            buzzerBeepCount = 0;
            P_PRINTLN("[ALARM] 报警解除, 蜂鸣器停止.");
        }
    }
}

void updateLedStatus() {
    unsigned long currentTime = millis();
    uint32_t colorToSet = COLOR_OFF_VAL;
    if (isScanningWifi) { // 扫描WiFi时优先显示
        if (currentTime - lastBlinkTime >= 200) { // 扫描时快速闪烁
            lastBlinkTime = currentTime;
            ledBlinkState = !ledBlinkState;
        }
        colorToSet = ledBlinkState ? COLOR_BLUE_VAL : pixels.Color(0,0,30); // 暗蓝色/蓝色交替
    }
    else if (tempStatus == SS_DISCONNECTED || humStatus == SS_DISCONNECTED || gasStatus == SS_DISCONNECTED) {
        if (currentTime - lastBlinkTime >= 500) {
            lastBlinkTime = currentTime;
            ledBlinkState = !ledBlinkState;
        }
        colorToSet = ledBlinkState ? COLOR_BLUE_VAL : COLOR_OFF_VAL;
    } else if (tempStatus == SS_WARNING || humStatus == SS_WARNING || gasStatus == SS_WARNING) {
        colorToSet = COLOR_RED_VAL;
    } else if (wifiConnectState == WIFI_CP_CONNECTING || wifiConnectState == WIFI_CP_DISCONNECTING) {
         if (currentTime - lastBlinkTime >= 300) {
            lastBlinkTime = currentTime;
            ledBlinkState = !ledBlinkState;
        }
        colorToSet = ledBlinkState ? COLOR_BLUE_VAL : pixels.Color(0,0,50);
    }
    else if (!WiFi.isConnected() && (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA)) {
        colorToSet = COLOR_YELLOW_VAL;
    }
    else {
        colorToSet = COLOR_GREEN_VAL;
    }
    pixels.setPixelColor(0, colorToSet);
    pixels.show();
}

void controlBuzzer() {
    unsigned long currentTime = millis();
    if (buzzerShouldBeActive) {
        if (buzzerBeepCount < BUZZER_ALARM_COUNT) {
            if (currentTime >= buzzerStopTime) {
                if (digitalRead(BUZZER_PIN) == HIGH) {
                    digitalWrite(BUZZER_PIN, LOW);
                    buzzerStopTime = currentTime + BUZZER_ALARM_INTERVAL;
                } else {
                    digitalWrite(BUZZER_PIN, HIGH);
                    buzzerStopTime = currentTime + BUZZER_ALARM_DURATION;
                    buzzerBeepCount++;
                }
            }
        } else {
            digitalWrite(BUZZER_PIN, LOW);
        }
    } else {
        digitalWrite(BUZZER_PIN, LOW);
        buzzerBeepCount = 0;
        buzzerStopTime = 0;
    }
}

// ==========================================================================
// == WebSocket 处理实现 (修改 scanWifi action) ==
// ==========================================================================
void onWebSocketEvent(uint8_t clientNum, WStype_t type, uint8_t * payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            P_PRINTF("[%u] WebSocket已断开连接!\n", clientNum);
            // 如果发起扫描的客户端断开连接，重置扫描请求者
            if (isScanningWifi && wifiScanRequesterClientNum == clientNum) {
                P_PRINTLN("[WIFI_SCAN] 请求扫描的客户端已断开，取消扫描结果发送。");
                wifiScanRequesterClientNum = 255; // 无效化请求者
            }
            break;
        case WStype_CONNECTED: {
            IPAddress ip = webSocket.remoteIP(clientNum);
            P_PRINTF("[%u] WebSocket已连接, IP: %s\n", clientNum, ip.toString().c_str());
            sendWifiStatusToClients(clientNum);
            sendSensorDataToClients(clientNum);
            sendHistoricalDataToClient(clientNum);
            sendCurrentSettingsToClient(clientNum);
            break;
        }
        case WStype_TEXT:
            handleWebSocketMessage(clientNum, payload, length);
            break;
        case WStype_BIN:
            P_PRINTF("[%u] 收到二进制数据,长度: %u\n", clientNum, length);
            break;
        case WStype_PONG:
            // P_PRINTF("[%u] 收到Pong.\n", clientNum); // 可以注释掉以减少日志
            break;
        case WStype_PING:
            // P_PRINTF("[%u] 收到Ping.\n", clientNum); // 可以注释掉
            break;
        default:
            // P_PRINTF("[%u] 未处理的WS事件类型: %d\n", clientNum, type); // 可以注释掉
            break;
    }
}

// sendSensorDataToClients, sendWifiStatusToClients, sendHistoricalDataToClient, sendCurrentSettingsToClient (保持不变)
void sendSensorDataToClients(uint8_t specificClientNum) {
    DynamicJsonDocument doc(512);
    doc["type"] = "sensorData";
    if (isnan(currentTemperature)) doc["temperature"] = nullptr; else doc["temperature"] = currentTemperature;
    if (isnan(currentHumidity)) doc["humidity"] = nullptr; else doc["humidity"] = currentHumidity;
    if (currentGasValue < 0) doc["gas"] = nullptr; else doc["gas"] = currentGasValue;
    doc["tempStatus"] = (tempStatus == SS_NORMAL) ? "normal" : ((tempStatus == SS_WARNING) ? "warning" : "disconnected");
    doc["humStatus"]  = (humStatus  == SS_NORMAL) ? "normal" : ((humStatus  == SS_WARNING) ? "warning" : "disconnected");
    doc["gasStatus"]  = (gasStatus  == SS_NORMAL) ? "normal" : ((gasStatus  == SS_WARNING) ? "warning" : "disconnected");
    String jsonString;
    serializeJson(doc, jsonString);
    if (specificClientNum != 255 && specificClientNum < webSocket.connectedClients()) webSocket.sendTXT(specificClientNum, jsonString);
    else webSocket.broadcastTXT(jsonString);
}

void sendWifiStatusToClients(uint8_t specificClientNum) {
    DynamicJsonDocument doc(512);
    doc["type"] = "wifiStatus";
    if (WiFi.isConnected()) {
        doc["connected"] = true; doc["ssid"] = WiFi.SSID(); doc["ip"] = WiFi.localIP().toString();
    } else {
        doc["connected"] = false; doc["ssid"] = "N/A";
        if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
             doc["ip"] = WiFi.softAPIP().toString(); doc["ap_mode"] = true; doc["ap_ssid"] = WIFI_AP_SSID;
        } else { doc["ip"] = "N/A"; }
    }
    String jsonString;
    serializeJson(doc, jsonString);
    if (specificClientNum != 255 && specificClientNum < webSocket.connectedClients()) webSocket.sendTXT(specificClientNum, jsonString);
    else webSocket.broadcastTXT(jsonString);
}

void sendHistoricalDataToClient(uint8_t clientNum) {
    if (clientNum >= webSocket.connectedClients()) return;
    P_PRINTF("[HISTORY] 发送历史数据给客户端 %u (%u 条)\n", clientNum, historicalData.size());
    const size_t capacity = JSON_ARRAY_SIZE(historicalData.size()) + historicalData.size() * JSON_OBJECT_SIZE(5);
    DynamicJsonDocument doc(min((size_t)(1024 * 12), capacity));
    doc["type"] = "historicalData";
    JsonArray historyArr = doc.createNestedArray("history");
    for (const auto& dp : historicalData) {
        JsonObject dataPoint = historyArr.createNestedObject();
        dataPoint["time"] = dp.timeStr; dataPoint["temp"] = dp.temp; dataPoint["hum"] = dp.hum; dataPoint["gas"] = dp.gas;
    }
    String jsonString;
    if (serializeJson(doc, jsonString) > 0) webSocket.sendTXT(clientNum, jsonString);
    else {
        P_PRINTLN("[HISTORY] 序列化历史数据失败.");
        DynamicJsonDocument errDoc(128); errDoc["type"] = "historicalData"; errDoc.createNestedArray("history");
        String errStr; serializeJson(errDoc, errStr); webSocket.sendTXT(clientNum, errStr);
    }
}

void sendCurrentSettingsToClient(uint8_t clientNum) {
    if (clientNum >= webSocket.connectedClients()) return;
    P_PRINTF("[SETTINGS] 发送当前设置给客户端 %u\n", clientNum);
    DynamicJsonDocument doc(1024);
    doc["type"] = "settingsData";
    JsonObject settingsObj = doc.createNestedObject("settings");
    JsonObject thresholdsObj = settingsObj.createNestedObject("thresholds");
    thresholdsObj["tempMin"] = thresholds.tempMin; thresholdsObj["tempMax"] = thresholds.tempMax;
    thresholdsObj["humMin"] = thresholds.humMin; thresholdsObj["humMax"] = thresholds.humMax;
    thresholdsObj["gasMin"] = thresholds.gasMin; thresholdsObj["gasMax"] = thresholds.gasMax;
    settingsObj["currentSSID"] = WiFi.isConnected() ? WiFi.SSID() : currentSsidForSettings;
    String jsonString;
    serializeJson(doc, jsonString);
    webSocket.sendTXT(clientNum, jsonString);
}


void handleWebSocketMessage(uint8_t clientNum, uint8_t * payload, size_t length) {
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, payload, length);
    if (error) {
        P_PRINTF("[%u] WS JSON解析失败: %s\n", clientNum, error.c_str());
        DynamicJsonDocument errDoc(128); errDoc["type"] = "error"; errDoc["message"] = "Invalid JSON.";
        String errStr; serializeJson(errDoc, errStr); webSocket.sendTXT(clientNum, errStr);
        return;
    }
    const char* action = doc["action"];
    if (!action) {
        P_PRINTLN("[%u] WS消息缺少 'action'.");
        DynamicJsonDocument errDoc(128); errDoc["type"] = "error"; errDoc["message"] = "Missing 'action'.";
        String errStr; serializeJson(errDoc, errStr); webSocket.sendTXT(clientNum, errStr);
        return;
    }
    P_PRINTF("[%u] WS action: %s\n", clientNum, action);
    DynamicJsonDocument responseDoc(256);

    if (strcmp(action, "getCurrentSettings") == 0) { sendCurrentSettingsToClient(clientNum); return; }
    if (strcmp(action, "getHistoricalData") == 0) { sendHistoricalDataToClient(clientNum); return; }

    if (strcmp(action, "saveThresholds") == 0) {
        thresholds.tempMin = doc["tempMin"].is<float>() ? doc["tempMin"].as<float>() : thresholds.tempMin;
        thresholds.tempMax = doc["tempMax"].is<float>() ? doc["tempMax"].as<float>() : thresholds.tempMax;
        thresholds.humMin  = doc["humMin"].is<float>()  ? doc["humMin"].as<float>()  : thresholds.humMin;
        thresholds.humMax  = doc["humMax"].is<float>()  ? doc["humMax"].as<float>()  : thresholds.humMax;
        thresholds.gasMin  = doc["gasMin"].is<int>()    ? doc["gasMin"].as<int>()    : thresholds.gasMin;
        thresholds.gasMax  = doc["gasMax"].is<int>()    ? doc["gasMax"].as<int>()    : thresholds.gasMax;
        saveSettings(); checkAlarms();
        responseDoc["type"] = "saveSettingsStatus"; responseDoc["success"] = true; responseDoc["message"] = "Thresholds saved.";
    } else if (strcmp(action, "scanWifi") == 0) {
        if (isScanningWifi) {
            P_PRINTLN("[WIFI_SCAN] WiFi扫描已在进行中.");
            responseDoc["type"] = "wifiScanResults"; // 可以用这个类型回复
            responseDoc["error"] = "Scan already in progress.";
            JsonArray emptyNetworks = responseDoc.createNestedArray("networks"); // 发送空列表
        } else {
            P_PRINTLN("[WIFI_SCAN] 请求异步扫描WiFi...");
            // 启动异步扫描: async=true, show_hidden=true, passive=false, max_ms_per_chan=300, channel=0 (all)
            if (WiFi.scanNetworks(true, true, false, 300, 0) == WIFI_SCAN_RUNNING) { // -1 表示扫描已开始
                isScanningWifi = true;
                wifiScanRequesterClientNum = clientNum;
                wifiScanStartTime = millis(); // 记录扫描开始时间以处理超时
                responseDoc["type"] = "scanStatus"; // 可以发送一个扫描开始的状态
                responseDoc["message"] = "WiFi scan initiated...";
                 P_PRINTLN("[WIFI_SCAN] 异步扫描已启动.");
            } else {
                P_PRINTLN("[WIFI_SCAN] 启动异步扫描失败.");
                responseDoc["type"] = "wifiScanResults";
                responseDoc["error"] = "Failed to start WiFi scan.";
                JsonArray emptyNetworks = responseDoc.createNestedArray("networks");
            }
        }
        // 不在此处直接返回扫描结果，结果将在 processWifiScan() 中异步发送
        // 此处仅返回启动状态或错误
    } else if (strcmp(action, "connectWifi") == 0) {
        if (wifiConnectState != WIFI_CP_IDLE && wifiConnectState != WIFI_CP_FAILED) {
            P_PRINTLN("[WIFI_CONN] WiFi连接已在进行中.");
            responseDoc["type"] = "connectWifiStatus"; responseDoc["success"] = false; responseDoc["message"] = "Connection attempt already in progress.";
        } else {
            wifiSsidToTry = doc["ssid"].as<String>();
            wifiPasswordToTry = doc["password"].as<String>();
            P_PRINTF("[WIFI_CONN] 收到连接请求: SSID=%s\n", wifiSsidToTry.c_str());
            if (wifiSsidToTry.length() == 0) {
                responseDoc["type"] = "connectWifiStatus"; responseDoc["success"] = false; responseDoc["message"] = "SSID cannot be empty.";
            } else {
                wifiConnectInitiatorClientNum = clientNum;
                wifiConnectState = WIFI_CP_DISCONNECTING;
                wifiConnectAttemptStartTime = millis();
                WiFi.disconnect(false);
                responseDoc["type"] = "connectWifiStatus"; responseDoc["success"] = false;
                responseDoc["message"] = "Initiating connection to " + wifiSsidToTry + "...";
            }
        }
    } else if (strcmp(action, "resetSettings") == 0) {
        P_PRINTLN("[RESET] 收到恢复出厂设置请求.");
        resetAllSettingsToDefault(); saveSettings(); historicalData.clear(); saveHistoricalData();
        responseDoc["type"] = "resetStatus"; responseDoc["success"] = true; responseDoc["message"] = "Settings reset. Device will restart.";
        String respStr; serializeJson(responseDoc, respStr); webSocket.sendTXT(clientNum, respStr);
        P_PRINTLN("[RESET] 设置已重置, 准备重启...");
        delay(1000); ESP.restart(); return;
    } else {
        P_PRINTF("[%u] 未知WS action: %s\n", clientNum, action);
        responseDoc["type"] = "error"; responseDoc["message"] = "Unknown action.";
    }
    String respStr; serializeJson(responseDoc, respStr); webSocket.sendTXT(clientNum, respStr);
}

// ==========================================================================
// == Web 服务器路由配置 (与之前版本相同，此处省略) ==
// ... (serveStaticFile, configureWebServer)
// ==========================================================================
void serveStaticFile(AsyncWebServerRequest *request, const char* path, const char* contentType) {
    if (SPIFFS.exists(path)) {
        request->send(SPIFFS, path, contentType);
    } else {
        P_PRINTF("[HTTP] 文件未找到: %s\n", path);
        request->send(404, "text/plain", "404: File Not Found");
    }
}
void configureWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){ serveStaticFile(request, "/index.html", "text/html"); });
    server.on("/settings.html", HTTP_GET, [](AsyncWebServerRequest *request){ serveStaticFile(request, "/settings.html", "text/html"); });
    server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){ serveStaticFile(request, "/style.css", "text/css"); });
    server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request){ serveStaticFile(request, "/script.js", "application/javascript"); });
    server.on("/lang.json", HTTP_GET, [](AsyncWebServerRequest *request){ serveStaticFile(request, "/lang.json", "application/json"); });
    server.on("/chart.min.js", HTTP_GET, [](AsyncWebServerRequest *request){ serveStaticFile(request, "/chart.min.js", "application/javascript"); });
    server.onNotFound([](AsyncWebServerRequest *request){
        if (request->url() == "/ws") {
            P_PRINTF("[HTTP] HTTP请求到WebSocket路径 /ws, 已忽略.\n");
            request->send(400, "text/plain", "Bad Request: Use WebSocket protocol for /ws");
            return;
        }
        P_PRINTF("[HTTP] 未找到 (onNotFound): %s\n", request->url().c_str());
        request->send(404, "text/plain", "404: Not Found");
    });
}

// ==========================================================================
// == 工具函数实现 (与之前版本相同，此处省略) ==
// ... (resetAllSettingsToDefault, generateTimeStr, addHistoricalDataPoint)
// ==========================================================================
void resetAllSettingsToDefault() {
    P_PRINTLN("[SETTINGS] 重置所有设置为默认值 (内存中).");
    thresholds.tempMin = DEFAULT_TEMP_MIN; thresholds.tempMax = DEFAULT_TEMP_MAX;
    thresholds.humMin = DEFAULT_HUM_MIN; thresholds.humMax = DEFAULT_HUM_MAX;
    thresholds.gasMin = DEFAULT_GAS_MIN; thresholds.gasMax = DEFAULT_GAS_MAX;
    currentSsidForSettings = "";
}

void generateTimeStr(unsigned long current_ts, char* buffer) {
    unsigned long secondsInDay = current_ts % (24UL * 60UL * 60UL);
    int hours = secondsInDay / 3600;
    int minutes = (secondsInDay % 3600) / 60;
    int seconds = secondsInDay % 60;
    sprintf(buffer, "%02d:%02d:%02d", hours, minutes, seconds);
}

void addHistoricalDataPoint(float temp, float hum, float gas_val) {
    if (isnan(temp) || isnan(hum) || gas_val < 0) return;
    SensorDataPoint dp;
    dp.timestamp = millis() / 1000;
    dp.temp = temp; dp.hum = hum; dp.gas = gas_val;
    generateTimeStr(dp.timestamp, dp.timeStr);
    if (historicalData.size() >= HISTORICAL_DATA_POINTS) {
        historicalData.erase(historicalData.begin());
    }
    historicalData.push_back(dp);
}
