#pragma once

#include <Arduino.h>
#include "pins/pins.h"
#include "settings/settings.h"
#include "RTOS/RTOS.h"

// INA232 電壓/電流/功率感測：
// - 使用軟體 I2C(SDA2/SCL2 + 內部上拉)，避開硬體 I2C NG 的 INVALID_STATE
// - 晶片 AVG=16；帶載 V/I 對不上時 current_plausible=false，該筆不得寫進測試結果
// - 須在 setup() 呼叫 ina232_start()
void ina232_start();
