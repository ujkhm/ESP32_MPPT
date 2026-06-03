#pragma once

#include <Arduino.h>
#include "driver/pulse_cnt.h"
#include "pins/pins.h"
#include "settings/settings.h"
#include "esp32-hal.h"
#include "RTOS/RTOS.h"

//轉速測量模組初始化，並始測量轉速和自動呼叫PID模組(須放在setup()中執行)
void speed_sensor_start();