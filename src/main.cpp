// =================================================================================
// == ESP32 温湿度及多通道气体监测器 V4 - 主文件 (main.cpp) ==
// == 职责: 程序入口，初始化并驱动各个模块运行。
// =================================================================================

#include <Arduino.h>
#include "config.h"
#include "data_manager.h"
#include "sensor_handler.h"
#include "web_handler.h"

// ==========================================================================
// == Arduino `setup()` 函数 ==
// ==========================================================================
void setup() {
    Serial.begin(115200);
    P_PRINTLN("\n[SETUP] 系统启动中 (V4 - Refactored)...");

    // 初始化硬件
    initHardware();

    // 初始化文件系统
    initSPIFFS();

    // 加载配置和历史数据
    loadConfig(currentConfig);
    loadHistoricalDataFromFile(historicalData);

    // 根据加载的配置更新硬件状态
    updateLedBrightness(currentConfig.ledBrightness);

    // 初始化网络服务 (WiFi, DNS, Web Server, WebSocket)
    initWiFiAndWebServer(currentConfig, wifiState);

    // 设置气体传感器预热结束时间
    gasSensorWarmupEndTime = millis() + GAS_SENSOR_WARMUP_PERIOD_MS;
    
    P_PRINTLN("[SETUP] 初始化完成, 系统运行中.");
}

// ==========================================================================
// == Arduino `loop()` 函数 ==
// ==========================================================================
void loop() {
    // 处理网络相关任务
    network_loop();

    // 获取当前时间
    unsigned long currentTime = millis();

    // 周期性地执行传感器读取、警报检查和数据记录
    if (currentTime - lastSensorReadTime >= SENSOR_READ_INTERVAL_MS) {
        lastSensorReadTime = currentTime;
        readSensors(currentState);
        checkAlarms(currentState, currentConfig);
        addHistoricalDataPoint(historicalData, currentState);
    }

    // 更新LED和蜂鸣器状态
    updateLedStatus(currentState, wifiState);
    controlBuzzer(currentState);

    // 周期性地通过WebSocket广播数据
    if (currentTime - lastWebSocketUpdateTime >= WEBSOCKET_UPDATE_INTERVAL_MS) {
        lastWebSocketUpdateTime = currentTime;
        // 仅在非连接/扫描状态下广播，避免干扰
        if ((wifiState.connectProgress == WIFI_CP_IDLE || wifiState.connectProgress == WIFI_CP_FAILED) && !wifiState.isScanning) {
            sendSensorDataToClients(currentState);
            sendWifiStatusToClients(wifiState);
        }
    }

    // 周期性地将历史数据保存到闪存
    if (currentTime - lastHistoricalDataSaveTime >= HISTORICAL_DATA_SAVE_INTERVAL_MS) {
        lastHistoricalDataSaveTime = currentTime;
        saveHistoricalDataToFile(historicalData);
    }
}
