#ifndef CONFIG_H
#define CONFIG_H

// ==========================================================================
// == WiFi 配置 ==
// ==========================================================================
#define WIFI_AP_SSID "ESP32_Sensor_Hub" // AP模式下的SSID
#define WIFI_AP_PASSWORD ""               // AP模式下的密码 (空表示开放)
#define WIFI_AP_CHANNEL 1                 // AP模式下的WiFi信道
#define WIFI_AP_MAX_CONNECTIONS 4         // AP模式下的最大连接数

// ==========================================================================
// == NTP (网络时间协议) 配置 ==
// ==========================================================================
#define NTP_SERVER1 "pool.ntp.org"
#define NTP_SERVER2 "time.nist.gov"
#define GMT_OFFSET_SEC 3600 * 8     // 时区偏移量 (例如 GMT+8 为 3600 * 8)
#define DAYLIGHT_OFFSET_SEC 0       // 夏令时偏移量 (秒)
#define MAX_NTP_ATTEMPTS_AFTER_WIFI 5 // WiFi连接后NTP的最大尝试次数
#define NTP_RETRY_DELAY_MS 10000      // WiFi连接后NTP失败的重试间隔
#define NTP_SYNC_INTERVAL_MS 3600000UL // NTP每小时同步间隔 (毫秒)

// ==========================================================================
// == I2C 引脚定义 (ESP32-S3 DevKitM-1 默认 Wire) ==
// ==========================================================================
#define I2C_SDA_PIN 8  // ESP32-S3 默认 I2C SDA
#define I2C_SCL_PIN 9  // ESP32-S3 默认 I2C SCL
#define GAS_SENSOR_I2C_ADDRESS 0x08 // Grove Multichannel Gas Sensor V2 默认地址

// ==========================================================================
// == 传感器引脚定义 ==
// ==========================================================================
#define DHT_PIN 4    // DHT11 数据引脚
#define DHT_TYPE DHT11 // DHT传感器类型

#define BUZZER_PIN 10 // 蜂鸣器引脚

// ==========================================================================
// == RGB LED (板载 NeoPixel GPIO48 for ESP32-S3 DevKitC-1 clone) ==
// ==========================================================================
#define NEOPIXEL_PIN 48 // NeoPixel RGB LED 引脚
#define NEOPIXEL_NUM 1  // NeoPixel LED 数量
#define DEFAULT_LED_BRIGHTNESS 20 // 默认LED亮度 (0-100)

// ==========================================================================
// == 气体传感器配置 ==
// ==========================================================================
#define GAS_SENSOR_WARMUP_PERIOD_MS 60000 // 气体传感器物理预热时间 (毫秒, 60秒)
#define SENSOR_VCC 3.3f                   // 传感器供电电压
#define ADC_RESOLUTION 4095.0f            // Grove Gas Sensor v2 使用12位ADC, 0-4095
#define CALIBRATION_SAMPLE_COUNT 100      // 校准时的采样次数
#define CALIBRATION_SAMPLE_INTERVAL_MS 200 // 校准时每次采样的间隔 (毫larg)

// ==========================================================================
// == 默认气体传感器 R0 值 (在洁净空气中的电阻) ==
// == 注意：这些是初始估算值，强烈建议进行校准以获得准确读数 ==
// ==========================================================================
#define DEFAULT_R0_CO       20.0f  // kOhm
#define DEFAULT_R0_NO2      10.0f   // kOhm
#define DEFAULT_R0_C2H5OH   2.0f   // kOhm
#define DEFAULT_R0_VOC      50.0f  // kOhm

// ==========================================================================
// == 默认报警阈值 (气体单位已改为PPM) ==
// ==========================================================================
// 温度 (°C) - 改为整数
#define DEFAULT_TEMP_MIN 10
#define DEFAULT_TEMP_MAX 30
// 湿度 (%) - 改为整数
#define DEFAULT_HUM_MIN 30
#define DEFAULT_HUM_MAX 70
// 气体浓度 (PPM) - 注意: 这些是最大值阈值
#define DEFAULT_CO_PPM_MAX 50.0f       // 一氧化碳 (CO) - PPM
#define DEFAULT_NO2_PPM_MAX 5.0f       // 二氧化氮 (NO2) - PPM
#define DEFAULT_C2H5OH_PPM_MAX 200.0f // 乙醇 (C2H5OH) - PPM
#define DEFAULT_VOC_PPM_MAX 10.0f      // 挥发性有机化合物 (VOC) - PPM

// ==========================================================================
// == SPIFFS 文件系统配置 ==
// ==========================================================================
#define SETTINGS_FILE "/settings_v4_cal.json"        // 配置文件名 (版本变更)
#define HISTORICAL_DATA_FILE "/history_v4_cal.json"  // 历史数据文件名 (版本变更)

// ==========================================================================
// == 数据和更新频率 ==
// ==========================================================================
#define SENSOR_READ_INTERVAL_MS 2000       // 传感器读取间隔 (毫秒)
#define WEBSOCKET_UPDATE_INTERVAL_MS 2000  // WebSocket 数据更新间隔 (毫秒)
#define HISTORICAL_DATA_SAVE_INTERVAL_MS 300000UL // 历史数据保存到SPIFFS的间隔 (5分钟)
#define HISTORICAL_DATA_POINTS 90          // 存储的历史数据点数量

// ==========================================================================
// == 调试信息输出 ==
// ==========================================================================
#define PROJECT_SERIAL_DEBUG true // 控制是否启用本项目特定的调试输出

#if PROJECT_SERIAL_DEBUG
  #define P_PRINT(x) Serial.print(x)
  #define P_PRINTLN(x) Serial.println(x)
  #define P_PRINTF(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#else
  #define P_PRINT(x)
  #define P_PRINTLN(x)
  #define P_PRINTF(fmt, ...)
#endif

// ==========================================================================
// == 蜂鸣器报警配置 ==
// ==========================================================================
#define BUZZER_ALARM_DURATION 150 // 蜂鸣器每次响的时长 (毫秒)
#define BUZZER_ALARM_COUNT 3      // 报警时蜂鸣器响的次数
#define BUZZER_ALARM_INTERVAL 200 // 蜂鸣器每次响之间的间隔 (毫秒)

#endif // CONFIG_H
