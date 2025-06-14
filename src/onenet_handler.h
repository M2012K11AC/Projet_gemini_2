#ifndef ONENET_HANDLER_H
#define ONENET_HANDLER_H

#include <Arduino.h>

// ==========================================================================
// == OneNET MQTT 配置 ==
// ==========================================================================

// -- 连接凭证 --
#define ONENET_MQTT_SERVER "www.onenet.hk.chinamobile.com" 
#define ONENET_MQTT_PORT 1883 // 使用标准的非加密MQTT端口
#define ONENET_PRODUCT_ID "IHL2T99b8k"
#define ONENET_DEVICE_ID "xiaomi"
// 注意：这个Token有一个很长的有效期 (et=2538749875)，如果它过期了，你需要在这里更新它
#define ONENET_TOKEN "version=2018-10-31&res=products%2FIHL2T99b8k%2Fdevices%2Fxiaomi&et=2538749875&method=md5&sign=11FeOhHkd%2FH6sq9FuCMNlA%3D%3D"

// -- 物模型主题 (根据你的文件和OneNET文档) --
#define ONENET_TOPIC_PROPERTY_POST "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_ID "/thing/property/post"
#define ONENET_TOPIC_PROPERTY_SET "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_ID "/thing/property/set"
#define ONENET_TOPIC_PROPERTY_POST_REPLY "$sys/" ONENET_PRODUCT_ID "/" ONENET_DEVICE_ID "/thing/property/post/reply"

// ==========================================================================
// == 函数声明 ==
// ==========================================================================

/**
 * @brief 初始化并启动OneNET MQTT上传任务.
 * 此函数应在setup()中调用.
 */
void initOneNetMqttTask();

/**
 * @brief OneNET MQTT 上传任务.
 * 这是一个独立的FreeRTOS任务，负责处理MQTT连接和数据上报.
 * @param pvParameters 任务参数 (未使用).
 */
void oneNetMqttTask(void *pvParameters);

#endif // ONENET_HANDLER_H
