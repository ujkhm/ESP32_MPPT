#pragma once

#include <Arduino.h>
#include "settings/settings.h"
#include <QuickPID.h>
#include "pins/pins.h"
#include "RTOS/RTOS.h"
#include "esp32-hal.h"

// 馬達 PWM / 定速 PID：
// 階梯開環 → 測速就緒 → (可選)sTune 自動調參 → 閉環定速；與測速模組共用失控保護
void motor_PID_start();