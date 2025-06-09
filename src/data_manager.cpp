#include "data_manager.h"
#include "config.h"
#include <SPIFFS.h>
#include <WiFi.h> // <-- 新增: 为访问 WiFi.SSID() 等函数
#include <time.h> // <-- 新增: 为 generateTimeStr 中的时间函数

// ==========================================================================
// == 全局变量定义 ==
// ==========================================================================
DeviceState currentState;
DeviceConfig currentConfig;
WifiState wifiState;
CircularBuffer historicalData(HISTORICAL_DATA_POINTS);

unsigned long lastSensorReadTime = 0;
unsigned long lastWebSocketUpdateTime = 0;
unsigned long lastHistoricalDataSaveTime = 0;
unsigned long gasSensorWarmupEndTime = 0;

// ==========================================================================
// == 构造函数实现 ==
// ==========================================================================

DeviceState::DeviceState() : 
    temperature(NAN), humidity(NAN),
    tempStatus(SS_INIT), humStatus(SS_INIT),
    gasCoStatus(SS_INIT), gasNo2Status(SS_INIT),
    gasC2h5ohStatus(SS_INIT), gasVocStatus(SS_INIT),
    buzzerShouldBeActive(false), buzzerStopTime(0), buzzerBeepCount(0),
    ledBlinkState(false), lastBlinkTime(0) {
    gasPpmValues = {NAN, NAN, NAN, NAN};
}

DeviceConfig::DeviceConfig() : ledBrightness(DEFAULT_LED_BRIGHTNESS) {
    thresholds = {
        DEFAULT_TEMP_MIN, DEFAULT_TEMP_MAX,
        DEFAULT_HUM_MIN, DEFAULT_HUM_MAX,
        DEFAULT_CO_PPM_MAX,
        DEFAULT_NO2_PPM_MAX,
        DEFAULT_C2H5OH_PPM_MAX,
        DEFAULT_VOC_PPM_MAX
    };
}

WifiState::WifiState() : 
    connectProgress(WIFI_CP_IDLE), connectAttemptStartTime(0),
    connectInitiatorClientNum(255), isScanning(false),
    scanRequesterClientNum(255), scanStartTime(0) {}


CircularBuffer::CircularBuffer(size_t size) : maxSize(size), head(0), tail(0), full(false) {
    buffer.resize(size);
}

// ==========================================================================
// == 函数实现 ==
// ==========================================================================

void initSPIFFS() {
    if (!SPIFFS.begin(true)) {
        P_PRINTLN("[SPIFFS] 初始化失败! 数据可能无法保存或加载.");
    } else {
        P_PRINTLN("[SPIFFS] 文件系统已挂载.");
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
                config.thresholds.coPpmMax   = thresholdsObj["coPpmMax"]   | DEFAULT_CO_PPM_MAX;
                config.thresholds.no2PpmMax  = thresholdsObj["no2PpmMax"]  | DEFAULT_NO2_PPM_MAX;
                config.thresholds.c2h5ohPpmMax = thresholdsObj["c2h5ohPpmMax"] | DEFAULT_C2H5OH_PPM_MAX;
                config.thresholds.vocPpmMax  = thresholdsObj["vocPpmMax"]  | DEFAULT_VOC_PPM_MAX;
                
                config.currentSsidForSettings = doc["wifi"]["ssid"].as<String>();
                config.currentPasswordForSettings = doc["wifi"]["password"].as<String>();
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
    P_PRINTF("  气体(PPM) - CO: %.2f, NO2: %.2f, C2H5OH: %.2f, VOC: %.2f\n",
                   config.thresholds.coPpmMax, config.thresholds.no2PpmMax,
                   config.thresholds.c2h5ohPpmMax, config.thresholds.vocPpmMax);
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
        thresholdsObj["coPpmMax"]   = config.thresholds.coPpmMax;
        thresholdsObj["no2PpmMax"]  = config.thresholds.no2PpmMax;
        thresholdsObj["c2h5ohPpmMax"] = config.thresholds.c2h5ohPpmMax;
        thresholdsObj["vocPpmMax"]  = config.thresholds.vocPpmMax;

        JsonObject wifiObj = doc.createNestedObject("wifi");
        if (WiFi.isConnected()) {
            wifiObj["ssid"] = WiFi.SSID();
            if (config.currentSsidForSettings == WiFi.SSID()) {
                 wifiObj["password"] = config.currentPasswordForSettings;
            } else {
                wifiObj["password"] = config.currentPasswordForSettings;
            }
        } else {
            wifiObj["ssid"] = config.currentSsidForSettings;
            wifiObj["password"] = config.currentPasswordForSettings;
        }

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


void resetAllSettingsToDefault(DeviceConfig& config) {
    P_PRINTLN("[CONFIG] 重置所有设置为默认值 (内存中).");
    config.thresholds = {
        DEFAULT_TEMP_MIN, DEFAULT_TEMP_MAX,
        DEFAULT_HUM_MIN, DEFAULT_HUM_MAX,
        DEFAULT_CO_PPM_MAX,
        DEFAULT_NO2_PPM_MAX,
        DEFAULT_C2H5OH_PPM_MAX,
        DEFAULT_VOC_PPM_MAX
    };
    config.currentSsidForSettings = "";
    config.currentPasswordForSettings = "";
    config.ledBrightness = DEFAULT_LED_BRIGHTNESS;
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
                    dp.isTimeRelative = obj["rel"] | false;
                    dp.temp = obj["t"];
                    dp.hum = obj["h"];
                    dp.gas.co = obj["co"];
                    dp.gas.no2 = obj["no2"];
                    dp.gas.c2h5oh = obj["c2h5oh"]; 
                    dp.gas.voc = obj["voc"];
                    generateTimeStr(dp.timestamp, dp.isTimeRelative, dp.timeStr); 
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
            obj["rel"] = dp.isTimeRelative;
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

// -- CircularBuffer 方法实现 --
void CircularBuffer::add(const SensorDataPoint& item) {
    buffer[head] = item;
    if (full) {
        tail = (tail + 1) % maxSize;
    }
    head = (head + 1) % maxSize;
    full = (head == tail);
}

const std::vector<SensorDataPoint>& CircularBuffer::getData() const {
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

size_t CircularBuffer::count() const {
    if (full) return maxSize;
    if (head >= tail) return head - tail;
    return maxSize - (tail - head);
}

bool CircularBuffer::isEmpty() const {
    return !full && (head == tail);
}

void CircularBuffer::clear() {
    head = 0;
    tail = 0;
    full = false;
    orderedData.clear();
}


// -- 数据处理函数 --
void addHistoricalDataPoint(CircularBuffer& histBuffer, const DeviceState& state) {
    if (isnan(state.temperature) && isnan(state.humidity) && isnan(state.gasPpmValues.co)) return;
    SensorDataPoint dp;
    // `ntpSynced` is declared in web_handler.h/cpp, so we need to use extern
    extern bool ntpSynced;
    dp.isTimeRelative = !ntpSynced;
    if (dp.isTimeRelative) {
        dp.timestamp = millis();
    } else {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        dp.timestamp = tv.tv_sec;
    }
    dp.temp = state.temperature; 
    dp.hum = state.humidity; 
    dp.gas = state.gasPpmValues;
    generateTimeStr(dp.timestamp, dp.isTimeRelative, dp.timeStr);
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

void generateTimeStr(unsigned long current_ts, bool isRelative, char* buffer) {
    extern bool ntpSynced;
    if (isRelative) {
        unsigned long seconds = current_ts / 1000;
        unsigned long days = seconds / (24 * 3600);
        seconds = seconds % (24 * 3600);
        unsigned long hours = seconds / 3600;
        seconds = seconds % 3600;
        unsigned long minutes = seconds / 60;
        seconds = seconds % 60;
        if (days > 0) {
            sprintf(buffer, "D%lu %02lu:%02lu", days, hours, minutes);
        } else {
            sprintf(buffer, "%02lu:%02lu:%02lu", hours, minutes, seconds);
        }
    } else {
        time_t now = current_ts;
        struct tm * p_tm = localtime(&now);
        if (p_tm) { 
            sprintf(buffer, "%02d:%02d:%02d", p_tm->tm_hour, p_tm->tm_min, p_tm->tm_sec);
        } else {
            strcpy(buffer, "00:00:00"); 
        }
    }
}
