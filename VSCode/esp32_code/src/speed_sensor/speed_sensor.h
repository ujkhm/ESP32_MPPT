#pragma once

#include <Arduino.h>
#include "driver/pulse_cnt.h"
#include "pins/pins.h"
#include "settings/settings.h"
#include "esp32-hal.h"
#include "RTOS/RTOS.h"

//轉速測量模組初始化，並等開始測量轉速
void speed_sensor_init();

//需要重複執行
void speed_sensor_start();