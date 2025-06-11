#include "web_handler.h"
#include "data_manager.h"
#include "sensor_handler.h" 
#include "config.h"

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <WebSocketsServer.h>
#include <DNSServer.h>
#include <time.h>
#include <SPIFFS.h>

// ==========================================================================
// == 模块内部使用的全局对象和变量 ==
// ==========================================================================
static DNSServer dnsServer;
static AsyncWebServer server(80);
static WebSocketsServer webSocket(81);

// NTP状态变量
bool ntpSynced = false;
bool ntpGiveUp = false;
int ntpInitialAttempts = 0;
unsigned long lastNtpAttemptTime = 0;
unsigned long lastNtpSyncTime = 0;

// WebSocket action handlers map
static std::map<String, WebSocketActionHandler> wsActionHandlers;

// ==========================================================================
// == 函数声明 (内部使用) ==
// ==========================================================================
void handleGetCurrentSettingsRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response);
void handleGetHistoricalDataRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response);
void handleSaveThresholdsRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response);
void handleSaveLedBrightnessRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response);
void handleScanWifiRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response);
void handleConnectWifiRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response);
void handleResetSettingsRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response);
void handleStartCalibrationRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response); // 新增
void startWifiScan(uint8_t clientNum, WifiState& wifiStatus, JsonDocument& responseDoc);

// ==========================================================================
// == 函数实现 ==
// ==========================================================================

void initWiFiAndWebServer(DeviceConfig& config, WifiState& wifiStatus) {
    WiFi.mode(WIFI_AP_STA);
    P_PRINTLN("[WIFI] 设置为AP+STA模式.");
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD, WIFI_AP_CHANNEL, 0, WIFI_AP_MAX_CONNECTIONS);
    IPAddress apIP = WiFi.softAPIP();
    P_PRINTF("[WIFI] AP模式已启动. SSID: %s, IP: %s\n", WIFI_AP_SSID, apIP.toString().c_str());

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", apIP);
    P_PRINTLN("[DNS] Captive Portal DNS服务器已启动.");

    configureWebServer();
    server.begin();
    P_PRINTLN("[HTTP] HTTP服务器已启动");

    setupWebSocketActions();
    webSocket.begin();
    webSocket.onEvent(onWebSocketEvent);
    P_PRINTLN("[WS] WebSocket服务器已启动");

    if (config.currentSsidForSettings.length() > 0) {
        P_PRINTF("[WIFI] 检测到保存的SSID: %s, 尝试自动连接...\n", config.currentSsidForSettings.c_str());
        wifiStatus.ssidToTry = config.currentSsidForSettings;
        wifiStatus.passwordToTry = config.currentPasswordForSettings;
        wifiStatus.connectInitiatorClientNum = 255;
        WiFi.begin(wifiStatus.ssidToTry.c_str(), wifiStatus.passwordToTry.c_str());
        wifiStatus.connectProgress = WIFI_CP_CONNECTING;
        wifiStatus.connectAttemptStartTime = millis();
    } else {
        P_PRINTLN("[WIFI] 没有已保存的STA SSID可供自动连接.");
        wifiStatus.connectProgress = WIFI_CP_IDLE;
    }

    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER1, NTP_SERVER2);
}

void configureWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(SPIFFS, "/index.html", "text/html"); });
    server.on("/settings.html", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(SPIFFS, "/settings.html", "text/html"); });
    server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(SPIFFS, "/style.css", "text/css"); });
    server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(SPIFFS, "/script.js", "application/javascript"); });
    server.on("/lang.json", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(SPIFFS, "/lang.json", "application/json"); });
    server.on("/chart.min.js", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(SPIFFS, "/chart.min.js", "application/javascript"); });
    
    server.on("/generate_204", HTTP_GET, handleCaptivePortal);
    server.on("/gen_204", HTTP_GET, handleCaptivePortal);
    server.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortal);
    server.on("/fwlink", HTTP_GET, handleCaptivePortal);
    server.on("/ncsi.txt", HTTP_GET, handleCaptivePortal);
    server.on("/connecttest.txt", HTTP_GET, handleCaptivePortal);
    server.on("/success.html", HTTP_GET, handleCaptivePortal);

    server.onNotFound(handleCaptivePortal);
}

void network_loop() {
    dnsServer.processNextRequest();
    webSocket.loop();
    processWiFiConnection(wifiState, currentConfig);
    processWifiScanResults(wifiState);

    unsigned long currentTime = millis();
    if (WiFi.isConnected() && !ntpSynced && !ntpGiveUp) {
        if (currentTime - lastNtpAttemptTime >= NTP_RETRY_DELAY_MS || lastNtpAttemptTime == 0) {
            attemptNtpSync(); 
            lastNtpAttemptTime = currentTime;
        }
    }
    if (ntpSynced && (currentTime - lastNtpSyncTime >= NTP_SYNC_INTERVAL_MS)) {
        attemptNtpSync();
    }
}

void attemptNtpSync() { 
    if (!WiFi.isConnected()) {
        P_PRINTLN("[NTP] WiFi未连接, 无法同步时间.");
        return;
    }

    if (ntpInitialAttempts >= MAX_NTP_ATTEMPTS_AFTER_WIFI && !ntpSynced) {
        P_PRINTLN("[NTP] 达到最大尝试次数, 同步失败. 将使用设备运行时间.");
        ntpGiveUp = true;
        return;
    }

    P_PRINTF("[NTP] 尝试同步时间 (尝试次数: %d/%d)...\n", ntpInitialAttempts + 1, MAX_NTP_ATTEMPTS_AFTER_WIFI);
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER1, NTP_SERVER2); 
    
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo, 5000)){ 
        P_PRINTLN("[NTP] 获取时间失败.");
        ntpInitialAttempts++;
    } else {
        char timeBuffer[80];
        strftime(timeBuffer, sizeof(timeBuffer), "%A, %B %d %Y %H:%M:%S", &timeinfo);
        P_PRINTF("[NTP] 时间已同步: %s\n", timeBuffer);
        ntpSynced = true;
        lastNtpSyncTime = millis();
        ntpGiveUp = true;
    }
}

void handleCaptivePortal(AsyncWebServerRequest *request) {
    request->redirect("/");
    P_PRINTF("[Portal] Captive portal重定向: %s\n", request->url().c_str());
}

void processWiFiConnection(WifiState& wifiStatus, DeviceConfig& config) {
    if (wifiStatus.connectProgress == WIFI_CP_IDLE || wifiStatus.connectProgress == WIFI_CP_FAILED) {
        return;
    }
    DynamicJsonDocument responseDoc(256);
    responseDoc["type"] = "connectWifiStatus";
    bool sendUpdateToClient = false;
    if (wifiStatus.connectProgress == WIFI_CP_DISCONNECTING) {
        if (WiFi.status() == WL_DISCONNECTED || millis() - wifiStatus.connectAttemptStartTime > 3000) { 
            P_PRINTF("[WIFI_PROC] 已断开或超时(WIFI_CP_DISCONNECTING). 尝试连接到 %s\n", wifiStatus.ssidToTry.c_str());
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
            config.currentPasswordForSettings = wifiStatus.passwordToTry;
            saveConfig(config); 
            P_PRINTF("[WIFI_PROC] 连接成功: SSID=%s, IP=%s\n", wifiStatus.ssidToTry.c_str(), WiFi.localIP().toString().c_str());
            responseDoc["success"] = true;
            responseDoc["message"] = "WiFi connected successfully to " + wifiStatus.ssidToTry;
            responseDoc["ip"] = WiFi.localIP().toString();
            wifiStatus.connectProgress = WIFI_CP_IDLE;
            sendUpdateToClient = true;
            ntpInitialAttempts = 0; 
            lastNtpAttemptTime = 0;
            ntpGiveUp = false;
        } else if (millis() - wifiStatus.connectAttemptStartTime > 20000) { 
            P_PRINTF("[WIFI_PROC] 连接超时: SSID=%s. WiFi Status: %d\n", wifiStatus.ssidToTry.c_str(), status); 
            WiFi.disconnect(true); 
            responseDoc["success"] = false;
            responseDoc["message"] = "Failed to connect to " + wifiStatus.ssidToTry + " (Timeout, Status: " + String(status) + ")";
            wifiStatus.connectProgress = WIFI_CP_FAILED;
            sendUpdateToClient = true;
        } else if (status == WL_NO_SSID_AVAIL || status == WL_CONNECT_FAILED || status == WL_CONNECTION_LOST) { 
            P_PRINTF("[WIFI_PROC] 连接失败: SSID=%s. WiFi Status: %d\n", wifiStatus.ssidToTry.c_str(), status);
            WiFi.disconnect(true); 
            responseDoc["success"] = false;
            responseDoc["message"] = "Failed to connect to " + wifiStatus.ssidToTry + " (Error, Status: " + String(status) + ")";
            wifiStatus.connectProgress = WIFI_CP_FAILED;
            sendUpdateToClient = true;
        }
        
        if (sendUpdateToClient) {
            if (wifiStatus.connectInitiatorClientNum != 255 && wifiStatus.connectInitiatorClientNum < webSocket.connectedClients()) {
                String responseStr;
                serializeJson(responseDoc, responseStr);
                webSocket.sendTXT(wifiStatus.connectInitiatorClientNum, responseStr);
            }
            sendWifiStatusToClients(wifiStatus); 
            wifiStatus.connectInitiatorClientNum = 255; 
        }
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
    if (!wifiStatus.isScanning) return;
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
        default: break;
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
    wsActionHandlers["startCalibration"] = handleStartCalibrationRequest; // 新增
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

// WebSocket action handlers
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
    currentConfig.thresholds.coPpmMax   = request["coPpmMax"]   | currentConfig.thresholds.coPpmMax;
    currentConfig.thresholds.no2PpmMax  = request["no2PpmMax"]  | currentConfig.thresholds.no2PpmMax;
    currentConfig.thresholds.c2h5ohPpmMax = request["c2h5ohPpmMax"] | currentConfig.thresholds.c2h5ohPpmMax;
    currentConfig.thresholds.vocPpmMax  = request["vocPpmMax"]  | currentConfig.thresholds.vocPpmMax;
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
            updateLedBrightness(brightness);
            saveConfig(currentConfig);
            response["type"] = "saveBrightnessStatus";
            response["success"] = true;
            response["message"] = "LED brightness saved and applied.";
        } else {
            response["type"] = "saveBrightnessStatus";
            response["success"] = false;
            response["message"] = "Invalid brightness value (must be 0-100).";
        }
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
            WiFi.disconnect(true); 
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

// 新增: 处理校准请求
void handleStartCalibrationRequest(uint8_t clientNum, const JsonDocument& request, JsonDocument& response) {
    P_PRINTLN("[WS] 收到启动校准请求.");
    startCalibration(); // 调用 sensor_handler 中的函数
    response["type"] = "calibrationStatus";
    response["success"] = true;
    response["message"] = "Calibration process initiated.";
}

void sendSensorDataToClients(const DeviceState& state, uint8_t specificClientNum) {
    DynamicJsonDocument doc(1024); 
    doc["type"] = "sensorData";

    if (isnan(state.temperature)) doc["temperature"] = nullptr; else doc["temperature"] = state.temperature;
    if (isnan(state.humidity)) doc["humidity"] = nullptr; else doc["humidity"] = state.humidity;
    
    JsonObject gas = doc.createNestedObject("gasPpm");
    if (isnan(state.gasPpmValues.co)) gas["co"] = nullptr; else gas["co"] = state.gasPpmValues.co;
    if (isnan(state.gasPpmValues.no2)) gas["no2"] = nullptr; else gas["no2"] = state.gasPpmValues.no2;
    if (isnan(state.gasPpmValues.c2h5oh)) gas["c2h5oh"] = nullptr; else gas["c2h5oh"] = state.gasPpmValues.c2h5oh;
    if (isnan(state.gasPpmValues.voc)) gas["voc"] = nullptr; else gas["voc"] = state.gasPpmValues.voc;

    doc["tempStatus"] = getSensorStatusString(state.tempStatus);
    doc["humStatus"]  = getSensorStatusString(state.humStatus);
    doc["gasCoStatus"] = getSensorStatusString(state.gasCoStatus);
    doc["gasNo2Status"] = getSensorStatusString(state.gasNo2Status);
    doc["gasC2h5ohStatus"] = getSensorStatusString(state.gasC2h5ohStatus);
    doc["gasVocStatus"] = getSensorStatusString(state.gasVocStatus);
    doc["timeIsRelative"] = !ntpSynced;
    char timeStr[12];
    if (ntpSynced) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        generateTimeStr(tv.tv_sec, false, timeStr);
    } else {
        generateTimeStr(millis(), true, timeStr);
    }
    doc["timeStr"] = timeStr;
    String jsonString;
    serializeJson(doc, jsonString);
    if (specificClientNum != 255 && specificClientNum < webSocket.connectedClients()) webSocket.sendTXT(specificClientNum, jsonString);
    else webSocket.broadcastTXT(jsonString);
}

void sendWifiStatusToClients(const WifiState& currentWifiState, uint8_t specificClientNum) {
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
    doc["connecting_attempt_ssid"] = (currentWifiState.connectProgress == WIFI_CP_CONNECTING || currentWifiState.connectProgress == WIFI_CP_DISCONNECTING) ? currentWifiState.ssidToTry : "";
    doc["connection_failed"] = (currentWifiState.connectProgress == WIFI_CP_FAILED);
    doc["ntp_synced"] = ntpSynced;
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
        dataPoint["rel"] = dp.isTimeRelative; 
        
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
    thresholdsObj["coPpmMax"] = config.thresholds.coPpmMax;     
    thresholdsObj["no2PpmMax"] = config.thresholds.no2PpmMax;   
    thresholdsObj["c2h5ohPpmMax"] = config.thresholds.c2h5ohPpmMax; 
    thresholdsObj["vocPpmMax"] = config.thresholds.vocPpmMax;

    // 新增: 发送R0值
    JsonObject r0Obj = settingsObj.createNestedObject("r0Values");
    r0Obj["co"] = config.r0Values.co;
    r0Obj["no2"] = config.r0Values.no2;
    r0Obj["c2h5oh"] = config.r0Values.c2h5oh;
    r0Obj["voc"] = config.r0Values.voc;

    settingsObj["currentSSID"] = WiFi.isConnected() ? WiFi.SSID() : config.currentSsidForSettings;
    settingsObj["ledBrightness"] = config.ledBrightness;
    String jsonString;
    serializeJson(doc, jsonString);
    webSocket.sendTXT(clientNum, jsonString);
}

// 新增: 发送校准状态
void sendCalibrationStatusToClients(uint8_t specificClientNum) {
    DynamicJsonDocument doc(1024);
    doc["type"] = "calibrationStatusUpdate";

    JsonObject calStatus = doc.createNestedObject("calibration");
    calStatus["state"] = currentState.calibrationState; // 0: IDLE, 1: IN_PROGRESS, 2: COMPLETED, 3: FAILED
    calStatus["progress"] = currentState.calibrationProgress;

    JsonObject currentR0 = calStatus.createNestedObject("currentR0");
    currentR0["co"] = currentConfig.r0Values.co;
    currentR0["no2"] = currentConfig.r0Values.no2;
    currentR0["c2h5oh"] = currentConfig.r0Values.c2h5oh;
    currentR0["voc"] = currentConfig.r0Values.voc;
    
    JsonObject measuredR0 = calStatus.createNestedObject("measuredR0");
    if (isnan(currentState.measuredR0.co)) measuredR0["co"] = nullptr; else measuredR0["co"] = currentState.measuredR0.co;
    if (isnan(currentState.measuredR0.no2)) measuredR0["no2"] = nullptr; else measuredR0["no2"] = currentState.measuredR0.no2;
    if (isnan(currentState.measuredR0.c2h5oh)) measuredR0["c2h5oh"] = nullptr; else measuredR0["c2h5oh"] = currentState.measuredR0.c2h5oh;
    if (isnan(currentState.measuredR0.voc)) measuredR0["voc"] = nullptr; else measuredR0["voc"] = currentState.measuredR0.voc;

    String jsonString;
    serializeJson(doc, jsonString);

    if (specificClientNum != 255 && specificClientNum < webSocket.connectedClients()) {
        webSocket.sendTXT(specificClientNum, jsonString);
    } else {
        webSocket.broadcastTXT(jsonString);
    }
}
