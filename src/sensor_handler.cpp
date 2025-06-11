#include "sensor_handler.h"
#include "config.h"
#include "data_manager.h"
#include "web_handler.h" // 包含 web_handler.h 以便发送WebSocket消息
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
static uint32_t COLOR_GREEN_VAL, COLOR_RED_VAL, COLOR_BLUE_VAL, COLOR_YELLOW_VAL, COLOR_ORANGE_VAL, COLOR_OFF_VAL, COLOR_CYAN_VAL;

// 传感器负载电阻 (RL)，单位 kOhm
const float RL_VALUE_KOHM = 10.0;

// ==========================================================================
// == 内部函数声明 ==
// ==========================================================================
float adcToRs(int adc_val);

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
    COLOR_ORANGE_VAL = pixels.Color(255, 100, 0);
    COLOR_CYAN_VAL   = pixels.Color(0, 255, 255);
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
        P_PRINTLN("[HW] 等待传感器预热...");
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
    uint32_t currentColor = pixels.getPixelColor(0);
    if(brightness_val > 0 && currentColor == 0) {
        pixels.setPixelColor(0, pixels.Color(1,1,1));
    }
    pixels.show();
    P_PRINTF("[LED] 亮度已更新为 %d%%\n", brightness_percent);
}

float adcToRs(int adc_val) {
    if (adc_val <= 0) return -1.0; 
    float v_out = (float)adc_val * SENSOR_VCC / ADC_RESOLUTION;
    if (v_out >= SENSOR_VCC) return -1.0; 
    return (SENSOR_VCC * RL_VALUE_KOHM / v_out) - RL_VALUE_KOHM;
}

void readSensors(DeviceState& state, const DeviceConfig& config) {
    float newTemp = dht.readTemperature();
    float newHum = dht.readHumidity();
    if (isnan(newTemp) || isnan(newHum)) {
        state.tempStatus = SS_DISCONNECTED;
        state.humStatus = SS_DISCONNECTED;
    } else {
        state.temperature = round(newTemp);
        state.humidity = round(newHum);
        if(state.tempStatus == SS_INIT || state.tempStatus == SS_DISCONNECTED) state.tempStatus = SS_NORMAL;
        if(state.humStatus == SS_INIT || state.humStatus == SS_DISCONNECTED) state.humStatus = SS_NORMAL;
    }

    if (!isGasSensorConnected()) {
        state.gasCoStatus = state.gasNo2Status = state.gasC2h5ohStatus = state.gasVocStatus = SS_DISCONNECTED;
        state.gasRsValues = {NAN, NAN, NAN, NAN};
        return;
    }

    bool isGasSensorPhysicallyWarmingUp = (millis() < gasSensorWarmupEndTime);
    if (isGasSensorPhysicallyWarmingUp || state.calibrationState == CAL_IN_PROGRESS) {
        if (isGasSensorPhysicallyWarmingUp) {
            state.gasCoStatus = state.gasNo2Status = state.gasC2h5ohStatus = state.gasVocStatus = SS_INIT;
        }
        state.gasPpmValues = {NAN, NAN, NAN, NAN};
        state.gasRsValues = {NAN, NAN, NAN, NAN};
    } else {
        state.gasRsValues.co = adcToRs(gas_sensor.getGM702B());
        state.gasRsValues.no2 = adcToRs(gas_sensor.getGM102B());
        state.gasRsValues.c2h5oh = adcToRs(gas_sensor.getGM302B());
        state.gasRsValues.voc = adcToRs(gas_sensor.getGM502B());

        calculatePpm(state, config);

        if(state.gasCoStatus == SS_INIT && state.gasRsValues.co >= 0) state.gasCoStatus = SS_NORMAL;
        if(state.gasNo2Status == SS_INIT && state.gasRsValues.no2 >= 0) state.gasNo2Status = SS_NORMAL;
        if(state.gasC2h5ohStatus == SS_INIT && state.gasRsValues.c2h5oh >= 0) state.gasC2h5ohStatus = SS_NORMAL;
        if(state.gasVocStatus == SS_INIT && state.gasRsValues.voc >= 0) state.gasVocStatus = SS_NORMAL;
        
        if(state.gasRsValues.co < 0) state.gasCoStatus = SS_DISCONNECTED;
        if(state.gasRsValues.no2 < 0) state.gasNo2Status = SS_DISCONNECTED;
        if(state.gasRsValues.c2h5oh < 0) state.gasC2h5ohStatus = SS_DISCONNECTED;
        if(state.gasRsValues.voc < 0) state.gasVocStatus = SS_DISCONNECTED;
    }
}

void calculatePpm(DeviceState& state, const DeviceConfig& config) {
    float ratio;

    if (state.gasRsValues.co > 0 && config.r0Values.co > 0) {
        ratio = state.gasRsValues.co / config.r0Values.co;
        double lgPPM = (log10(ratio) * -2.82) - 0.12;
        state.gasPpmValues.co = pow(10, lgPPM);
    } else {
        state.gasPpmValues.co = NAN;
    }

    if (state.gasRsValues.no2 > 0 && config.r0Values.no2 > 0) {
        ratio = state.gasRsValues.no2 / config.r0Values.no2;
        double lgPPM = (log10(ratio) * 1.9) - 0.2;
        state.gasPpmValues.no2 = pow(10, lgPPM);
    } else {
        state.gasPpmValues.no2 = NAN;
    }

    if (state.gasRsValues.c2h5oh > 0 && config.r0Values.c2h5oh > 0) {
        ratio = state.gasRsValues.c2h5oh / config.r0Values.c2h5oh;
        double lgPPM = (log10(ratio) * -2.0) - 0.5;
        state.gasPpmValues.c2h5oh = pow(10, lgPPM);
    } else {
        state.gasPpmValues.c2h5oh = NAN;
    }
    
    if (state.gasRsValues.voc > 0 && config.r0Values.voc > 0) {
        ratio = state.gasRsValues.voc / config.r0Values.voc;
        double lgPPM = (log10(ratio) * -2.5) - 0.6;
        state.gasPpmValues.voc = pow(10, lgPPM);
    } else {
        state.gasPpmValues.voc = NAN;
    }
}

void checkAlarms(DeviceState& state, const DeviceConfig& config) {
    bool anyAlarm = false;
    if (state.tempStatus == SS_NORMAL) {
        if (state.temperature < config.thresholds.tempMin || state.temperature > config.thresholds.tempMax) {
            P_PRINTF("[ALARM] 温度超限! %d°C (范围: %d-%d)\n", state.temperature, config.thresholds.tempMin, config.thresholds.tempMax);
            state.tempStatus = SS_WARNING;
        }
    } else if (state.tempStatus == SS_WARNING) {
        if (state.temperature >= config.thresholds.tempMin && state.temperature <= config.thresholds.tempMax) {
           state.tempStatus = SS_NORMAL;
        }
    }
    if (state.humStatus == SS_NORMAL) {
        if (state.humidity < config.thresholds.humMin || state.humidity > config.thresholds.humMax) {
            P_PRINTF("[ALARM] 湿度超限! %d%% (范围: %d-%d)\n", state.humidity, config.thresholds.humMin, config.thresholds.humMax);
            state.humStatus = SS_WARNING;
        }
    } else if (state.humStatus == SS_WARNING) {
        if (state.humidity >= config.thresholds.humMin && state.humidity <= config.thresholds.humMax) {
            state.humStatus = SS_NORMAL; 
        }
    }
    if (state.gasCoStatus == SS_NORMAL && state.gasPpmValues.co > config.thresholds.coPpmMax) {
        P_PRINTF("[ALARM] CO超限! %.2f PPM (阈值: >%.2f)\n", state.gasPpmValues.co, config.thresholds.coPpmMax);
        state.gasCoStatus = SS_WARNING;
    } else if (state.gasCoStatus == SS_WARNING && state.gasPpmValues.co <= config.thresholds.coPpmMax) {
        state.gasCoStatus = SS_NORMAL;
    }
    if (state.gasNo2Status == SS_NORMAL && state.gasPpmValues.no2 > config.thresholds.no2PpmMax) {
        P_PRINTF("[ALARM] NO2超限! %.2f PPM (阈值: >%.2f)\n", state.gasPpmValues.no2, config.thresholds.no2PpmMax);
        state.gasNo2Status = SS_WARNING;
    } else if (state.gasNo2Status == SS_WARNING && state.gasPpmValues.no2 <= config.thresholds.no2PpmMax) {
        state.gasNo2Status = SS_NORMAL;
    }
    if (state.gasC2h5ohStatus == SS_NORMAL && state.gasPpmValues.c2h5oh > config.thresholds.c2h5ohPpmMax) {
        P_PRINTF("[ALARM] C2H5OH超限! %.2f PPM (阈值: >%.2f)\n", state.gasPpmValues.c2h5oh, config.thresholds.c2h5ohPpmMax);
        state.gasC2h5ohStatus = SS_WARNING;
    } else if (state.gasC2h5ohStatus == SS_WARNING && state.gasPpmValues.c2h5oh <= config.thresholds.c2h5ohPpmMax) {
        state.gasC2h5ohStatus = SS_NORMAL;
    }
    if (state.gasVocStatus == SS_NORMAL && state.gasPpmValues.voc > config.thresholds.vocPpmMax) {
        P_PRINTF("[ALARM] VOC超限! %.2f PPM (阈值: >%.2f)\n", state.gasPpmValues.voc, config.thresholds.vocPpmMax);
        state.gasVocStatus = SS_WARNING;
    } else if (state.gasVocStatus == SS_WARNING && state.gasPpmValues.voc <= config.thresholds.vocPpmMax) {
        state.gasVocStatus = SS_NORMAL;
    }
    
    anyAlarm = (state.tempStatus == SS_WARNING || state.humStatus == SS_WARNING || state.gasCoStatus == SS_WARNING || state.gasNo2Status == SS_WARNING || state.gasC2h5ohStatus == SS_WARNING || state.gasVocStatus == SS_WARNING);

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
    bool isAnySensorInitializing = (state.gasCoStatus == SS_INIT || state.gasNo2Status == SS_INIT ||
                                   state.gasC2h5ohStatus == SS_INIT || state.gasVocStatus == SS_INIT);
    
    DeviceState& mutableState = const_cast<DeviceState&>(state); 

    const unsigned long UNIFIED_BLINK_INTERVAL = 500; // 统一闪烁频率为500ms
    
    if (state.calibrationState == CAL_IN_PROGRESS) {
        if (currentTime - mutableState.lastBlinkTime >= UNIFIED_BLINK_INTERVAL) { 
            mutableState.lastBlinkTime = currentTime; 
            mutableState.ledBlinkState = !mutableState.ledBlinkState;
        }
        colorToSet = mutableState.ledBlinkState ? COLOR_CYAN_VAL : pixels.Color(0,50,50);
    } else if (isAnySensorWarning) { 
        colorToSet = COLOR_RED_VAL; 
    } else if (isAnySensorInitializing) { 
        if (currentTime - mutableState.lastBlinkTime >= UNIFIED_BLINK_INTERVAL) {
            mutableState.lastBlinkTime = currentTime;
            mutableState.ledBlinkState = !mutableState.ledBlinkState;
        }
        colorToSet = mutableState.ledBlinkState ? COLOR_ORANGE_VAL : pixels.Color(100,60,0); 
    } else if (isAnySensorDisconnected) { 
        if (currentTime - mutableState.lastBlinkTime >= UNIFIED_BLINK_INTERVAL) {
            mutableState.lastBlinkTime = currentTime;
            mutableState.ledBlinkState = !mutableState.ledBlinkState;
        }
        colorToSet = mutableState.ledBlinkState ? COLOR_BLUE_VAL : COLOR_OFF_VAL; 
    } else if (wifiStatus.isScanning || wifiStatus.connectProgress == WIFI_CP_CONNECTING || wifiStatus.connectProgress == WIFI_CP_DISCONNECTING) { 
         if (currentTime - mutableState.lastBlinkTime >= UNIFIED_BLINK_INTERVAL) {
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
        } else {
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

void startCalibration() {
    if (currentState.calibrationState == CAL_IN_PROGRESS) {
        P_PRINTLN("[CAL] 校准已在进行中。");
        return;
    }
    P_PRINTLN("[CAL] 收到校准请求，释放信号量以启动任务...");
    xSemaphoreGive(calibrationSemaphore);
}

void calibrationTask(void *pvParameters) {
    for (;;) {
        if (xSemaphoreTake(calibrationSemaphore, portMAX_DELAY) == pdTRUE) {
            P_PRINTLN("[CAL_TASK] 开始校准流程...");
            
            currentState.calibrationState = CAL_IN_PROGRESS;
            currentState.calibrationProgress = 0;
            sendCalibrationStatusToClients();

            if (millis() < gasSensorWarmupEndTime) {
                 P_PRINTLN("[CAL_TASK] 等待传感器预热完成...");
                 while (millis() < gasSensorWarmupEndTime) {
                    currentState.calibrationProgress = (int)((float)millis() / gasSensorWarmupEndTime * 20.0f);
                    sendCalibrationStatusToClients();
                    vTaskDelay(pdMS_TO_TICKS(500));
                 }
            }
            
            GasResistData r0_sum = {0.0, 0.0, 0.0, 0.0};
            int valid_samples[4] = {0, 0, 0, 0};

            P_PRINTLN("[CAL_TASK] 开始采集数据...");
            for (int i = 0; i < CALIBRATION_SAMPLE_COUNT; i++) {
                float current_rs_co = adcToRs(gas_sensor.getGM702B());
                float current_rs_no2 = adcToRs(gas_sensor.getGM102B());
                float current_rs_c2h5oh = adcToRs(gas_sensor.getGM302B());
                float current_rs_voc = adcToRs(gas_sensor.getGM502B());

                if (current_rs_co > 0) { r0_sum.co += current_rs_co; valid_samples[0]++; }
                if (current_rs_no2 > 0) { r0_sum.no2 += current_rs_no2; valid_samples[1]++; }
                if (current_rs_c2h5oh > 0) { r0_sum.c2h5oh += current_rs_c2h5oh; valid_samples[2]++; }
                if (current_rs_voc > 0) { r0_sum.voc += current_rs_voc; valid_samples[3]++; }

                currentState.calibrationProgress = 20 + (int)((float)(i + 1) / CALIBRATION_SAMPLE_COUNT * 80.0f);
                currentState.measuredR0.co = (valid_samples[0] > 0) ? (r0_sum.co / valid_samples[0]) : NAN;
                currentState.measuredR0.no2 = (valid_samples[1] > 0) ? (r0_sum.no2 / valid_samples[1]) : NAN;
                currentState.measuredR0.c2h5oh = (valid_samples[2] > 0) ? (r0_sum.c2h5oh / valid_samples[2]) : NAN;
                currentState.measuredR0.voc = (valid_samples[3] > 0) ? (r0_sum.voc / valid_samples[3]) : NAN;
                
                sendCalibrationStatusToClients();
                vTaskDelay(pdMS_TO_TICKS(CALIBRATION_SAMPLE_INTERVAL_MS));
            }

            P_PRINTLN("[CAL_TASK] 数据采集完成，正在计算并保存...");

            bool success = false;
            if (valid_samples[0] > 0) { currentConfig.r0Values.co = r0_sum.co / valid_samples[0]; success = true; }
            if (valid_samples[1] > 0) { currentConfig.r0Values.no2 = r0_sum.no2 / valid_samples[1]; success = true; }
            if (valid_samples[2] > 0) { currentConfig.r0Values.c2h5oh = r0_sum.c2h5oh / valid_samples[2]; success = true; }
            if (valid_samples[3] > 0) { currentConfig.r0Values.voc = r0_sum.voc / valid_samples[3]; success = true; }

            if (success) {
                saveConfig(currentConfig);
                currentState.calibrationState = CAL_COMPLETED;
                P_PRINTLN("[CAL_TASK] 校准成功并已保存。");
                P_PRINTF("  新R0值 - CO: %.2f, NO2: %.2f, C2H5OH: %.2f, VOC: %.2f\n",
                   currentConfig.r0Values.co, currentConfig.r0Values.no2,
                   currentConfig.r0Values.c2h5oh, currentConfig.r0Values.voc);
            } else {
                currentState.calibrationState = CAL_FAILED;
                P_PRINTLN("[CAL_TASK] 校准失败，没有有效的采样数据。");
            }
            
            sendCalibrationStatusToClients(); 
            
            P_PRINTLN("[CAL_TASK] 3秒后设备将重启以应用新校准值...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            ESP.restart();
        }
    }
}
