#include "onenet_handler.h"
#include "config.h"
#include "data_manager.h" // 引入data_manager来访问全局的currentState
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ==========================================================================
// == 模块内部使用的全局对象和变量 ==
// ==========================================================================
static WiFiClient espClient; // 用于MQTT的TCP连接客户端
static PubSubClient mqttClient(espClient); // MQTT客户端实例
static TaskHandle_t oneNetTaskHandle = NULL; // FreeRTOS任务句柄
static unsigned long postMsgId = 0; // 用于追踪上报消息的ID

// 上次心跳和数据上报的时间戳
static unsigned long lastConnectTime = 0;
static unsigned long lastPostTime = 0;

// 上报周期 (60秒)
const unsigned long POST_INTERVAL_MS = 60000;

// ==========================================================================
// == 内部函数声明 ==
// ==========================================================================
void mqttCallback(char* topic, byte* payload, unsigned int length);
void connectToOneNet();
void postProperties();

// ==========================================================================
// == 函数实现 ==
// ==========================================================================

/**
 * @brief 初始化并创建OneNET MQTT处理任务。
 */
void initOneNetMqttTask() {
    xTaskCreatePinnedToCore(
        oneNetMqttTask, "OneNetMqttTask", 8192, NULL, 1, &oneNetTaskHandle, 1
    );
    P_PRINTLN("[OneNET] MQTT处理任务已创建并启动.");
}

/**
 * @brief OneNET MQTT处理任务的主循环。
 * @details 此任务根据WiFi状态自动管理其活动。
 * - WiFi连接时：尝试连接MQTT服务器并周期性上报数据。
 * - WiFi断开时：自动暂停，断开MQTT连接并等待WiFi恢复。
 */
void oneNetMqttTask(void *pvParameters) {
    // 初始化MQTT客户端配置
    mqttClient.setServer(ONENET_MQTT_SERVER, ONENET_MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(2048); 

    for (;;) {
        // 1. 检查WiFi连接状态，这是任务执行的前提
        if (WiFi.status() != WL_CONNECTED) {
            // 如果WiFi断开时MQTT客户端还显示在线，则主动断开
            if (mqttClient.connected()) {
                mqttClient.disconnect();
                P_PRINTLN("[OneNET Task] WiFi已断开, 主动断开MQTT连接。任务暂停。");
            }
            // 打印等待信息，并延迟，避免空转消耗CPU
            P_PRINTLN("[OneNET Task] WiFi未连接, 任务暂停等待中...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue; // 返回循环开始处，重新检查WiFi状态
        }

        // 2. WiFi已连接，开始处理MQTT逻辑
        // 如果MQTT未连接，则尝试连接
        if (!mqttClient.connected()) {
            connectToOneNet();
        }

        // 3. 维持MQTT心跳
        mqttClient.loop();

        // 4. 周期性上报数据
        // 检查是否到达上报时间并且MQTT已连接
        if (millis() - lastPostTime >= POST_INTERVAL_MS) {
            lastPostTime = millis();
            if (mqttClient.connected()) {
                postProperties();
            } else {
                P_PRINTLN("[OneNET Task] MQTT未连接, 跳过本次数据上报.");
            }
        }
        
        // 短暂延时，让出CPU给其他任务
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief MQTT消息回调函数。
 * @details 当从订阅的主题收到消息时，此函数被调用。
 */
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    P_PRINTF("[OneNET] 收到消息, 主题: %s\n", topic);
    char payloadStr[length + 1];
    memcpy(payloadStr, payload, length);
    payloadStr[length] = '\0';
    P_PRINTF("[OneNET] 消息内容: %s\n", payloadStr);

    // 在这里可以添加处理来自云端指令的逻辑
}

/**
 * @brief 连接到OneNET MQTT服务器。
 * @details 包含连接频率限制，防止频繁重连。
 */
void connectToOneNet() {
    // 限制5秒内最多尝试连接一次
    if (millis() - lastConnectTime < 5000) {
        return;
    }
    lastConnectTime = millis();

    P_PRINTLN("[OneNET] 正在尝试连接到MQTT服务器...");
    
    // 使用设备ID、产品ID和Token进行连接
    if (mqttClient.connect(ONENET_DEVICE_ID, ONENET_PRODUCT_ID, ONENET_TOKEN)) {
        P_PRINTLN("[OneNET] MQTT连接成功!");
        
        // 连接成功后，订阅相关主题
        if (mqttClient.subscribe(ONENET_TOPIC_PROPERTY_SET)) {
            P_PRINTF("[OneNET] 成功订阅主题: %s\n", ONENET_TOPIC_PROPERTY_SET);
        } else {
            P_PRINTF("[OneNET] ***错误*** 订阅主题 %s 失败\n", ONENET_TOPIC_PROPERTY_SET);
        }
        if (mqttClient.subscribe(ONENET_TOPIC_PROPERTY_POST_REPLY)) {
            P_PRINTF("[OneNET] 成功订阅主题: %s\n", ONENET_TOPIC_PROPERTY_POST_REPLY);
        } else {
            P_PRINTF("[OneNET] ***错误*** 订阅主题 %s 失败\n", ONENET_TOPIC_PROPERTY_POST_REPLY);
        }

    } else {
        P_PRINTF("[OneNET] ***错误*** MQTT连接失败, rc=%d. 5秒后重试.\n", mqttClient.state());
    }
}

/**
 * @brief 格式化并上报传感器属性到OneNET平台。
 */
void postProperties() {
    // 如果传感器还未准备好，则跳过本次上报
    if (currentState.tempStatus == SS_INIT || currentState.tempStatus == SS_DISCONNECTED) {
        P_PRINTLN("[OneNET] 传感器数据未就绪，跳过本次上报。");
        return;
    }
    
    DynamicJsonDocument postDoc(1024);
    
    postDoc["id"] = String(postMsgId++);
    postDoc["version"] = "1.0";
    
    JsonObject params = postDoc.createNestedObject("params");
    
    // 确保上报的格式和数据类型与物模型完全一致
    
    // 1. 温度 (temp_value) -> int64
    JsonObject temp_value_obj = params.createNestedObject("temp_value");
    temp_value_obj["value"] = currentState.temperature;

    // 2. 湿度 (humidity_value) -> int64
    JsonObject humidity_value_obj = params.createNestedObject("humidity_value");
    humidity_value_obj["value"] = (int)currentState.humidity;

    // 3. 气体浓度 (CO, NO2, C2H5OH, VOC) -> float
    if (!isnan(currentState.gasPpmValues.co)) {
        JsonObject co_ppm_obj = params.createNestedObject("CO_ppm");
        co_ppm_obj["value"] = round(currentState.gasPpmValues.co * 100) / 100.0;
    }
    if (!isnan(currentState.gasPpmValues.no2)) {
        JsonObject no2_ppm_obj = params.createNestedObject("NO2_ppm");
        no2_ppm_obj["value"] = round(currentState.gasPpmValues.no2 * 100) / 100.0;
    }
    if (!isnan(currentState.gasPpmValues.c2h5oh)) {
        JsonObject c2h5oh_ppm_obj = params.createNestedObject("C2H5OH_ppm");
        c2h5oh_ppm_obj["value"] = round(currentState.gasPpmValues.c2h5oh * 10) / 10.0;
    }
    if (!isnan(currentState.gasPpmValues.voc)) {
        JsonObject voc_ppm_obj = params.createNestedObject("VOC_ppm");
        voc_ppm_obj["value"] = round(currentState.gasPpmValues.voc * 100) / 100.0;
    }
    
    String postData;
    serializeJson(postDoc, postData);

    P_PRINTLN("[OneNET] 准备上报数据:");
    P_PRINTLN(postData);

    if (mqttClient.publish(ONENET_TOPIC_PROPERTY_POST, postData.c_str())) {
        P_PRINTLN("[OneNET] 属性上报成功.");
    } else {
        P_PRINTLN("[OneNET] ***错误*** 属性上报失败!");
    }
}
