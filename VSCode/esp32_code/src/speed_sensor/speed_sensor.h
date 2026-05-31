#pragma once

#include <Arduino.h>
#include "driver/pcnt.h"
#include "pins/pins.h"
#include "settings/settings.h"
#include "esp32-hal.h"

//轉速測量模組初始化，並等待開始
void speed_sensor_init();

//開始測量轉速，只需在setup裡面執行一次
void speed_sensor_start();