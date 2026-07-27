#pragma once

#include <Arduino.h>
#include "driver/mcpwm_cap.h"
#include "pins/pins.h"
#include "settings/settings.h"
#include "esp32-hal.h"
#include "RTOS/RTOS.h"

// 轉速測量模組初始化，並開始測量轉速和自動呼叫PID模組(須放在setup()中執行)
// PID 未介入前：階梯升 PWM → 等待 → 擷取第一筆可用轉速(初始化補丁)
// 達 (RPM_INIT_READABLE+餘量) 後固定 PWM，觀察至 READY_SETTLE_MIN_SAMPLES → READY；
// 並與 motor_PID 共用失控保護
void speed_sensor_start();