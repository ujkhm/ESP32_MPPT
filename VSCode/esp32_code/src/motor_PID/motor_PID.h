#pragma once

#include <Arduino.h>
#include "settings/settings.h"
#include <QuickPID.h>
#include "pins/pins.h"
#include "RTOS/RTOS.h"
#include "esp32-hal.h"

// 馬達 PWM / 定速 PID：
// 階梯開環 → 測速就緒 → (可選)sTune 自動調參 → 閉環定速；與測速模組共用失控保護
// 閉環期間：負載通斷與換轉速檔會短暫提高 PI 增益與輸出斜率，其餘時間維持調參值
void motor_PID_start();