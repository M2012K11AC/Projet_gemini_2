#ifndef DATA_MANAGER_H
#define DATA_MANAGER_H

#include <Arduino.h>
#include <vector>
#include <ArduinoJson.h>
#include "config.h"

// ==========================================================================
// == 数据结构定义 ==
// ==========================================================================

// 传感器状态枚举
enum SensorStatusVal { SS_NORMAL, SS_WARNING, SS_DISCONNECTED, SS_INIT };

// 新增：传感器校准状态机枚举
enum CalibrationState { CAL_IDLE, CAL_IN_PROGRESS, CAL_COMPLETED, CAL_FAILED };

// 多通道气体传感器数据结构 (存储转换后的PPM值)
struct GasPpmData {
    float co;      // 一氧化碳 (CO) - PPM
    float no2;     // 二氧化氮 (NO2) - PPM
    float c2h5oh;  // 乙醇 (C2H5OH) - PPM
    float voc;     // 挥发性有机物 (VOC) - PPM
};

// 新增：气体传感器电阻数据结构 (kOhm)
struct GasResistData {
    float co;
    float no2;
    float c2h5oh;
    float voc;
};


// 设备当前状态
struct DeviceState {
    int temperature;
    int humidity;
    GasPpmData gasPpmValues;
    GasResistData gasRsValues; // 新增: 用于存储当前测量的Rs值
    SensorStatusVal tempStatus, humStatus, gasCoStatus, gasNo2Status, gasC2h5ohStatus, gasVocStatus;
    bool buzzerShouldBeActive;
    unsigned long buzzerStopTime;
    int buzzerBeepCount;
    bool ledBlinkState;
    unsigned long lastBlinkTime;
    
    // 新增: 校准相关状态
    CalibrationState calibrationState;
    int calibrationProgress;      // 校准进度 (0-100)
    GasResistData measuredR0;     // 校准过程中测量的R0值

    DeviceState(); // 构造函数
};

// 报警阈值配置
struct AlarmThresholds {
    int tempMin, tempMax;
    int humMin, humMax;
    float coPpmMax, no2PpmMax, c2h5ohPpmMax, vocPpmMax;
};

// 设备配置 (从SPIFFS加载/保存)
struct DeviceConfig {
    AlarmThresholds thresholds;
    GasResistData r0Values; // 新增: 用于存储校准后的R0值
    String currentSsidForSettings;
    String currentPasswordForSettings;
    uint8_t ledBrightness;

    DeviceConfig(); // 构造函数
};

// WiFi 连接状态管理
enum WifiConnectProgress { WIFI_CP_IDLE, WIFI_CP_DISCONNECTING, WIFI_CP_CONNECTING, WIFI_CP_FAILED };
struct WifiState {
    WifiConnectProgress connectProgress;
    String ssidToTry;
    String passwordToTry;
    unsigned long connectAttemptStartTime;
    uint8_t connectInitiatorClientNum;
    bool isScanning;
    uint8_t scanRequesterClientNum;
    unsigned long scanStartTime;

    WifiState(); // 构造函数
};

// 历史数据点结构
struct SensorDataPoint {
    unsigned long timestamp;
    bool isTimeRelative;
    int temp, hum;
    GasPpmData gas; // 类型从 GasData 更改为 GasPpmData
    char timeStr[12]; // 用于存储格式化后的时间字符串
};

// 环形缓冲区类声明
class CircularBuffer {
public:
    CircularBuffer(size_t size);
    void add(const SensorDataPoint& item);
    const std::vector<SensorDataPoint>& getData() const;
    size_t count() const;
    bool isEmpty() const;
    void clear();

private:
    std::vector<SensorDataPoint> buffer;
    mutable std::vector<SensorDataPoint> orderedData; // 用于返回有序数据，避免重复分配
    size_t maxSize, head, tail;
    bool full;
};

// ==========================================================================
// == 全局变量声明 ==
// ==========================================================================
extern DeviceState currentState;
extern DeviceConfig currentConfig;
extern WifiState wifiState;
extern CircularBuffer historicalData;

extern unsigned long lastSensorReadTime, lastWebSocketUpdateTime, lastHistoricalDataSaveTime;
extern unsigned long gasSensorWarmupEndTime;

// 新增: FreeRTOS 任务句柄和信号量，用于控制校准流程
extern TaskHandle_t calibrationTaskHandle;
extern SemaphoreHandle_t calibrationSemaphore;


// ==========================================================================
// == 函数声明 ==
// ==========================================================================

// -- 文件和配置管理 --
void initSPIFFS();
void loadConfig(DeviceConfig& config);
void saveConfig(const DeviceConfig& config);
void resetAllSettingsToDefault(DeviceConfig& config);
void loadHistoricalDataFromFile(CircularBuffer& histBuffer);
void saveHistoricalDataToFile(const CircularBuffer& histBuffer);

// -- 数据处理 --
void addHistoricalDataPoint(CircularBuffer& histBuffer, const DeviceState& state);
String getSensorStatusString(SensorStatusVal status);
void generateTimeStr(unsigned long current_timestamp, bool isTimeRelative, char* buffer);

#endif // DATA_MANAGER_H
