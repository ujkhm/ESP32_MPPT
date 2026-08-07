#pragma once

#include <Arduino.h>
#include "pins/pins.h"
#include "settings/settings.h"
#include "RTOS/RTOS.h"

// OLED 顯示模組(純顯示)：讀 ui_settings.state / settings / ina / PID 畫畫面
// 按鈕與馬達起動/急停請在 main.cpp 處理
bool board_ui_oled_init(); // setup() 主任務初始化 OLED
void board_ui_start();     // 啟動週期刷新任務
