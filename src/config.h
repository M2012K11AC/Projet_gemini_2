#ifndef CONFIG_H
#define CONFIG_H

// ==========================================================================
// == WiFi 配置 ==
// ==========================================================================
#define WIFI_AP_SSID "ESP32_Sensor_Hub"
#define WIFI_AP_PASSWORD ""
#define WIFI_AP_CHANNEL 1
#define WIFI_AP_MAX_CONNECTIONS 4

// ==========================================================================
// == 传感器引脚定义 ==
// ==========================================================================
#define DHT_PIN 4
#define DHT_TYPE DHT11

#define MQ2_PIN 5
#define BUZZER_PIN 10

// ==========================================================================
// == RGB LED (板载 NeoPixel GPIO48 for ESP32-S3 DevKitC-1 clone) ==
// ==========================================================================
#define NEOPIXEL_PIN 48
#define NEOPIXEL_NUM 1

// ==========================================================================
// == 默认报警阈值 ==
// ==========================================================================
#define DEFAULT_TEMP_MIN 10.0f
#define DEFAULT_TEMP_MAX 30.0f
#define DEFAULT_HUM_MIN 30.0f
#define DEFAULT_HUM_MAX 70.0f
#define DEFAULT_GAS_MIN 0
#define DEFAULT_GAS_MAX 700 // MQ-2 ADC 读数值

// ==========================================================================
// == SPIFFS 文件系统配置 ==
// ==========================================================================
#define SETTINGS_FILE "/settings.json"
#define HISTORICAL_DATA_FILE "/history.json"

// ==========================================================================
// == 数据和更新频率 ==
// ==========================================================================
#define SENSOR_READ_INTERVAL_MS 2000
#define WEBSOCKET_UPDATE_INTERVAL_MS 2000
#define HISTORICAL_DATA_POINTS ( (3 * 60 * 1000) / SENSOR_READ_INTERVAL_MS )

// ==========================================================================
// == 调试信息输出 (已重命名以避免与库冲突) ==
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


// 蜂鸣器报警
#define BUZZER_ALARM_DURATION 150
#define BUZZER_ALARM_COUNT 3
#define BUZZER_ALARM_INTERVAL 200

// RGB LED 颜色定义 (GRB格式 for Adafruit_NeoPixel)
// 这些宏依赖于 'pixels' 对象，所以它们应该在使用前确保 'pixels' 已初始化
// 或者将 pixels.Color(...) 直接用在代码中，而不是作为宏定义。
// 为了安全，这里注释掉，建议直接在代码中使用 pixels.Color()。
// #define COLOR_GREEN   pixels.Color(0, 255, 0)
// #define COLOR_RED     pixels.Color(255, 0, 0)
// #define COLOR_BLUE    pixels.Color(0, 0, 255)
// #define COLOR_YELLOW  pixels.Color(255, 255, 0)
// #define COLOR_OFF     pixels.Color(0, 0, 0)
// 在main.cpp中直接使用 pixels.Color(R, G, B) 会更清晰

#endif // CONFIG_H
