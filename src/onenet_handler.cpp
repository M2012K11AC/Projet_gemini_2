#include "onenet_handler.h"
#include "config.h"
#include "data_manager.h" // 引入data_manager来访问全局的currentState和currentConfig
#include "sensor_handler.h" // 引入sensor_handler以调用LED和Buzzer控制

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ==========================================================================
// == 模块内部使用的全局对象和变量 ==
// ==========================================================================
static WiFiClient espClient; 
static PubSubClient mqttClient(espClient); 
static TaskHandle_t oneNetTaskHandle = NULL; // FreeRTOS任务句柄，初始为NULL
static unsigned long postMsgId = 0;
static unsigned long postEventId = 0; 

static unsigned long lastConnectTime = 0;
static unsigned long lastPostTime = 0;

const unsigned long POST_INTERVAL_MS = 60000; 

// ==========================================================================
// == 内部函数声明 ==
// ==========================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length);
void connectToOneNet();
void postProperties();
void replyToCloud(const char* msgId, int code, const char* msg);

// ==========================================================================
// == 函数实现 ==
// ==========================================================================

void startOneNetMqttTask() {
    if (oneNetTaskHandle == NULL) {
        xTaskCreatePinnedToCore(
            oneNetMqttTask,         // 任务函数
            "OneNetMqttTask",       // 任务名
            8192,                   // 任务堆栈大小
            NULL,                   // 任务参数
            1,                      // 任务优先级
            &oneNetTaskHandle,      // 任务句柄
            1                       // 在核心1上运行
        );
        P_PRINTLN("[OneNET] MQTT处理任务已创建并启动.");
    } else {
        P_PRINTLN("[OneNET] MQTT任务已在运行中, 无需重复启动.");
    }
}

void stopOneNetMqttTask() {
    if (oneNetTaskHandle != NULL) {
        P_PRINTLN("[OneNET] 正在停止MQTT任务...");
        mqttClient.disconnect();
        vTaskDelete(oneNetTaskHandle);
        oneNetTaskHandle = NULL; // 将句柄置为NULL，表示任务已停止
        P_PRINTLN("[OneNET] MQTT任务已停止.");
    }
}


void oneNetMqttTask(void *pvParameters) {
    mqttClient.setServer(ONENET_MQTT_SERVER, ONENET_MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(1024);

    for (;;) {
        // 虽然任务的启停由WiFi状态控制，但在此处保留检查作为安全措施
        if (WiFi.status() != WL_CONNECTED) {
            P_PRINTLN("[OneNET Task] WiFi断开, 任务暂停等待被外部停止...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (!mqttClient.connected()) {
            connectToOneNet();
        }

        mqttClient.loop();

        if (millis() - lastPostTime >= POST_INTERVAL_MS) {
            lastPostTime = millis();
            if (mqttClient.connected()) {
                postProperties();
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String topicStr = String(topic);
    P_PRINTF("[OneNET] 收到消息, 主题: %s\n", topicStr.c_str());
    
    char payloadStr[length + 1];
    memcpy(payloadStr, payload, length);
    payloadStr[length] = '\0';
    P_PRINTF("[OneNET] 消息内容: %s\n", payloadStr);

    if (topicStr.equals(ONENET_TOPIC_PROPERTY_SET)) {
        DynamicJsonDocument doc(512);
        DeserializationError error = deserializeJson(doc, payloadStr);

        if (error) {
            P_PRINTF("[OneNET] JSON解析失败: %s\n", error.c_str());
            return;
        }

        const char* msgId = doc["id"];
        JsonObject params = doc["params"];
        bool configChanged = false;

        for (JsonPair kv : params) {
            const char* key = kv.key().c_str();
            JsonVariant value = kv.value();
            
            if (strcmp(key, "maxtemp_set") == 0) {
                currentConfig.thresholds.tempMax = value.as<int>();
                configChanged = true;
            } else if (strcmp(key, "minitemp_set") == 0) {
                currentConfig.thresholds.tempMin = value.as<int>();
                configChanged = true;
            } else if (strcmp(key, "maxhum_set") == 0) {
                currentConfig.thresholds.humMax = value.as<int>();
                configChanged = true;
            } else if (strcmp(key, "minihum_set") == 0) {
                currentConfig.thresholds.humMin = value.as<int>();
                configChanged = true;
            } else if (strcmp(key, "maxCO_set") == 0) {
                currentConfig.thresholds.coPpmMax = value.as<float>();
                configChanged = true;
            } else if (strcmp(key, "maxNO2_set") == 0) {
                currentConfig.thresholds.no2PpmMax = value.as<float>();
                configChanged = true;
            } else if (strcmp(key, "led_switch") == 0) {
                P_PRINTF("[OneNET] LED 开关指令: %s\n", value.as<bool>() ? "ON" : "OFF");
            }
        }

        if (configChanged) {
            P_PRINTLN("[OneNET] 报警阈值已从云端更新。");
            saveConfig(currentConfig);
            checkAlarms(currentState, currentConfig);
        }
        replyToCloud(msgId, 200, "success");
    }
}

void replyToCloud(const char* msgId, int code, const char* msg) {
    DynamicJsonDocument replyDoc(128);
    replyDoc["id"] = msgId;
    replyDoc["code"] = code;
    replyDoc["msg"] = msg;
    String replyStr;
    serializeJson(replyDoc, replyStr);
    if(mqttClient.publish(ONENET_TOPIC_PROPERTY_SET_REPLY, replyStr.c_str())) {
        P_PRINTF("[OneNET] 向云端发送回复: %s\n", replyStr.c_str());
    } else {
        P_PRINTLN("[OneNET] ***错误*** 发送回复失败!");
    }
}

void connectToOneNet() {
    if (millis() - lastConnectTime < 5000) return;
    lastConnectTime = millis();
    P_PRINTLN("[OneNET Task] 正在尝试连接到MQTT服务器...");
    if (mqttClient.connect(ONENET_DEVICE_ID, ONENET_PRODUCT_ID, ONENET_TOKEN)) {
        P_PRINTLN("[OneNET Task] MQTT连接成功!");
        if (mqttClient.subscribe(ONENET_TOPIC_PROPERTY_SET)) {
            P_PRINTF("[OneNET Task] 成功订阅主题: %s\n", ONENET_TOPIC_PROPERTY_SET);
        } else { P_PRINTF("[OneNET Task] ***错误*** 订阅主题 %s 失败\n", ONENET_TOPIC_PROPERTY_SET); }
        if (mqttClient.subscribe(ONENET_TOPIC_PROPERTY_POST_REPLY)) {
            P_PRINTF("[OneNET Task] 成功订阅主题: %s\n", ONENET_TOPIC_PROPERTY_POST_REPLY);
        } else { P_PRINTF("[OneNET Task] ***错误*** 订阅主题 %s 失败\n", ONENET_TOPIC_PROPERTY_POST_REPLY); }
    } else {
        P_PRINTF("[OneNET Task] ***错误*** MQTT连接失败, rc=%d. 5秒后重试.\n", mqttClient.state());
    }
}

void postProperties() {
    if (currentState.tempStatus == SS_INIT || currentState.tempStatus == SS_DISCONNECTED) return;
    DynamicJsonDocument finalDoc(512);
    finalDoc["id"] = String(postMsgId++);
    finalDoc["version"] = "1.0";
    JsonObject params = finalDoc.createNestedObject("params");
    params["temp_value"] = currentState.temperature;
    params["humidity_value"] = currentState.humidity;
    if (!isnan(currentState.gasPpmValues.co)) params["CO_ppm"] = currentState.gasPpmValues.co;
    if (!isnan(currentState.gasPpmValues.no2)) params["NO2_ppm"] = currentState.gasPpmValues.no2;
    if (!isnan(currentState.gasPpmValues.c2h5oh)) params["C2H5OH_ppm"] = currentState.gasPpmValues.c2h5oh;
    if (!isnan(currentState.gasPpmValues.voc)) params["VOC_ppm"] = currentState.gasPpmValues.voc;
    String postData;
    serializeJson(finalDoc, postData);
    if (mqttClient.publish(ONENET_TOPIC_PROPERTY_POST, postData.c_str())) {
        P_PRINTLN("[OneNET] 属性上报成功.");
    } else {
        P_PRINTLN("[OneNET] ***错误*** 属性上报失败!");
    }
}

void postTemperatureAlarm(SensorStatusVal status) {
    if (!mqttClient.connected()) return;
    DynamicJsonDocument doc(256);
    doc["id"] = String(postEventId++);
    doc["version"] = "1.0";
    JsonObject params = doc.createNestedObject("params");
    int alarmStatus = 0;
    if (status == SS_WARNING) {
        alarmStatus = (currentState.temperature > currentConfig.thresholds.tempMax) ? 1 : 2;
    }
    params["alarm_status"] = alarmStatus;
    String postData;
    serializeJson(doc, postData);
    P_PRINTF("[OneNET] 上报温度报警事件: %s\n", postData.c_str());
    mqttClient.publish(ONENET_TOPIC_EVENT_TEMP_ALARM_POST, postData.c_str());
}

void postHumidityAlarm(SensorStatusVal status) {
    if (!mqttClient.connected()) return;
    DynamicJsonDocument doc(256);
    doc["id"] = String(postEventId++);
    doc["version"] = "1.0";
    JsonObject params = doc.createNestedObject("params");
    int alarmStatus = 0;
    if (status == SS_WARNING) {
        alarmStatus = (currentState.humidity > currentConfig.thresholds.humMax) ? 1 : 2;
    }
    params["alarm_status"] = alarmStatus;
    String postData;
    serializeJson(doc, postData);
    P_PRINTF("[OneNET] 上报湿度报警事件: %s\n", postData.c_str());
    mqttClient.publish(ONENET_TOPIC_EVENT_HUM_ALARM_POST, postData.c_str());
}

void postGasAlarm(const char* gasType, float currentValue, float thresholdValue) {
    if (!mqttClient.connected()) return;
    DynamicJsonDocument doc(256);
    doc["id"] = String(postEventId++);
    doc["version"] = "1.0";
    JsonObject params = doc.createNestedObject("params");
    params["gas_type"] = gasType;
    params["current_value"] = currentValue;
    params["threshold_value"] = thresholdValue;
    String postData;
    serializeJson(doc, postData);
    P_PRINTF("[OneNET] 上报气体报警事件: %s\n", postData.c_str());
    mqttClient.publish(ONENET_TOPIC_EVENT_GAS_ALARM_POST, postData.c_str());
}
