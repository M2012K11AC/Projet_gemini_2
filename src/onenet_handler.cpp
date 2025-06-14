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

/**
 * @brief MQTT消息回调函数.
 * 当订阅的主题收到消息时，此函数被调用.
 * @param topic 消息的主题.
 * @param payload 消息的内容.
 * @param length 消息的长度.
 */
void mqttCallback(char* topic, byte* payload, unsigned int length);

/**
 * @brief 连接到OneNET MQTT服务器.
 * 如果断开连接，此函数会尝试重新连接.
 */
void connectToOneNet();

/**
 * @brief 将传感器数据作为物模型属性上报到OneNET.
 */
void postProperties();

// ==========================================================================
// == 函数实现 ==
// ==========================================================================

void initOneNetMqttTask() {
    // 创建一个独立的任务来处理OneNET的连接和数据上报
    xTaskCreatePinnedToCore(
        oneNetMqttTask,         // 任务函数
        "OneNetMqttTask",       // 任务名
        8192,                   // 任务堆栈大小 (MQTT和JSON需要较大堆栈)
        NULL,                   // 任务参数
        1,                      // 任务优先级
        &oneNetTaskHandle,      // 任务句柄
        1                       // 在核心1上运行
    );
    P_PRINTLN("[OneNET] MQTT处理任务已创建并启动.");
}

void oneNetMqttTask(void *pvParameters) {
    // 设置MQTT服务器和回调
    mqttClient.setServer(ONENET_MQTT_SERVER, ONENET_MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(1024); // 增加缓冲区大小以适应OneNET的JSON格式

    // 任务主循环
    for (;;) {
        // 1. 检查WiFi是否连接
        if (WiFi.status() != WL_CONNECTED) {
            P_PRINTLN("[OneNET Task] WiFi未连接, 等待中...");
            vTaskDelay(pdMS_TO_TICKS(5000)); // 等待5秒后重试
            continue; // 继续下一次循环检查
        }

        // 2. 检查并维持MQTT连接
        if (!mqttClient.connected()) {
            connectToOneNet();
        }

        // 3. 维持客户端运行
        mqttClient.loop();

        // 4. 定期上报数据
        if (millis() - lastPostTime >= POST_INTERVAL_MS) {
            lastPostTime = millis();
            if (mqttClient.connected()) {
                postProperties();
            } else {
                P_PRINTLN("[OneNET Task] MQTT未连接, 跳过本次数据上报.");
            }
        }
        
        // 短暂延时，避免任务完全阻塞CPU
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    P_PRINTF("[OneNET] 收到消息, 主题: %s\n", topic);
    char payloadStr[length + 1];
    memcpy(payloadStr, payload, length);
    payloadStr[length] = '\0';
    P_PRINTF("[OneNET] 消息内容: %s\n", payloadStr);

    // 在这里可以添加处理云端下发指令的逻辑
    // 例如，解析JSON并根据指令控制设备
}

void connectToOneNet() {
    // 防止过于频繁地重连
    if (millis() - lastConnectTime < 5000) {
        return;
    }
    lastConnectTime = millis();

    P_PRINTLN("[OneNET] 正在尝试连接到MQTT服务器...");
    
    // 使用设备ID作为ClientID，产品ID作为用户名，Token作为密码进行连接
    if (mqttClient.connect(ONENET_DEVICE_ID, ONENET_PRODUCT_ID, ONENET_TOKEN)) {
        P_PRINTLN("[OneNET] MQTT连接成功!");
        
        // 订阅相关主题
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

void postProperties() {
    // 检查传感器状态，如果正在初始化或未连接，则不上报
    if (currentState.tempStatus == SS_INIT || currentState.tempStatus == SS_DISCONNECTED) {
        P_PRINTLN("[OneNET] 传感器数据未就绪，跳过本次上报。");
        return;
    }
    
    // 创建JSON文档
    DynamicJsonDocument doc(512);
    
    // 创建一个内嵌的 params 对象
    JsonObject params = doc.createNestedObject("params");
    
    // 添加温湿度属性
    params["Temperature"] = currentState.temperature;
    params["Humidity"] = currentState.humidity;

    // 添加气体浓度属性 (确保它们是有效数字)
    if (!isnan(currentState.gasPpmValues.co)) {
        params["CO"] = currentState.gasPpmValues.co;
    }
    if (!isnan(currentState.gasPpmValues.no2)) {
        params["NO2"] = currentState.gasPpmValues.no2;
    }
    if (!isnan(currentState.gasPpmValues.c2h5oh)) {
        params["C2H5OH"] = currentState.gasPpmValues.c2h5oh;
    }
    if (!isnan(currentState.gasPpmValues.voc)) {
        params["VOC"] = currentState.gasPpmValues.voc;
    }
    
    // 使用ArduinoJson构建完整的上报格式
    DynamicJsonDocument finalDoc(512);
    finalDoc["id"] = String(postMsgId++);
    finalDoc["version"] = "1.0";
    // 嵌套 params 对象
    finalDoc["params"] = params;

    // 序列化为字符串
    String postData;
    serializeJson(finalDoc, postData);

    P_PRINTLN("[OneNET] 准备上报数据:");
    P_PRINTLN(postData);

    // 发布消息
    if (mqttClient.publish(ONENET_TOPIC_PROPERTY_POST, postData.c_str())) {
        P_PRINTLN("[OneNET] 属性上报成功.");
    } else {
        P_PRINTLN("[OneNET] ***错误*** 属性上报失败!");
    }
}
