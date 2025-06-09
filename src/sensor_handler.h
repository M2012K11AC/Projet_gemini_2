#ifndef SENSOR_HANDLER_H
#define SENSOR_HANDLER_H

#include "data_manager.h"
#include <Adafruit_NeoPixel.h>

// ==========================================================================
// == 函数声明 ==
// ==========================================================================

// -- 硬件初始化与控制 --
void initHardware();
bool isGasSensorConnected();
void updateLedBrightness(uint8_t brightness_percent);
void updateLedStatus(const DeviceState& state, const WifiState& wifiStatus);
void controlBuzzer(DeviceState& state);

// -- 传感器数据处理 --
void readSensors(DeviceState& state);
void checkAlarms(DeviceState& state, const DeviceConfig& config);

#endif // SENSOR_HANDLER_H