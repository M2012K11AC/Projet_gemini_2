#include "sensor_handler.h"
#include "config.h"
#include "data_manager.h"
#include <WiFi.h>

#include <DHT.h>
#include <Wire.h>
#include "Multichannel_Gas_GMXXX.h"
#include <Adafruit_NeoPixel.h>

// ==========================================================================
// == 模块内部使用的全局对象和变量 ==
// ==========================================================================
static DHT dht(DHT_PIN, DHT_TYPE);
static GAS_GMXXX<TwoWire> gas_sensor;
static Adafruit_NeoPixel pixels(NEOPIXEL_NUM, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// 颜色定义 (在initHardware中初始化)
static uint32_t COLOR_GREEN_VAL, COLOR_RED_VAL, COLOR_BLUE_VAL, COLOR_YELLOW_VAL, COLOR_ORANGE_VAL, COLOR_OFF_VAL;

// ==========================================================================
// == 函数实现 ==
// ==========================================================================

void initHardware() {
    pixels.begin();
    pixels.setBrightness(map(DEFAULT_LED_BRIGHTNESS, 0, 100, 0, 255));
    pixels.clear();
    pixels.show();
    P_PRINTLN("[HW] NeoPixel RGB LED已初始化.");

    // 初始化颜色常量
    COLOR_GREEN_VAL  = pixels.Color(0, 120, 0);
    COLOR_RED_VAL    = pixels.Color(120, 0, 0);
    COLOR_BLUE_VAL   = pixels.Color(0, 0, 120);
    COLOR_YELLOW_VAL = pixels.Color(120, 120, 0);
    COLOR_ORANGE_VAL = pixels.Color(255, 165, 0);
    COLOR_OFF_VAL    = pixels.Color(0, 0, 0);

    // 初始状态灯为蓝色，表示正在启动
    pixels.setPixelColor(0, COLOR_BLUE_VAL);
    pixels.show();

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    P_PRINTLN("[HW] 蜂鸣器已初始化.");

    dht.begin();
    P_PRINTLN("[HW] DHT传感器已初始化.");

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN); 
    P_PRINTF("[HW] I2C总线已在 SDA=%d, SCL=%d 初始化.\n", I2C_SDA_PIN, I2C_SCL_PIN);
    
    if (isGasSensorConnected()) {
        P_PRINTLN("[HW] Grove多通道气体传感器V2已连接.");
        gas_sensor.begin(Wire, GAS_SENSOR_I2C_ADDRESS); 
        gas_sensor.preheated();
    } else {
        P_PRINTLN("[HW] ***错误*** 未检测到Grove多通道气体传感器V2!");
    }
}

bool isGasSensorConnected() {
    Wire.beginTransmission(GAS_SENSOR_I2C_ADDRESS);
    byte error = Wire.endTransmission();
    return (error == 0);
}

void updateLedBrightness(uint8_t brightness_percent) {
    uint8_t brightness_val = map(brightness_percent, 0, 100, 0, 255);
    pixels.setBrightness(brightness_val);
    pixels.show();
    P_PRINTF("[LED] 亮度已更新为 %d%%\n", brightness_percent);
}

void readSensors(DeviceState& state) {
    // 读取DHT11温湿度
    float newTemp = dht.readTemperature();
    float newHum = dht.readHumidity();
    if (isnan(newTemp) || isnan(newHum)) {
        state.tempStatus = SS_DISCONNECTED;
        state.humStatus = SS_DISCONNECTED;
    } else {
        // [修复] 将浮点数读数四舍五入为整数
        state.temperature = round(newTemp);
        state.humidity = round(newHum);
        if(state.tempStatus == SS_INIT || state.tempStatus == SS_DISCONNECTED) state.tempStatus = SS_NORMAL;
        if(state.humStatus == SS_INIT || state.humStatus == SS_DISCONNECTED) state.humStatus = SS_NORMAL;
    }

    // 读取气体传感器
    if (!isGasSensorConnected()) {
        state.gasCoStatus = state.gasNo2Status = state.gasC2h5ohStatus = state.gasVocStatus = SS_DISCONNECTED;
        state.gasPpmValues = {NAN, NAN, NAN, NAN};
        return;
    }

    bool isGasSensorPhysicallyWarmingUp = (millis() < gasSensorWarmupEndTime);
    if (isGasSensorPhysicallyWarmingUp) {
        state.gasCoStatus = state.gasNo2Status = state.gasC2h5ohStatus = state.gasVocStatus = SS_INIT;
        state.gasPpmValues = {NAN, NAN, NAN, NAN}; 
    } else {
        state.gasPpmValues.co = gas_sensor.measure_CO();
        state.gasPpmValues.no2 = gas_sensor.measure_NO2();
        state.gasPpmValues.c2h5oh = gas_sensor.measure_C2H5OH();
        state.gasPpmValues.voc = gas_sensor.measure_VOC();

        // 确定状态 (从INIT转为NORMAL)
        if(state.gasCoStatus == SS_INIT && state.gasPpmValues.co >= 0) state.gasCoStatus = SS_NORMAL;
        if(state.gasNo2Status == SS_INIT && state.gasPpmValues.no2 >= 0) state.gasNo2Status = SS_NORMAL;
        if(state.gasC2h5ohStatus == SS_INIT && state.gasPpmValues.c2h5oh >= 0) state.gasC2h5ohStatus = SS_NORMAL;
        if(state.gasVocStatus == SS_INIT && state.gasPpmValues.voc >= 0) state.gasVocStatus = SS_NORMAL;

        // 处理可能的传感器读取错误（返回负值）
        if(state.gasPpmValues.co < 0) state.gasCoStatus = SS_DISCONNECTED;
        if(state.gasPpmValues.no2 < 0) state.gasNo2Status = SS_DISCONNECTED;
        if(state.gasPpmValues.c2h5oh < 0) state.gasC2h5ohStatus = SS_DISCONNECTED;
        if(state.gasPpmValues.voc < 0) state.gasVocStatus = SS_DISCONNECTED;
    }
}

void checkAlarms(DeviceState& state, const DeviceConfig& config) {
    bool anyAlarm = false;
    // 温度报警
    if (state.tempStatus != SS_DISCONNECTED && state.tempStatus != SS_INIT) {
        if (state.temperature < config.thresholds.tempMin || state.temperature > config.thresholds.tempMax) {
            if (state.tempStatus == SS_NORMAL) P_PRINTF("[ALARM] 温度超限! %d°C (范围: %d-%d)\n", state.temperature, config.thresholds.tempMin, config.thresholds.tempMax);
            state.tempStatus = SS_WARNING; anyAlarm = true;
        } else if (state.tempStatus == SS_WARNING) {
            state.tempStatus = SS_NORMAL;
        }
    }
    // 湿度报警
    if (state.humStatus != SS_DISCONNECTED && state.humStatus != SS_INIT) {
        if (state.humidity < config.thresholds.humMin || state.humidity > config.thresholds.humMax) {
            if (state.humStatus == SS_NORMAL) P_PRINTF("[ALARM] 湿度超限! %d%% (范围: %d-%d)\n", state.humidity, config.thresholds.humMin, config.thresholds.humMax);
            state.humStatus = SS_WARNING; anyAlarm = true;
        } else if (state.humStatus == SS_WARNING) { 
            state.humStatus = SS_NORMAL; 
        }
    }
    // 气体报警
    if (state.gasCoStatus == SS_NORMAL && state.gasPpmValues.co > config.thresholds.coPpmMax) {
        P_PRINTF("[ALARM] CO超限! %.2f PPM (阈值: >%.2f)\n", state.gasPpmValues.co, config.thresholds.coPpmMax);
        state.gasCoStatus = SS_WARNING; anyAlarm = true;
    } else if (state.gasCoStatus == SS_WARNING && state.gasPpmValues.co <= config.thresholds.coPpmMax) {
        state.gasCoStatus = SS_NORMAL;
    }

    if (state.gasNo2Status == SS_NORMAL && state.gasPpmValues.no2 > config.thresholds.no2PpmMax) {
        P_PRINTF("[ALARM] NO2超限! %.2f PPM (阈值: >%.2f)\n", state.gasPpmValues.no2, config.thresholds.no2PpmMax);
        state.gasNo2Status = SS_WARNING; anyAlarm = true;
    } else if (state.gasNo2Status == SS_WARNING && state.gasPpmValues.no2 <= config.thresholds.no2PpmMax) {
        state.gasNo2Status = SS_NORMAL;
    }

    if (state.gasC2h5ohStatus == SS_NORMAL && state.gasPpmValues.c2h5oh > config.thresholds.c2h5ohPpmMax) {
        P_PRINTF("[ALARM] C2H5OH超限! %.2f PPM (阈值: >%.2f)\n", state.gasPpmValues.c2h5oh, config.thresholds.c2h5ohPpmMax);
        state.gasC2h5ohStatus = SS_WARNING; anyAlarm = true;
    } else if (state.gasC2h5ohStatus == SS_WARNING && state.gasPpmValues.c2h5oh <= config.thresholds.c2h5ohPpmMax) {
        state.gasC2h5ohStatus = SS_NORMAL;
    }

    if (state.gasVocStatus == SS_NORMAL && state.gasPpmValues.voc > config.thresholds.vocPpmMax) {
        P_PRINTF("[ALARM] VOC超限! %.2f PPM (阈值: >%.2f)\n", state.gasPpmValues.voc, config.thresholds.vocPpmMax);
        state.gasVocStatus = SS_WARNING; anyAlarm = true;
    } else if (state.gasVocStatus == SS_WARNING && state.gasPpmValues.voc <= config.thresholds.vocPpmMax) {
        state.gasVocStatus = SS_NORMAL;
    }

    // 控制蜂鸣器状态
    if (anyAlarm) {
        if (!state.buzzerShouldBeActive) {
            state.buzzerShouldBeActive = true;
            state.buzzerBeepCount = 0; 
            P_PRINTLN("[ALARM] 蜂鸣器激活!");
        }
    } else {
        if (state.buzzerShouldBeActive) {
            state.buzzerShouldBeActive = false;
            digitalWrite(BUZZER_PIN, LOW); 
            state.buzzerBeepCount = 0;
            P_PRINTLN("[ALARM] 报警解除, 蜂鸣器停止.");
        }
    }
}

void updateLedStatus(const DeviceState& state, const WifiState& wifiStatus) {
    unsigned long currentTime = millis();
    uint32_t colorToSet = COLOR_OFF_VAL;
    bool isAnySensorDisconnected = (state.tempStatus == SS_DISCONNECTED || state.humStatus == SS_DISCONNECTED ||
                                  state.gasCoStatus == SS_DISCONNECTED || state.gasNo2Status == SS_DISCONNECTED ||
                                  state.gasC2h5ohStatus == SS_DISCONNECTED || state.gasVocStatus == SS_DISCONNECTED);
    bool isAnySensorWarning = (state.tempStatus == SS_WARNING || state.humStatus == SS_WARNING ||
                               state.gasCoStatus == SS_WARNING || state.gasNo2Status == SS_WARNING ||
                               state.gasC2h5ohStatus == SS_WARNING || state.gasVocStatus == SS_WARNING);
    bool isAnySensorInitializing = (state.tempStatus == SS_INIT || state.humStatus == SS_INIT ||
                                   state.gasCoStatus == SS_INIT || state.gasNo2Status == SS_INIT ||
                                   state.gasC2h5ohStatus == SS_INIT || state.gasVocStatus == SS_INIT);
    
    DeviceState& mutableState = const_cast<DeviceState&>(state); 
    
    if (wifiStatus.isScanning) { 
        if (currentTime - mutableState.lastBlinkTime >= 200) { 
            mutableState.lastBlinkTime = currentTime; 
            mutableState.ledBlinkState = !mutableState.ledBlinkState;
        }
        colorToSet = mutableState.ledBlinkState ? COLOR_BLUE_VAL : pixels.Color(0,0,30); 
    } else if (isAnySensorInitializing) { 
        if (currentTime - mutableState.lastBlinkTime >= 300) {
            mutableState.lastBlinkTime = currentTime;
            mutableState.ledBlinkState = !mutableState.ledBlinkState;
        }
        colorToSet = mutableState.ledBlinkState ? COLOR_ORANGE_VAL : pixels.Color(100,60,0); 
    } else if (isAnySensorDisconnected) { 
        if (currentTime - mutableState.lastBlinkTime >= 500) {
            mutableState.lastBlinkTime = currentTime;
            mutableState.ledBlinkState = !mutableState.ledBlinkState;
        }
        colorToSet = mutableState.ledBlinkState ? COLOR_BLUE_VAL : COLOR_OFF_VAL; 
    } else if (isAnySensorWarning) { 
        colorToSet = COLOR_RED_VAL; 
    } else if (wifiStatus.connectProgress == WIFI_CP_CONNECTING || wifiStatus.connectProgress == WIFI_CP_DISCONNECTING) { 
         if (currentTime - mutableState.lastBlinkTime >= 300) {
            mutableState.lastBlinkTime = currentTime;
            mutableState.ledBlinkState = !mutableState.ledBlinkState;
        }
        colorToSet = mutableState.ledBlinkState ? COLOR_BLUE_VAL : pixels.Color(0,0,50); 
    } else if (!WiFi.isConnected() && (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA)) { 
        colorToSet = COLOR_YELLOW_VAL; 
    } else { 
        colorToSet = COLOR_GREEN_VAL; 
    }

    if (pixels.getPixelColor(0) != colorToSet) { 
        pixels.setPixelColor(0, colorToSet);
        pixels.show();
    }
}

void controlBuzzer(DeviceState& state) {
    unsigned long currentTime = millis();
    if (state.buzzerShouldBeActive) {
        if (state.buzzerBeepCount < BUZZER_ALARM_COUNT) {
            if (currentTime >= state.buzzerStopTime) {
                if (digitalRead(BUZZER_PIN) == HIGH) { 
                    digitalWrite(BUZZER_PIN, LOW);
                    state.buzzerStopTime = currentTime + BUZZER_ALARM_INTERVAL; 
                } else { 
                    digitalWrite(BUZZER_PIN, HIGH);
                    state.buzzerStopTime = currentTime + BUZZER_ALARM_DURATION; 
                    state.buzzerBeepCount++;
                }
            }
        } else { // 响3次后停止本次报警声音，但状态依然是报警
            digitalWrite(BUZZER_PIN, LOW); 
        }
    } else { 
        if (digitalRead(BUZZER_PIN) == HIGH) { 
            digitalWrite(BUZZER_PIN, LOW);
        }
        state.buzzerBeepCount = 0; 
        state.buzzerStopTime = 0;  
    }
}
