#ifndef WEB_HANDLER_H
#define WEB_HANDLER_H

#include "data_manager.h"
#include <map>
#include <functional>

#include <ESPAsyncWebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>


// ==========================================================================
// == 全局变量声明 (网络相关) ==
// ==========================================================================
extern bool ntpSynced;
extern bool ntpGiveUp;
extern int ntpInitialAttempts;
extern unsigned long lastNtpAttemptTime;
extern unsigned long lastNtpSyncTime;

// 定义 WebSocket Action Handler 类型
typedef std::function<void(uint8_t, const JsonDocument&, JsonDocument&)> WebSocketActionHandler;

// ==========================================================================
// == 函数声明 ==
// ==========================================================================

// -- 初始化 --
void initWiFiAndWebServer(DeviceConfig& config, WifiState& wifiStatus);
void configureWebServer();
void setupWebSocketActions();
void handleCaptivePortal(AsyncWebServerRequest* request);
void initWebSocketTask(); // <-- 新增: 初始化WebSocket后台任务

// -- 循环处理 --
void network_loop();
void webSocketLoopTask(void* pvParameters); // <-- 新增: WebSocket后台任务函数

// -- WebSocket 事件处理 --
void onWebSocketEvent(uint8_t clientNum, WStype_t type, uint8_t * payload, size_t length);
void handleWebSocketMessage(uint8_t clientNum, const JsonDocument& doc, JsonDocument& responseDoc);

// -- WebSocket 数据发送 --
void sendSensorDataToClients(const DeviceState& state, uint8_t specificClientNum = 255);
void sendWifiStatusToClients(const WifiState& currentWifiState, uint8_t specificClientNum = 255);
void sendHistoricalDataToClient(uint8_t clientNum, const CircularBuffer& histBuffer);
void sendCurrentSettingsToClient(uint8_t clientNum, const DeviceConfig& config);
void sendCalibrationStatusToClients(uint8_t specificClientNum = 255);


#endif // WEB_HANDLER_H
