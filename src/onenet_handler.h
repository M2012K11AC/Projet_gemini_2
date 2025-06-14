#ifndef ONENET_HANDLER_H
#define ONENET_HANDLER_H

#include <Arduino.h>
#include "data_manager.h" // 包含此文件以使用 SensorStatusVal 枚举

// ==========================================================================
// == OneNET MQTT 配置 ==
// ==========================================================================

// -- 连接凭证 --
#define ONENET_MQTT_SERVER "www.onenet.hk.chinamobile.com"
#define ONENET_MQTT_PORT 1883
#define ONENET_PRODUCT_ID "IHL2T99b8k"
#define ONENET_DEVICE_ID "xiaomi"
#define ONENET_TOKEN "version=2018-10-31&res=products%2FIHL2T99b8k%2Fdevices%2Fxiaomi&et=2538749875&method=md5&sign=11FeOhHkd%2FH6sq9FuCMNlA%3D%3D"

// -- 物模型属性 (Properties) 主题 --
#define ONENET_TOPIC_PROPERTY_POST "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_ID "/thing/property/post"
#define ONENET_TOPIC_PROPERTY_SET "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_ID "/thing/property/set"
#define ONENET_TOPIC_PROPERTY_POST_REPLY "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_ID "/thing/property/post/reply"
#define ONENET_TOPIC_PROPERTY_SET_REPLY "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_ID "/thing/property/set_reply"

// -- 物模型事件 (Events) 主题 --
#define ONENET_TOPIC_EVENT_POST_BASE "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_ID "/thing/event/"
#define ONENET_TOPIC_EVENT_TEMP_ALARM_POST ONENET_TOPIC_EVENT_POST_BASE "temp_alarm/post"
#define ONENET_TOPIC_EVENT_HUM_ALARM_POST ONENET_TOPIC_EVENT_POST_BASE "hum_alarm/post"
#define ONENET_TOPIC_EVENT_GAS_ALARM_POST ONENET_TOPIC_EVENT_POST_BASE "gas_alarm/post"


// ==========================================================================
// == 函数声明 ==
// ==========================================================================

/**
 * @brief 启动OneNET MQTT上传任务 (如果尚未运行).
 * 当WiFi连接成功时调用.
 */
void startOneNetMqttTask();

/**
 * @brief 停止并删除OneNET MQTT上传任务.
 * 当WiFi断开连接时调用.
 */
void stopOneNetMqttTask();

/**
 * @brief OneNET MQTT 上传任务函数本身.
 * 由FreeRTOS调度器调用，不应直接调用.
 * @param pvParameters 任务参数 (未使用).
 */
void oneNetMqttTask(void *pvParameters);

/**
 * @brief 上报温度报警事件.
 * @param status 当前的温度状态 (例如 SS_WARNING, SS_NORMAL).
 */
void postTemperatureAlarm(SensorStatusVal status);

/**
 * @brief 上报湿度报警事件.
 * @param status 当前的湿度状态.
 */
void postHumidityAlarm(SensorStatusVal status);

/**
 * @brief 上报气体报警事件.
 * @param gasType 报警的气体类型 (例如 "CO", "NO2").
 * @param currentValue 当前的气体浓度值.
 * @param thresholdValue 触发报警的阈值.
 */
void postGasAlarm(const char* gasType, float currentValue, float thresholdValue);


#endif // ONENET_HANDLER_H
