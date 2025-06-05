// ==========================================================================
// == ESP32 温湿度及多通道气体监测器 V2 (使用 Multichannel_Gas_GMXXX) ==
// ==========================================================================

// == 核心库 ==
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <Adafruit_NeoPixel.h>
#include <DHT.h>
#include "Multichannel_Gas_GMXXX.h" // 使用新的气体传感器库
#include <Wire.h>                   // I2C 通信库
#include <map>                      // 用于 WebSocket action 映射
#include <functional>               // 用于 std::function
#include <time.h>                   // 用于NTP时间处理

// == 自定义配置文件 ==
#include "config.h" // NTP_SYNC_INTERVAL_MS 将从此文件引入

// == 数据结构与状态管理 ==

// 传感器状态枚举
enum SensorStatusVal { SS_NORMAL, SS_WARNING, SS_DISCONNECTED, SS_INIT };

// 多通道气体传感器数据结构 (存储原始ADC值或转换后的PPM)
struct GasData {
    float co;      // 一氧化碳 (CO) - 当前存储ADC值
    float no2;     // 二氧化氮 (NO2) - 当前存储ADC值
    float c2h5oh;  // 乙醇 (C2H5OH) - 当前存储ADC值
    float voc;     // 挥发性有机物 (VOC) - 当前存储ADC值
};

// 设备当前状态
struct DeviceState {
    float temperature;
    float humidity;
    GasData gasValues;

    SensorStatusVal tempStatus;
    SensorStatusVal humStatus;
    SensorStatusVal gasCoStatus;
    SensorStatusVal gasNo2Status;
    SensorStatusVal gasC2h5ohStatus;
    SensorStatusVal gasVocStatus;
    
    bool buzzerShouldBeActive;
    unsigned long buzzerStopTime;
    int buzzerBeepCount;

    bool ledBlinkState;
    unsigned long lastBlinkTime;

    DeviceState() : temperature(NAN), humidity(NAN),
                    tempStatus(SS_INIT), humStatus(SS_INIT),
                    gasCoStatus(SS_INIT), gasNo2Status(SS_INIT),
                    gasC2h5ohStatus(SS_INIT), gasVocStatus(SS_INIT),
                    buzzerShouldBeActive(false), buzzerStopTime(0), buzzerBeepCount(0),
                    ledBlinkState(false), lastBlinkTime(0) {
        gasValues = {NAN, NAN, NAN, NAN}; // 初始化为NAN
    }
};

// 报警阈值配置
struct AlarmThresholds {
    float tempMin, tempMax;
    float humMin, humMax;
    float coMin, coMax;         // 注意: 这些阈值需要与 gasValues 中的单位一致 (ADC或PPM)
    float no2Min, no2Max;
    float c2h5ohMin, c2h5ohMax;
    float vocMin, vocMax;
};

// 设备配置 (从SPIFFS加载/保存)
struct DeviceConfig {
    AlarmThresholds thresholds;
    String currentSsidForSettings; // 用于保存到设置的SSID
    uint8_t ledBrightness;       // LED亮度 (0-100)

    DeviceConfig() : ledBrightness(DEFAULT_LED_BRIGHTNESS) {
        thresholds = {
            DEFAULT_TEMP_MIN, DEFAULT_TEMP_MAX,
            DEFAULT_HUM_MIN, DEFAULT_HUM_MAX,
            DEFAULT_CO_MIN, DEFAULT_CO_MAX,
            DEFAULT_NO2_MIN, DEFAULT_NO2_MAX,
            DEFAULT_C2H5OH_MIN, DEFAULT_C2H5OH_MAX,
            DEFAULT_VOC_MIN, DEFAULT_VOC_MAX
        };
    }
};

// WiFi 连接状态管理
enum WifiConnectProgress { WIFI_CP_IDLE, WIFI_CP_DISCONNECTING, WIFI_CP_CONNECTING, WIFI_CP_FAILED };
struct WifiState {
    WifiConnectProgress connectProgress;
    String ssidToTry;
    String passwordToTry;
    unsigned long connectAttemptStartTime;
    uint8_t connectInitiatorClientNum; // 发起连接的客户端编号
    bool isScanning;
    uint8_t scanRequesterClientNum;    // 发起扫描的客户端编号
    unsigned long scanStartTime;

    WifiState() : connectProgress(WIFI_CP_IDLE), connectAttemptStartTime(0),
                  connectInitiatorClientNum(255), isScanning(false),
                  scanRequesterClientNum(255), scanStartTime(0) {}
};

// 历史数据点结构
struct SensorDataPoint {
    unsigned long timestamp; // Unix timestamp (seconds since epoch)
    float temp;
    float hum;
    GasData gas; // 存储气体数据
    char timeStr[9]; // HH:MM:SS 格式的时间字符串 (用于图表)
};

// 环形缓冲区实现
class CircularBuffer {
public:
    CircularBuffer(size_t size) : maxSize(size), head(0), tail(0), full(false) {
        buffer.resize(size);
    }

    void add(const SensorDataPoint& item) {
        buffer[head] = item;
        if (full) {
            tail = (tail + 1) % maxSize;
        }
        head = (head + 1) % maxSize;
        full = (head == tail);
    }

    const std::vector<SensorDataPoint>& getData() const {
        orderedData.clear();
        if (isEmpty()) return orderedData;
        if (full) {
            for (size_t i = 0; i < maxSize; ++i) {
                orderedData.push_back(buffer[(tail + i) % maxSize]);
            }
        } else {
            for (size_t i = tail; i != head; i = (i + 1) % maxSize) {
                orderedData.push_back(buffer[i]);
            }
        }
        return orderedData;
    }
    
    size_t count() const {
        if (full) return maxSize;
        if (head >= tail) return head - tail;
        return maxSize - (tail - head);
    }

    bool isEmpty() const {
        return !full && (head == tail);
    }
    
    void clear() {
        head = 0;
        tail = 0;
        full = false;
        orderedData.clear();
    }

private:
    std::vector<SensorDataPoint> buffer;
    mutable std::vector<SensorDataPoint> orderedData;
    size_t maxSize;
    size_t head;
    size_t tail;
    bool full;
};


// ==========================================================================
// == 全局对象和变量 ==
// ==========================================================================
AsyncWebServer server(80);
WebSocketsServer webSocket(81);

DHT dht(DHT_PIN, DHT_TYPE);
GAS_GMXXX<TwoWire> gas_sensor; // 使用新的气体传感器库类型

Adafruit_NeoPixel pixels(NEOPIXEL_NUM, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

uint32_t COLOR_GREEN_VAL;
uint32_t COLOR_RED_VAL;
uint32_t COLOR_BLUE_VAL;
uint32_t COLOR_YELLOW_VAL;
uint32_t COLOR_ORANGE_VAL;
uint32_t COLOR_OFF_VAL;

DeviceState currentState;
DeviceConfig currentConfig;
WifiState wifiState;

CircularBuffer historicalData(HISTORICAL_DATA_POINTS);

unsigned long lastSensorReadTime = 0;
unsigned long lastWebSocketUpdateTime = 0;
unsigned long lastHistoricalDataSaveTime = 0;

// NTP 相关状态
unsigned long lastNtpSyncTime = 0;
bool ntpSynced = false;
int ntpInitialAttempts = 0; 
unsigned long lastNtpAttemptTime = 0; 

unsigned long gasSensorWarmupEndTime = 0; 

// WebSocket action 处理函数映射
typedef std::function<void(uint8_t, const JsonDocument&, JsonDocument&)> WebSocketActionHandler;
std::map<String, WebSocketActionHandler> wsActionHandlers;

// ==========================================================================
// == 函数声明 ==
// ==========================================================================
void initHardware();
void initSPIFFS();
void initWiFi(DeviceConfig& config, WifiState& wifiStatus);
void attemptNtpSync(); 
void configureWebServer();
void setupWebSocketActions();
void loadConfig(DeviceConfig& config);
void saveConfig(const DeviceConfig& config);
void loadHistoricalDataFromFile(CircularBuffer& histBuffer);
void saveHistoricalDataToFile(const CircularBuffer& histBuffer);
void resetAllSettingsToDefault(DeviceConfig& config);
void processWiFiConnection(WifiState& wifiStatus, DeviceConfig& config);
void startWifiScan(uint8_t clientNum, WifiState& wifiStatus, JsonDocument& responseDoc);
void processWifiScanResults(WifiState& wifiStatus);
void readSensors(DeviceState& state);
void checkAlarms(DeviceState& state, const DeviceConfig& config);
void updateLedStatus(const DeviceState& state, const WifiState& wifiStatus, const Adafruit_NeoPixel& led);
void controlBuzzer(DeviceState& state);
void onWebSocketEvent(uint8_t clientNum, WStype_t type, uint8_t * payload, size_t length);
void handleWebSocketMessage(uint8_t clientNum, const JsonDocument& doc, JsonDocument& responseDoc);
void sendSensorDataToClients(const DeviceState& state, uint8_t specificClientNum = 255);
void sendWifiStatusToClients(const WifiState& wifiStatus, uint8_t specificClientNum = 255);
void sendHistoricalDataToClient(uint8_t clientNum, const CircularBuffer& histBuffer);
void sendCurrentSettingsToClient(uint8_t clientNum, const DeviceConfig& config);
void handleGetCurrentSettingsRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response);
void handleGetHistoricalDataRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response);
void handleSaveThresholdsRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response);
void handleSaveLedBrightnessRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response);
void handleScanWifiRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response);
void handleConnectWifiRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response);
void handleResetSettingsRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response);
void generateTimeStr(unsigned long current_timestamp, char* buffer);
void addHistoricalDataPoint(CircularBuffer& histBuffer, const DeviceState& state);
void serveStaticFile(AsyncWebServerRequest *request, const char* path, const char* contentType);
String getSensorStatusString(SensorStatusVal status);


// ==========================================================================
// == Arduino `setup()` 函数 ==
// ==========================================================================
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000);
    P_PRINTLN("\n[SETUP] 系统启动中...");

    initHardware(); 
    initSPIFFS();
    
    loadConfig(currentConfig);
    pixels.setBrightness(map(currentConfig.ledBrightness, 0, 100, 0, 255));
    pixels.show();

    loadHistoricalDataFromFile(historicalData);
    
    initWiFi(currentConfig, wifiState);
    
    P_PRINTLN("[NTP] 初始配置NTP...");
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER1, NTP_SERVER2);

    configureWebServer();
    server.begin();
    P_PRINTLN("[SETUP] HTTP服务器已启动 (端口 80)");

    setupWebSocketActions();
    webSocket.begin();
    webSocket.onEvent(onWebSocketEvent);
    P_PRINTLN("[SETUP] WebSocket服务器已启动 (端口 81)");

    gasSensorWarmupEndTime = millis() + GAS_SENSOR_WARMUP_PERIOD_MS; 
    gas_sensor.preheated(); 
    P_PRINTLN("[SETUP] 初始化完成, 系统运行中.");
}

// ==========================================================================
// == Arduino `loop()` 函数 ==
// ==========================================================================
void loop() {
    webSocket.loop(); 
    processWiFiConnection(wifiState, currentConfig); 
    processWifiScanResults(wifiState); 

    unsigned long currentTime = millis();

    if (WiFi.isConnected() && !ntpSynced && ntpInitialAttempts < MAX_NTP_ATTEMPTS_AFTER_WIFI) {
        if (currentTime - lastNtpAttemptTime >= NTP_RETRY_DELAY_MS || lastNtpAttemptTime == 0) {
            attemptNtpSync();
            lastNtpAttemptTime = currentTime;
        }
    }
    if (ntpSynced && (currentTime - lastNtpSyncTime >= NTP_SYNC_INTERVAL_MS)) { // NTP_SYNC_INTERVAL_MS is now a macro
        P_PRINTLN("[NTP] 尝试每小时重新同步时间...");
        attemptNtpSync(); 
    }


    if (currentTime - lastSensorReadTime >= SENSOR_READ_INTERVAL_MS) {
        lastSensorReadTime = currentTime;
        readSensors(currentState);
        checkAlarms(currentState, currentConfig);
        if (ntpSynced) { 
             addHistoricalDataPoint(historicalData, currentState);
        }
    }

    updateLedStatus(currentState, wifiState, pixels);
    controlBuzzer(currentState);

    if (currentTime - lastWebSocketUpdateTime >= WEBSOCKET_UPDATE_INTERVAL_MS) {
        lastWebSocketUpdateTime = currentTime;
        if (wifiState.connectProgress == WIFI_CP_IDLE || wifiState.connectProgress == WIFI_CP_FAILED) {
             if (!wifiState.isScanning) { 
                sendSensorDataToClients(currentState);
                sendWifiStatusToClients(wifiState);
            }
        }
    }

    if (currentTime - lastHistoricalDataSaveTime >= HISTORICAL_DATA_SAVE_INTERVAL_MS) {
        lastHistoricalDataSaveTime = currentTime;
        saveHistoricalDataToFile(historicalData);
    }
}

// ==========================================================================
// == 初始化函数实现 ==
// ==========================================================================
void initHardware() {
    pixels.begin();
    pixels.setBrightness(map(DEFAULT_LED_BRIGHTNESS, 0, 100, 0, 255));
    pixels.clear();
    pixels.show();
    P_PRINTLN("[HW] NeoPixel RGB LED已初始化.");

    COLOR_GREEN_VAL  = pixels.Color(0, 120, 0);
    COLOR_RED_VAL    = pixels.Color(120, 0, 0);
    COLOR_BLUE_VAL   = pixels.Color(0, 0, 120);
    COLOR_YELLOW_VAL = pixels.Color(120, 120, 0);
    COLOR_ORANGE_VAL = pixels.Color(255, 165, 0);
    COLOR_OFF_VAL    = pixels.Color(0, 0, 0);

    pixels.setPixelColor(0, COLOR_BLUE_VAL);
    pixels.show();

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    P_PRINTLN("[HW] 蜂鸣器已初始化.");

    dht.begin();
    P_PRINTLN("[HW] DHT传感器已初始化.");

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN); 
    P_PRINTF("[HW] I2C总线已在 SDA=%d, SCL=%d 初始化.\n", I2C_SDA_PIN, I2C_SCL_PIN);
    
    gas_sensor.begin(Wire, GAS_SENSOR_I2C_ADDRESS); 
    P_PRINTF("[HW] Grove多通道气体传感器GMXXX尝试在地址 0x%02X 初始化...\n", GAS_SENSOR_I2C_ADDRESS);
    delay(100); 
}

void initSPIFFS() {
    if (!SPIFFS.begin(true)) {
        P_PRINTLN("[SPIFFS] 初始化失败! 数据可能无法保存或加载.");
    } else {
        P_PRINTLN("[SPIFFS] 文件系统已挂载.");
    }
}

void attemptNtpSync() { // Renamed from initNTP to reflect its purpose
    if (ntpSynced && millis() - lastNtpSyncTime < NTP_SYNC_INTERVAL_MS && ntpInitialAttempts >= MAX_NTP_ATTEMPTS_AFTER_WIFI) {
        return;
    }
    if (!WiFi.isConnected()) {
        P_PRINTLN("[NTP] WiFi未连接, 无法同步时间.");
        ntpSynced = false; 
        return;
    }

    P_PRINTF("[NTP] 尝试同步时间 (尝试次数: %d/%d)...\n", ntpInitialAttempts + 1, MAX_NTP_ATTEMPTS_AFTER_WIFI);
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER1, NTP_SERVER2); 
    
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo, 5000)){ 
        P_PRINTLN("[NTP] 获取时间失败.");
        ntpSynced = false;
        if (WiFi.isConnected()) { 
             ntpInitialAttempts++;
        }
    } else {
        char timeBuffer[80];
        strftime(timeBuffer, sizeof(timeBuffer), "%A, %B %d %Y %H:%M:%S", &timeinfo);
        P_PRINTF("[NTP] 时间已同步: %s\n", timeBuffer);
        ntpSynced = true;
        lastNtpSyncTime = millis();
        ntpInitialAttempts = MAX_NTP_ATTEMPTS_AFTER_WIFI; 
    }
}


void loadConfig(DeviceConfig& config) {
    P_PRINTLN("[CONFIG] 正在加载配置...");
    if (SPIFFS.exists(SETTINGS_FILE)) {
        File file = SPIFFS.open(SETTINGS_FILE, "r");
        if (file && file.size() > 0) {
            DynamicJsonDocument doc(2048); 
            DeserializationError error = deserializeJson(doc, file);
            if (error) {
                P_PRINTF("[CONFIG] JSON反序列化失败: %s\n", error.c_str());
                resetAllSettingsToDefault(config);
            } else {
                JsonObject thresholdsObj = doc["thresholds"];
                config.thresholds.tempMin = thresholdsObj["tempMin"] | DEFAULT_TEMP_MIN;
                config.thresholds.tempMax = thresholdsObj["tempMax"] | DEFAULT_TEMP_MAX;
                config.thresholds.humMin  = thresholdsObj["humMin"]  | DEFAULT_HUM_MIN;
                config.thresholds.humMax  = thresholdsObj["humMax"]  | DEFAULT_HUM_MAX;
                config.thresholds.coMin   = thresholdsObj["coMin"]   | DEFAULT_CO_MIN;
                config.thresholds.coMax   = thresholdsObj["coMax"]   | DEFAULT_CO_MAX;
                config.thresholds.no2Min  = thresholdsObj["no2Min"]  | DEFAULT_NO2_MIN;
                config.thresholds.no2Max  = thresholdsObj["no2Max"]  | DEFAULT_NO2_MAX;
                config.thresholds.c2h5ohMin = thresholdsObj["c2h5ohMin"] | DEFAULT_C2H5OH_MIN;
                config.thresholds.c2h5ohMax = thresholdsObj["c2h5ohMax"] | DEFAULT_C2H5OH_MAX;
                config.thresholds.vocMin  = thresholdsObj["vocMin"]  | DEFAULT_VOC_MIN;
                config.thresholds.vocMax  = thresholdsObj["vocMax"]  | DEFAULT_VOC_MAX;
                
                config.currentSsidForSettings = doc["wifi"]["ssid"].as<String>();
                config.ledBrightness = doc["led"]["brightness"] | DEFAULT_LED_BRIGHTNESS;
                P_PRINTLN("[CONFIG] 配置加载成功.");
            }
        } else {
            P_PRINTLN(file ? "[CONFIG] 配置文件为空." : "[CONFIG] 打开配置文件失败.");
            resetAllSettingsToDefault(config);
        }
        if(file) file.close();
    } else {
        P_PRINTLN("[CONFIG] 配置文件不存在, 使用默认值并保存.");
        resetAllSettingsToDefault(config);
        saveConfig(config);
    }
     P_PRINTF("  加载阈值 - 温度: %.1f-%.1f, 湿度: %.1f-%.1f\n",
                   config.thresholds.tempMin, config.thresholds.tempMax, config.thresholds.humMin, config.thresholds.humMax);
    P_PRINTF("  CO: %.0f-%.0f, NO2: %.0f-%.0f, C2H5OH: %.0f-%.0f, VOC: %.0f-%.0f (ADC值)\n",
                   config.thresholds.coMin, config.thresholds.coMax, config.thresholds.no2Min, config.thresholds.no2Max,
                   config.thresholds.c2h5ohMin, config.thresholds.c2h5ohMax, config.thresholds.vocMin, config.thresholds.vocMax);
    P_PRINTF("  加载的WiFi SSID (自动连接): %s\n", config.currentSsidForSettings.c_str());
    P_PRINTF("  加载的LED亮度: %d\n", config.ledBrightness);
}

void saveConfig(const DeviceConfig& config) {
    P_PRINTLN("[CONFIG] 正在保存配置...");
    File file = SPIFFS.open(SETTINGS_FILE, "w");
    if (file) {
        DynamicJsonDocument doc(2048); 
        JsonObject thresholdsObj = doc.createNestedObject("thresholds");
        thresholdsObj["tempMin"] = config.thresholds.tempMin;
        thresholdsObj["tempMax"] = config.thresholds.tempMax;
        thresholdsObj["humMin"]  = config.thresholds.humMin;
        thresholdsObj["humMax"]  = config.thresholds.humMax;
        thresholdsObj["coMin"]   = config.thresholds.coMin;
        thresholdsObj["coMax"]   = config.thresholds.coMax;
        thresholdsObj["no2Min"]  = config.thresholds.no2Min;
        thresholdsObj["no2Max"]  = config.thresholds.no2Max;
        thresholdsObj["c2h5ohMin"] = config.thresholds.c2h5ohMin;
        thresholdsObj["c2h5ohMax"] = config.thresholds.c2h5ohMax;
        thresholdsObj["vocMin"]  = config.thresholds.vocMin;
        thresholdsObj["vocMax"]  = config.thresholds.vocMax;

        JsonObject wifiObj = doc.createNestedObject("wifi");
        wifiObj["ssid"] = WiFi.isConnected() ? WiFi.SSID() : config.currentSsidForSettings;

        JsonObject ledObj = doc.createNestedObject("led");
        ledObj["brightness"] = config.ledBrightness;

        if (serializeJson(doc, file) == 0) {
            P_PRINTLN("[CONFIG] 写入配置文件失败.");
        } else {
            P_PRINTLN("[CONFIG] 配置保存成功.");
        }
        file.close();
    } else {
        P_PRINTLN("[CONFIG] 创建/打开配置文件用于写入失败.");
    }
}

void loadHistoricalDataFromFile(CircularBuffer& histBuffer) {
    P_PRINTLN("[HISTORY] 正在加载历史数据...");
    histBuffer.clear();
    if (SPIFFS.exists(HISTORICAL_DATA_FILE)) {
        File file = SPIFFS.open(HISTORICAL_DATA_FILE, "r");
        if (file && file.size() > 0) {
            DynamicJsonDocument doc(min((size_t)(1024 * 16), (size_t)(HISTORICAL_DATA_POINTS * 250)));
            DeserializationError error = deserializeJson(doc, file);
            if (error) {
                P_PRINTF("[HISTORY] JSON反序列化历史数据失败: %s\n", error.c_str());
            } else {
                JsonArray arr = doc.as<JsonArray>();
                unsigned int count = 0;
                for (JsonObject obj : arr) {
                    if (histBuffer.count() >= HISTORICAL_DATA_POINTS) break; 
                    SensorDataPoint dp;
                    dp.timestamp = obj["ts"]; 
                    dp.temp = obj["t"];
                    dp.hum = obj["h"];
                    dp.gas.co = obj["co"];
                    dp.gas.no2 = obj["no2"];
                    dp.gas.c2h5oh = obj["c2h5oh"]; 
                    dp.gas.voc = obj["voc"];
                    generateTimeStr(dp.timestamp, dp.timeStr); 
                    histBuffer.add(dp);
                    count++;
                }
                P_PRINTF("[HISTORY] 加载了 %u 条历史数据.\n", count);
            }
        } else { P_PRINTLN(file ? "[HISTORY] 文件为空." : "[HISTORY] 打开文件失败."); }
        if(file) file.close();
    } else { P_PRINTLN("[HISTORY] 文件不存在."); }
}

void saveHistoricalDataToFile(const CircularBuffer& histBuffer) {
    P_PRINTLN("[HISTORY] 正在保存历史数据...");
    File file = SPIFFS.open(HISTORICAL_DATA_FILE, "w");
    if (file) {
        const std::vector<SensorDataPoint>& dataToSave = histBuffer.getData();
        DynamicJsonDocument doc(min((size_t)(1024 * 16), (size_t)(dataToSave.size() * 250)));
        JsonArray arr = doc.to<JsonArray>();
        for (const auto& dp : dataToSave) {
            JsonObject obj = arr.createNestedObject();
            obj["ts"] = dp.timestamp;
            obj["t"] = dp.temp;
            obj["h"] = dp.hum;
            obj["co"] = dp.gas.co;
            obj["no2"] = dp.gas.no2;
            obj["c2h5oh"] = dp.gas.c2h5oh; 
            obj["voc"] = dp.gas.voc;
        }
        size_t bytesWritten = serializeJson(doc, file);
        if (bytesWritten == 0 && !dataToSave.empty()) P_PRINTLN("[HISTORY] 写入失败.");
        else P_PRINTF("[HISTORY] %u 条 (%lu B) 已保存.\n", dataToSave.size(), bytesWritten);
        file.close();
    } else { P_PRINTLN("[HISTORY] 创建文件失败."); }
}


void initWiFi(DeviceConfig& config, WifiState& wifiStatus) {
    WiFi.mode(WIFI_AP_STA);
    P_PRINTLN("[WIFI] 设置为AP+STA模式.");
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD, WIFI_AP_CHANNEL, 0, WIFI_AP_MAX_CONNECTIONS);
    IPAddress apIP = WiFi.softAPIP();
    P_PRINTF("[WIFI] AP模式已启动. SSID: %s, IP: %s\n", WIFI_AP_SSID, apIP.toString().c_str());

    if (config.currentSsidForSettings.length() > 0) {
        P_PRINTF("[WIFI] 检测到保存的SSID: %s, 将在后台尝试连接...\n", config.currentSsidForSettings.c_str());
        wifiStatus.ssidToTry = config.currentSsidForSettings;
        wifiStatus.passwordToTry = ""; 
        wifiStatus.connectInitiatorClientNum = 255; 
        wifiStatus.connectProgress = WIFI_CP_DISCONNECTING;
        wifiStatus.connectAttemptStartTime = millis();
        WiFi.disconnect(false); 
    } else {
        P_PRINTLN("[WIFI] 没有已保存的STA SSID可供自动连接.");
    }
}

void processWiFiConnection(WifiState& wifiStatus, DeviceConfig& config) {
    if (wifiStatus.connectProgress == WIFI_CP_IDLE || wifiStatus.connectProgress == WIFI_CP_FAILED) {
        return;
    }

    DynamicJsonDocument responseDoc(256);
    responseDoc["type"] = "connectWifiStatus";

    if (wifiStatus.connectProgress == WIFI_CP_DISCONNECTING) {
        if (WiFi.status() == WL_DISCONNECTED || millis() - wifiStatus.connectAttemptStartTime > 2000) { 
            P_PRINTF("[WIFI_PROC] 已断开或超时. 尝试连接到 %s\n", wifiStatus.ssidToTry.c_str());
            WiFi.begin(wifiStatus.ssidToTry.c_str(), wifiStatus.passwordToTry.c_str());
            wifiStatus.connectProgress = WIFI_CP_CONNECTING;
            wifiStatus.connectAttemptStartTime = millis(); 
        }
        return;
    }

    if (wifiStatus.connectProgress == WIFI_CP_CONNECTING) {
        wl_status_t status = WiFi.status();
        if (status == WL_CONNECTED) {
            config.currentSsidForSettings = wifiStatus.ssidToTry; 
            saveConfig(config); 
            P_PRINTF("[WIFI_PROC] 连接成功: SSID=%s, IP=%s\n", wifiStatus.ssidToTry.c_str(), WiFi.localIP().toString().c_str());
            responseDoc["success"] = true;
            responseDoc["message"] = "WiFi connected successfully to " + wifiStatus.ssidToTry;
            responseDoc["ip"] = WiFi.localIP().toString();
            wifiStatus.connectProgress = WIFI_CP_IDLE;
            
            ntpInitialAttempts = 0; // WiFi连接成功，重置NTP尝试计数
            lastNtpAttemptTime = 0;   // 允许立即在loop中尝试NTP同步

        } else if (millis() - wifiStatus.connectAttemptStartTime > 20000) { 
            P_PRINTF("[WIFI_PROC] 连接超时: SSID=%s. WiFi Status: %d\n", wifiStatus.ssidToTry.c_str(), status);
            WiFi.disconnect(false); 
            responseDoc["success"] = false;
            responseDoc["message"] = "Failed to connect to " + wifiStatus.ssidToTry + " (Timeout, Status: " + String(status) + ")";
            wifiStatus.connectProgress = WIFI_CP_FAILED;
        } else if (status == WL_NO_SSID_AVAIL || status == WL_CONNECT_FAILED || status == WL_CONNECTION_LOST || status == WL_IDLE_STATUS ) {
            P_PRINTF("[WIFI_PROC] 连接失败: SSID=%s. WiFi Status: %d\n", wifiStatus.ssidToTry.c_str(), status);
            WiFi.disconnect(false); 
            responseDoc["success"] = false;
            responseDoc["message"] = "Failed to connect to " + wifiStatus.ssidToTry + " (Error, Status: " + String(status) + ")";
            wifiStatus.connectProgress = WIFI_CP_FAILED;
        } else {
            return;
        }

        if (wifiStatus.connectInitiatorClientNum != 255 && wifiStatus.connectInitiatorClientNum < webSocket.connectedClients()) {
            String responseStr;
            serializeJson(responseDoc, responseStr);
            webSocket.sendTXT(wifiStatus.connectInitiatorClientNum, responseStr);
        }
        sendWifiStatusToClients(wifiStatus); 
        wifiStatus.connectInitiatorClientNum = 255; 
    }
}

void startWifiScan(uint8_t clientNum, WifiState& wifiStatus, JsonDocument& responseDoc) {
    if (wifiStatus.isScanning) {
        P_PRINTLN("[WIFI_SCAN] WiFi扫描已在进行中.");
        responseDoc["type"] = "wifiScanResults";
        responseDoc["error"] = "Scan already in progress.";
        responseDoc.createNestedArray("networks"); 
    } else {
        P_PRINTLN("[WIFI_SCAN] 请求异步扫描WiFi...");
        if (WiFi.scanNetworks(true, true, false, 300, 0) == WIFI_SCAN_RUNNING) {
            wifiStatus.isScanning = true;
            wifiStatus.scanRequesterClientNum = clientNum;
            wifiStatus.scanStartTime = millis();
            responseDoc["type"] = "scanStatus";
            responseDoc["message"] = "WiFi scan initiated...";
            P_PRINTLN("[WIFI_SCAN] 异步扫描已启动.");
        } else {
            P_PRINTLN("[WIFI_SCAN] 启动异步扫描失败.");
            responseDoc["type"] = "wifiScanResults";
            responseDoc["error"] = "Failed to start WiFi scan.";
            responseDoc.createNestedArray("networks");
        }
    }
}

void processWifiScanResults(WifiState& wifiStatus) {
    if (!wifiStatus.isScanning) {
        return;
    }

    int8_t scanResult = WiFi.scanComplete();
    const unsigned long WIFI_SCAN_TIMEOUT_MS = 20000; 

    if (scanResult == WIFI_SCAN_RUNNING) {
        if (millis() - wifiStatus.scanStartTime > WIFI_SCAN_TIMEOUT_MS) {
            P_PRINTLN("[WIFI_SCAN_PROC] 扫描超时!");
            WiFi.scanDelete();
            wifiStatus.isScanning = false;
            DynamicJsonDocument scanTimeoutDoc(128);
            scanTimeoutDoc["type"] = "wifiScanResults";
            scanTimeoutDoc["error"] = "Scan timed out.";
            scanTimeoutDoc.createNestedArray("networks");
            String timeoutStr;
            serializeJson(scanTimeoutDoc, timeoutStr);
            if (wifiStatus.scanRequesterClientNum != 255 && wifiStatus.scanRequesterClientNum < webSocket.connectedClients()) {
                webSocket.sendTXT(wifiStatus.scanRequesterClientNum, timeoutStr);
            }
            wifiStatus.scanRequesterClientNum = 255;
        }
        return;
    }

    P_PRINTF("[WIFI_SCAN_PROC] 异步扫描完成. 结果: %d\n", scanResult);
    wifiStatus.isScanning = false;

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
    } else if (scanResult == WIFI_SCAN_FAILED) {
         doc["error"] = "Scan failed.";
         P_PRINTLN("[WIFI_SCAN_PROC] WiFi扫描失败.");
    } else {
         P_PRINTLN("[WIFI_SCAN_PROC] 未发现WiFi网络.");
    }

    String responseStr;
    serializeJson(doc, responseStr);
    if (wifiStatus.scanRequesterClientNum != 255 && wifiStatus.scanRequesterClientNum < webSocket.connectedClients()) {
        webSocket.sendTXT(wifiStatus.scanRequesterClientNum, responseStr);
    }
    WiFi.scanDelete();
    wifiStatus.scanRequesterClientNum = 255;
}

void readSensors(DeviceState& state) {
    float newTemp = dht.readTemperature();
    float newHum = dht.readHumidity();
    if (isnan(newTemp) || isnan(newHum)) {
        state.tempStatus = SS_DISCONNECTED;
        state.humStatus = SS_DISCONNECTED;
    } else {
        state.temperature = newTemp;
        state.humidity = newHum;
        if(state.tempStatus == SS_INIT || state.tempStatus == SS_DISCONNECTED) state.tempStatus = SS_NORMAL;
        if(state.humStatus == SS_INIT || state.humStatus == SS_DISCONNECTED) state.humStatus = SS_NORMAL;
    }

    bool isGasSensorPhysicallyWarmingUp = (millis() < gasSensorWarmupEndTime);

    if (isGasSensorPhysicallyWarmingUp) {
        state.gasCoStatus = SS_INIT; state.gasNo2Status = SS_INIT;
        state.gasC2h5ohStatus = SS_INIT; state.gasVocStatus = SS_INIT;
        state.gasValues = {NAN, NAN, NAN, NAN}; 
        P_PRINTLN("[SENSOR] 气体传感器物理预热中...");
    } else {
        uint32_t raw_co = gas_sensor.measure_CO();
        uint32_t raw_no2 = gas_sensor.measure_NO2();
        uint32_t raw_c2h5oh = gas_sensor.measure_C2H5OH();
        uint32_t raw_voc = gas_sensor.measure_VOC();

        state.gasValues.co = (float)raw_co;
        state.gasValues.no2 = (float)raw_no2;
        state.gasValues.c2h5oh = (float)raw_c2h5oh;
        state.gasValues.voc = (float)raw_voc;
        
        bool allGasZeroAfterWarmup = !isGasSensorPhysicallyWarmingUp && 
                                     raw_co == 0 && raw_no2 == 0 && 
                                     raw_c2h5oh == 0 && raw_voc == 0;
        
        bool outOfRange = raw_co > GAS_SENSOR_ADC_MAX_VALID || raw_no2 > GAS_SENSOR_ADC_MAX_VALID ||
                          raw_c2h5oh > GAS_SENSOR_ADC_MAX_VALID || raw_voc > GAS_SENSOR_ADC_MAX_VALID;

        if (allGasZeroAfterWarmup || outOfRange) {
            P_PRINTLN("[SENSOR] 气体传感器读数可能无效 (全0或超范围)，检查I2C连接。");
            state.gasCoStatus = SS_DISCONNECTED;
            state.gasNo2Status = SS_DISCONNECTED;
            state.gasC2h5ohStatus = SS_DISCONNECTED;
            state.gasVocStatus = SS_DISCONNECTED;
        } else {
            state.gasCoStatus = SS_NORMAL;
            state.gasNo2Status = SS_NORMAL;
            state.gasC2h5ohStatus = SS_NORMAL;
            state.gasVocStatus = SS_NORMAL;
        }
        
        if(state.gasCoStatus == SS_INIT && state.gasCoStatus != SS_DISCONNECTED) state.gasCoStatus = SS_NORMAL;
        if(state.gasNo2Status == SS_INIT && state.gasNo2Status != SS_DISCONNECTED) state.gasNo2Status = SS_NORMAL;
        if(state.gasC2h5ohStatus == SS_INIT && state.gasC2h5ohStatus != SS_DISCONNECTED) state.gasC2h5ohStatus = SS_NORMAL;
        if(state.gasVocStatus == SS_INIT && state.gasVocStatus != SS_DISCONNECTED) state.gasVocStatus = SS_NORMAL;
    }
}

void checkAlarms(DeviceState& state, const DeviceConfig& config) {
    bool anyAlarm = false;

    if (state.tempStatus != SS_DISCONNECTED && state.tempStatus != SS_INIT) {
        if (state.temperature < config.thresholds.tempMin || state.temperature > config.thresholds.tempMax) {
            if (state.tempStatus == SS_NORMAL) P_PRINTF("[ALARM] 温度超限! %.1f°C (范围: %.1f-%.1f)\n", state.temperature, config.thresholds.tempMin, config.thresholds.tempMax);
            state.tempStatus = SS_WARNING;
            anyAlarm = true;
        } else {
            state.tempStatus = SS_NORMAL;
        }
    }
    if (state.humStatus != SS_DISCONNECTED && state.humStatus != SS_INIT) {
        if (state.humidity < config.thresholds.humMin || state.humidity > config.thresholds.humMax) {
            if (state.humStatus == SS_NORMAL) P_PRINTF("[ALARM] 湿度超限! %.1f%% (范围: %.1f-%.1f)\n", state.humidity, config.thresholds.humMin, config.thresholds.humMax);
            state.humStatus = SS_WARNING;
            anyAlarm = true;
        } else {
            state.humStatus = SS_NORMAL;
        }
    }

    if (state.gasCoStatus != SS_DISCONNECTED && state.gasCoStatus != SS_INIT) {
        if (state.gasValues.co < config.thresholds.coMin || state.gasValues.co > config.thresholds.coMax) {
            if (state.gasCoStatus == SS_NORMAL) P_PRINTF("[ALARM] CO超限! %.0f (ADC) (范围: %.0f-%.0f)\n", state.gasValues.co, config.thresholds.coMin, config.thresholds.coMax);
            state.gasCoStatus = SS_WARNING;
            anyAlarm = true;
        } else {
            state.gasCoStatus = SS_NORMAL;
        }
    }
    if (state.gasNo2Status != SS_DISCONNECTED && state.gasNo2Status != SS_INIT) {
        if (state.gasValues.no2 < config.thresholds.no2Min || state.gasValues.no2 > config.thresholds.no2Max) {
            if (state.gasNo2Status == SS_NORMAL) P_PRINTF("[ALARM] NO2超限! %.0f (ADC) (范围: %.0f-%.0f)\n", state.gasValues.no2, config.thresholds.no2Min, config.thresholds.no2Max);
            state.gasNo2Status = SS_WARNING;
            anyAlarm = true;
        } else {
            state.gasNo2Status = SS_NORMAL;
        }
    }
    if (state.gasC2h5ohStatus != SS_DISCONNECTED && state.gasC2h5ohStatus != SS_INIT) {
        if (state.gasValues.c2h5oh < config.thresholds.c2h5ohMin || state.gasValues.c2h5oh > config.thresholds.c2h5ohMax) {
            if (state.gasC2h5ohStatus == SS_NORMAL) P_PRINTF("[ALARM] C2H5OH超限! %.0f (ADC) (范围: %.0f-%.0f)\n", state.gasValues.c2h5oh, config.thresholds.c2h5ohMin, config.thresholds.c2h5ohMax);
            state.gasC2h5ohStatus = SS_WARNING;
            anyAlarm = true;
        } else {
            state.gasC2h5ohStatus = SS_NORMAL;
        }
    }
    if (state.gasVocStatus != SS_DISCONNECTED && state.gasVocStatus != SS_INIT) {
        if (state.gasValues.voc < config.thresholds.vocMin || state.gasValues.voc > config.thresholds.vocMax) {
            if (state.gasVocStatus == SS_NORMAL) P_PRINTF("[ALARM] VOC超限! %.0f (ADC) (范围: %.0f-%.0f)\n", state.gasValues.voc, config.thresholds.vocMin, config.thresholds.vocMax);
            state.gasVocStatus = SS_WARNING;
            anyAlarm = true;
        } else {
            state.gasVocStatus = SS_NORMAL;
        }
    }

    if (anyAlarm) {
        if (!state.buzzerShouldBeActive) {
            state.buzzerShouldBeActive = true;
            state.buzzerBeepCount = 0; 
            P_PRINTLN("[ALARM] 蜂鸣器激活!");
        }
    } else {
        if (state.buzzerShouldBeActive) {
            state.buzzerShouldBeActive = false;
            digitalWrite(BUZZER_PIN, LOW); 
            state.buzzerBeepCount = 0;
            P_PRINTLN("[ALARM] 报警解除, 蜂鸣器停止.");
        }
    }
}

void updateLedStatus(const DeviceState& state, const WifiState& wifiStatus, const Adafruit_NeoPixel& led) {
    unsigned long currentTime = millis();
    uint32_t colorToSet = COLOR_OFF_VAL;
    bool isAnySensorDisconnected = (state.tempStatus == SS_DISCONNECTED || state.humStatus == SS_DISCONNECTED ||
                                  state.gasCoStatus == SS_DISCONNECTED || state.gasNo2Status == SS_DISCONNECTED ||
                                  state.gasC2h5ohStatus == SS_DISCONNECTED || state.gasVocStatus == SS_DISCONNECTED);
    bool isAnySensorWarning = (state.tempStatus == SS_WARNING || state.humStatus == SS_WARNING ||
                               state.gasCoStatus == SS_WARNING || state.gasNo2Status == SS_WARNING ||
                               state.gasC2h5ohStatus == SS_WARNING || state.gasVocStatus == SS_WARNING);
    bool isAnySensorInitializing = (state.tempStatus == SS_INIT || state.humStatus == SS_INIT ||
                                   state.gasCoStatus == SS_INIT || state.gasNo2Status == SS_INIT ||
                                   state.gasC2h5ohStatus == SS_INIT || state.gasVocStatus == SS_INIT);

    DeviceState& mutableState = const_cast<DeviceState&>(state); 

    if (wifiStatus.isScanning) { 
        if (currentTime - mutableState.lastBlinkTime >= 200) { 
            mutableState.lastBlinkTime = currentTime; 
            mutableState.ledBlinkState = !mutableState.ledBlinkState;
        }
        colorToSet = mutableState.ledBlinkState ? COLOR_BLUE_VAL : pixels.Color(0,0,30); 
    } else if (isAnySensorInitializing) { 
        if (currentTime - mutableState.lastBlinkTime >= 300) {
            mutableState.lastBlinkTime = currentTime;
            mutableState.ledBlinkState = !mutableState.ledBlinkState;
        }
        colorToSet = mutableState.ledBlinkState ? COLOR_ORANGE_VAL : pixels.Color(100,60,0); 
    } else if (isAnySensorDisconnected) { 
        if (currentTime - mutableState.lastBlinkTime >= 500) {
            mutableState.lastBlinkTime = currentTime;
            mutableState.ledBlinkState = !mutableState.ledBlinkState;
        }
        colorToSet = mutableState.ledBlinkState ? COLOR_BLUE_VAL : COLOR_OFF_VAL; 
    } else if (isAnySensorWarning) { 
        colorToSet = COLOR_RED_VAL; 
    } else if (wifiStatus.connectProgress == WIFI_CP_CONNECTING || wifiStatus.connectProgress == WIFI_CP_DISCONNECTING) { 
         if (currentTime - mutableState.lastBlinkTime >= 300) {
            mutableState.lastBlinkTime = currentTime;
            mutableState.ledBlinkState = !mutableState.ledBlinkState;
        }
        colorToSet = mutableState.ledBlinkState ? COLOR_BLUE_VAL : pixels.Color(0,0,50); 
    } else if (!WiFi.isConnected() && (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA)) { 
        colorToSet = COLOR_YELLOW_VAL; 
    } else { 
        colorToSet = COLOR_GREEN_VAL; 
    }
    
    if (pixels.getPixelColor(0) != colorToSet) { 
        pixels.setPixelColor(0, colorToSet);
        pixels.show();
    }
}

void controlBuzzer(DeviceState& state) {
    unsigned long currentTime = millis();
    if (state.buzzerShouldBeActive) {
        if (state.buzzerBeepCount < BUZZER_ALARM_COUNT) {
            if (currentTime >= state.buzzerStopTime) {
                if (digitalRead(BUZZER_PIN) == HIGH) { 
                    digitalWrite(BUZZER_PIN, LOW);
                    state.buzzerStopTime = currentTime + BUZZER_ALARM_INTERVAL; 
                } else { 
                    digitalWrite(BUZZER_PIN, HIGH);
                    state.buzzerStopTime = currentTime + BUZZER_ALARM_DURATION; 
                    state.buzzerBeepCount++;
                }
            }
        } else { 
            digitalWrite(BUZZER_PIN, LOW); 
        }
    } else { 
        if (digitalRead(BUZZER_PIN) == HIGH) { 
            digitalWrite(BUZZER_PIN, LOW);
        }
        state.buzzerBeepCount = 0; 
        state.buzzerStopTime = 0;  
    }
}

void onWebSocketEvent(uint8_t clientNum, WStype_t type, uint8_t * payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            P_PRINTF("[%u] WebSocket已断开连接!\n", clientNum);
            if (wifiState.isScanning && wifiState.scanRequesterClientNum == clientNum) {
                P_PRINTLN("[WIFI_SCAN] 请求扫描的客户端已断开，取消扫描结果发送。");
                wifiState.scanRequesterClientNum = 255; 
            }
            break;
        case WStype_CONNECTED: {
            IPAddress ip = webSocket.remoteIP(clientNum);
            P_PRINTF("[%u] WebSocket已连接, IP: %s\n", clientNum, ip.toString().c_str());
            sendWifiStatusToClients(wifiState, clientNum);
            sendSensorDataToClients(currentState, clientNum);
            sendHistoricalDataToClient(clientNum, historicalData);
            sendCurrentSettingsToClient(clientNum, currentConfig);
            break;
        }
        case WStype_TEXT: {
            P_PRINTF("[%u] WS收到文本: %s\n", clientNum, (char *)payload);
            DynamicJsonDocument doc(1024); 
            DeserializationError error = deserializeJson(doc, payload, length);
            
            DynamicJsonDocument responseDoc(512); 

            if (error) {
                P_PRINTF("[%u] WS JSON解析失败: %s\n", clientNum, error.c_str());
                responseDoc["type"] = "error";
                responseDoc["message"] = "Invalid JSON payload.";
            } else {
                handleWebSocketMessage(clientNum, doc, responseDoc);
            }
            
            if (responseDoc.size() > 0) {
                String responseStr;
                serializeJson(responseDoc, responseStr);
                webSocket.sendTXT(clientNum, responseStr);
            }
            break;
        }
        case WStype_BIN: P_PRINTF("[%u] 收到二进制数据,长度: %u\n", clientNum, length); break;
        case WStype_PONG: break;
        case WStype_PING: break;
        default: P_PRINTF("[%u] 未处理的WS事件类型: %d\n", clientNum, type); break;
    }
}

void setupWebSocketActions() {
    wsActionHandlers["getCurrentSettings"] = handleGetCurrentSettingsRequest;
    wsActionHandlers["getHistoricalData"] = handleGetHistoricalDataRequest;
    wsActionHandlers["saveThresholds"] = handleSaveThresholdsRequest;
    wsActionHandlers["saveLedBrightness"] = handleSaveLedBrightnessRequest;
    wsActionHandlers["scanWifi"] = handleScanWifiRequest;
    wsActionHandlers["connectWifi"] = handleConnectWifiRequest;
    wsActionHandlers["resetSettings"] = handleResetSettingsRequest;
}

void handleWebSocketMessage(uint8_t clientNum, const JsonDocument& doc, JsonDocument& responseDoc) {
    const char* action = doc["action"];
    if (!action) {
        P_PRINTLN("[%u] WS消息缺少 'action'.");
        responseDoc["type"] = "error";
        responseDoc["message"] = "Missing 'action' field.";
        return;
    }
    P_PRINTF("[%u] WS action: %s\n", clientNum, action);

    String actionStr = String(action);
    auto it = wsActionHandlers.find(actionStr);

    if (it != wsActionHandlers.end()) {
        it->second(clientNum, doc, responseDoc);
    } else {
        P_PRINTF("[%u] 未知WS action: %s\n", clientNum, action);
        responseDoc["type"] = "error";
        responseDoc["message"] = "Unknown action: " + actionStr;
    }
}

void handleGetCurrentSettingsRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response) {
    sendCurrentSettingsToClient(clientNum, currentConfig);
}

void handleGetHistoricalDataRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response) {
    sendHistoricalDataToClient(clientNum, historicalData);
}

void handleSaveThresholdsRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response) {
    currentConfig.thresholds.tempMin = request["tempMin"] | currentConfig.thresholds.tempMin;
    currentConfig.thresholds.tempMax = request["tempMax"] | currentConfig.thresholds.tempMax;
    currentConfig.thresholds.humMin  = request["humMin"]  | currentConfig.thresholds.humMin;
    currentConfig.thresholds.humMax  = request["humMax"]  | currentConfig.thresholds.humMax;
    currentConfig.thresholds.coMin   = request["coMin"]   | currentConfig.thresholds.coMin;
    currentConfig.thresholds.coMax   = request["coMax"]   | currentConfig.thresholds.coMax;
    currentConfig.thresholds.no2Min  = request["no2Min"]  | currentConfig.thresholds.no2Min;
    currentConfig.thresholds.no2Max  = request["no2Max"]  | currentConfig.thresholds.no2Max;
    currentConfig.thresholds.c2h5ohMin = request["c2h5ohMin"] | currentConfig.thresholds.c2h5ohMin;
    currentConfig.thresholds.c2h5ohMax = request["c2h5ohMax"] | currentConfig.thresholds.c2h5ohMax;
    currentConfig.thresholds.vocMin  = request["vocMin"]  | currentConfig.thresholds.vocMin;
    currentConfig.thresholds.vocMax  = request["vocMax"]  | currentConfig.thresholds.vocMax;
    
    saveConfig(currentConfig);
    checkAlarms(currentState, currentConfig); 
    
    response["type"] = "saveSettingsStatus";
    response["success"] = true;
    response["message"] = "Thresholds saved.";
}

void handleSaveLedBrightnessRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response) {
    if (request.containsKey("brightness") && request["brightness"].is<int>()) {
        int brightness = request["brightness"].as<int>();
        if (brightness >= 0 && brightness <= 100) {
            currentConfig.ledBrightness = brightness;
            pixels.setBrightness(map(brightness, 0, 100, 0, 255));
            pixels.show(); 
            saveConfig(currentConfig);
            response["type"] = "saveBrightnessStatus";
            response["success"] = true;
            response["message"] = "LED brightness saved and applied.";
        } else {
            response["type"] = "saveBrightnessStatus";
            response["success"] = false;
            response["message"] = "Invalid brightness value (must be 0-100).";
        }
    } else {
        response["type"] = "saveBrightnessStatus";
        response["success"] = false;
        response["message"] = "Missing or invalid 'brightness' field.";
    }
}

void handleScanWifiRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response) {
    startWifiScan(clientNum, wifiState, response);
}

void handleConnectWifiRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response) {
    if (wifiState.connectProgress != WIFI_CP_IDLE && wifiState.connectProgress != WIFI_CP_FAILED) {
        P_PRINTLN("[WIFI_CONN] WiFi连接已在进行中.");
        response["type"] = "connectWifiStatus";
        response["success"] = false;
        response["message"] = "Connection attempt already in progress.";
    } else {
        wifiState.ssidToTry = request["ssid"].as<String>();
        wifiState.passwordToTry = request["password"].as<String>();
        P_PRINTF("[WIFI_CONN] 收到连接请求: SSID=%s\n", wifiState.ssidToTry.c_str());
        if (wifiState.ssidToTry.length() == 0) {
            response["type"] = "connectWifiStatus";
            response["success"] = false;
            response["message"] = "SSID cannot be empty.";
        } else {
            wifiState.connectInitiatorClientNum = clientNum;
            wifiState.connectProgress = WIFI_CP_DISCONNECTING;
            wifiState.connectAttemptStartTime = millis();
            WiFi.disconnect(false); 
            response["type"] = "connectWifiStatus"; 
            response["success"] = false; 
            response["message"] = "Initiating connection to " + wifiState.ssidToTry + "...";
        }
    }
}

void handleResetSettingsRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response) {
    P_PRINTLN("[RESET] 收到恢复出厂设置请求.");
    resetAllSettingsToDefault(currentConfig);
    saveConfig(currentConfig);
    historicalData.clear();
    saveHistoricalDataToFile(historicalData);
    
    response["type"] = "resetStatus";
    response["success"] = true;
    response["message"] = "Settings reset. Device will restart.";
    
    String respStr;
    serializeJson(response, respStr);
    webSocket.sendTXT(clientNum, respStr);
    
    P_PRINTLN("[RESET] 设置已重置, 准备重启...");
    delay(1000);
    ESP.restart();
}

void sendSensorDataToClients(const DeviceState& state, uint8_t specificClientNum) {
    DynamicJsonDocument doc(1024); 
    doc["type"] = "sensorData";
    if (isnan(state.temperature)) doc["temperature"] = nullptr; else doc["temperature"] = state.temperature;
    if (isnan(state.humidity)) doc["humidity"] = nullptr; else doc["humidity"] = state.humidity;
    
    if (isnan(state.gasValues.co)) doc["co"] = nullptr; else doc["co"] = state.gasValues.co;
    if (isnan(state.gasValues.no2)) doc["no2"] = nullptr; else doc["no2"] = state.gasValues.no2;
    if (isnan(state.gasValues.c2h5oh)) doc["c2h5oh"] = nullptr; else doc["c2h5oh"] = state.gasValues.c2h5oh;
    if (isnan(state.gasValues.voc)) doc["voc"] = nullptr; else doc["voc"] = state.gasValues.voc;

    doc["tempStatus"] = getSensorStatusString(state.tempStatus);
    doc["humStatus"]  = getSensorStatusString(state.humStatus);
    doc["gasCoStatus"] = getSensorStatusString(state.gasCoStatus);
    doc["gasNo2Status"] = getSensorStatusString(state.gasNo2Status);
    doc["gasC2h5ohStatus"] = getSensorStatusString(state.gasC2h5ohStatus);
    doc["gasVocStatus"] = getSensorStatusString(state.gasVocStatus);
    
    String jsonString;
    serializeJson(doc, jsonString);
    if (specificClientNum != 255 && specificClientNum < webSocket.connectedClients()) webSocket.sendTXT(specificClientNum, jsonString);
    else webSocket.broadcastTXT(jsonString);
}

void sendWifiStatusToClients(const WifiState& wifiStatus, uint8_t specificClientNum) {
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
    doc["connecting_attempt_ssid"] = (wifiStatus.connectProgress == WIFI_CP_CONNECTING || wifiStatus.connectProgress == WIFI_CP_DISCONNECTING) ? wifiStatus.ssidToTry : "";
    doc["connection_failed"] = (wifiStatus.connectProgress == WIFI_CP_FAILED);

    String jsonString;
    serializeJson(doc, jsonString);
    if (specificClientNum != 255 && specificClientNum < webSocket.connectedClients()) webSocket.sendTXT(specificClientNum, jsonString);
    else webSocket.broadcastTXT(jsonString);
}

void sendHistoricalDataToClient(uint8_t clientNum, const CircularBuffer& histBuffer) {
    if (clientNum >= webSocket.connectedClients()) return;
    const std::vector<SensorDataPoint>& dataToSend = histBuffer.getData();
    P_PRINTF("[HISTORY] 发送历史数据给客户端 %u (%u 条)\n", clientNum, dataToSend.size());
    
    DynamicJsonDocument doc(min((size_t)(1024 * 16), (size_t)(JSON_ARRAY_SIZE(dataToSend.size()) + dataToSend.size() * JSON_OBJECT_SIZE(8)))); 
    doc["type"] = "historicalData";
    JsonArray historyArr = doc.createNestedArray("history");

    for (const auto& dp : dataToSend) {
        JsonObject dataPoint = historyArr.createNestedObject();
        dataPoint["time"] = dp.timeStr; 
        dataPoint["temp"] = dp.temp; 
        dataPoint["hum"] = dp.hum; 
        dataPoint["co"] = dp.gas.co;
        dataPoint["no2"] = dp.gas.no2;
        dataPoint["c2h5oh"] = dp.gas.c2h5oh;
        dataPoint["voc"] = dp.gas.voc;
    }
    String jsonString;
    if (serializeJson(doc, jsonString) > 0) {
        webSocket.sendTXT(clientNum, jsonString);
    } else {
        P_PRINTLN("[HISTORY] 序列化历史数据失败 (可能JSON过大).");
        DynamicJsonDocument errDoc(128); 
        errDoc["type"] = "historicalData"; 
        errDoc["error"] = "Failed to serialize history (too large).";
        errDoc.createNestedArray("history");
        String errStr; serializeJson(errDoc, errStr); webSocket.sendTXT(clientNum, errStr);
    }
}

void sendCurrentSettingsToClient(uint8_t clientNum, const DeviceConfig& config) {
    if (clientNum >= webSocket.connectedClients()) return;
    P_PRINTF("[SETTINGS] 发送当前设置给客户端 %u\n", clientNum);
    DynamicJsonDocument doc(2048); 
    doc["type"] = "settingsData";
    JsonObject settingsObj = doc.createNestedObject("settings");
    JsonObject thresholdsObj = settingsObj.createNestedObject("thresholds");
    thresholdsObj["tempMin"] = config.thresholds.tempMin; 
    thresholdsObj["tempMax"] = config.thresholds.tempMax;
    thresholdsObj["humMin"] = config.thresholds.humMin;   
    thresholdsObj["humMax"] = config.thresholds.humMax;
    thresholdsObj["coMin"] = config.thresholds.coMin;     
    thresholdsObj["coMax"] = config.thresholds.coMax;
    thresholdsObj["no2Min"] = config.thresholds.no2Min;   
    thresholdsObj["no2Max"] = config.thresholds.no2Max;
    thresholdsObj["c2h5ohMin"] = config.thresholds.c2h5ohMin; 
    thresholdsObj["c2h5ohMax"] = config.thresholds.c2h5ohMax;
    thresholdsObj["vocMin"] = config.thresholds.vocMin;   
    thresholdsObj["vocMax"] = config.thresholds.vocMax;
    
    settingsObj["currentSSID"] = WiFi.isConnected() ? WiFi.SSID() : config.currentSsidForSettings;
    settingsObj["ledBrightness"] = config.ledBrightness;
    
    String jsonString;
    serializeJson(doc, jsonString);
    webSocket.sendTXT(clientNum, jsonString);
}

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

void resetAllSettingsToDefault(DeviceConfig& config) {
    P_PRINTLN("[CONFIG] 重置所有设置为默认值 (内存中).");
    config.thresholds = {
        DEFAULT_TEMP_MIN, DEFAULT_TEMP_MAX,
        DEFAULT_HUM_MIN, DEFAULT_HUM_MAX,
        DEFAULT_CO_MIN, DEFAULT_CO_MAX,
        DEFAULT_NO2_MIN, DEFAULT_NO2_MAX,
        DEFAULT_C2H5OH_MIN, DEFAULT_C2H5OH_MAX,
        DEFAULT_VOC_MIN, DEFAULT_VOC_MAX
    };
    config.currentSsidForSettings = "";
    config.ledBrightness = DEFAULT_LED_BRIGHTNESS;
    pixels.setBrightness(map(config.ledBrightness, 0, 100, 0, 255)); 
    pixels.show();
}

void generateTimeStr(unsigned long current_ts, char* buffer) {
    time_t now = current_ts;
    struct tm * p_tm = localtime(&now);
    if (p_tm) { 
        sprintf(buffer, "%02d:%02d:%02d", p_tm->tm_hour, p_tm->tm_min, p_tm->tm_sec);
    } else {
        strcpy(buffer, "00:00:00"); 
    }
}

void addHistoricalDataPoint(CircularBuffer& histBuffer, const DeviceState& state) {
    if (isnan(state.temperature) || isnan(state.humidity) || 
        isnan(state.gasValues.co) ) return; 

    SensorDataPoint dp;
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    dp.timestamp = tv.tv_sec;

    dp.temp = state.temperature; 
    dp.hum = state.humidity; 
    dp.gas = state.gasValues;
    generateTimeStr(dp.timestamp, dp.timeStr);
    
    histBuffer.add(dp);
}

String getSensorStatusString(SensorStatusVal status) {
    switch (status) {
        case SS_NORMAL: return "normal";
        case SS_WARNING: return "warning";
        case SS_DISCONNECTED: return "disconnected";
        case SS_INIT: return "initializing";
        default: return "unknown";
    }
}

