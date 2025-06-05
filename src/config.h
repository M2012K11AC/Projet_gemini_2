#ifndef CONFIG_H
#define CONFIG_H

// ==========================================================================
// == WiFi 配置 ==
// ==========================================================================
#define WIFI_AP_SSID "ESP32_Sensor_Hub_V2" // AP模式下的SSID
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
#define NTP_SYNC_INTERVAL_MS 3600000UL // NTP每小时同步间隔 (毫秒) <--- 添加UL后缀确保为无符号长整型

// ==========================================================================
// == I2C 引脚定义 (ESP32-S3 DevKitM-1 默认 Wire) ==
// ==========================================================================
#define I2C_SDA_PIN 8  // ESP32-S3 默认 I2C SDA
#define I2C_SCL_PIN 9  // ESP32-S3 默认 I2C SCL
#define GAS_SENSOR_I2C_ADDRESS 0x08 // Grove Multichannel Gas Sensor GMXXX 默认地址

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
#define DEFAULT_LED_BRIGHTNESS 50 // 默认LED亮度 (0-100)

// ==========================================================================
// == 气体传感器预热时间 ==
// ==========================================================================
#define GAS_SENSOR_WARMUP_PERIOD_MS 60000 // 气体传感器物理预热时间 (毫秒, 例如60秒)
#define GAS_SENSOR_ADC_MAX_VALID 1023 // 气体传感器ADC读数的合理上限 (10-bit ADC)


// ==========================================================================
// == 默认报警阈值 ==
// !! 注意: 下列气体阈值为传感器返回的原始ADC值。
// !! GMXXX库的ADC分辨率通常为10位 (0-1023)。
// ==========================================================================
// 温度 (°C)
#define DEFAULT_TEMP_MIN 10.0f
#define DEFAULT_TEMP_MAX 30.0f
// 湿度 (%)
#define DEFAULT_HUM_MIN 30.0f
#define DEFAULT_HUM_MAX 70.0f
// 气体浓度 (ADC值)
#define DEFAULT_CO_MIN 0       // 一氧化碳 (CO) - ADC值
#define DEFAULT_CO_MAX 700     // 示例ADC阈值
#define DEFAULT_NO2_MIN 0      // 二氧化氮 (NO2) - ADC值
#define DEFAULT_NO2_MAX 500    // 示例ADC阈值
#define DEFAULT_C2H5OH_MIN 0   // 乙醇 (C2H5OH) - ADC值
#define DEFAULT_C2H5OH_MAX 600 // 示例ADC阈值
#define DEFAULT_VOC_MIN 0      // 挥发性有机化合物 (VOC) - ADC值
#define DEFAULT_VOC_MAX 550    // 示例ADC阈值

// ==========================================================================
// == SPIFFS 文件系统配置 ==
// ==========================================================================
#define SETTINGS_FILE "/settings_gm.json"        // 配置文件名
#define HISTORICAL_DATA_FILE "/history_gm.json"  // 历史数据文件名

// ==========================================================================
// == 数据和更新频率 ==
// ==========================================================================
#define SENSOR_READ_INTERVAL_MS 2000       // 传感器读取间隔 (毫秒)
#define WEBSOCKET_UPDATE_INTERVAL_MS 2000  // WebSocket 数据更新间隔 (毫秒)
#define HISTORICAL_DATA_SAVE_INTERVAL_MS 300000UL // 历史数据保存到SPIFFS的间隔 (5分钟)
#define HISTORICAL_DATA_POINTS 90          // 存储的历史数据点数量 (例如 90个点 * 2秒/点 = 3分钟数据)

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
