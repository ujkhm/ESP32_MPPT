#pragma once


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


// 所有任務優先級與執行核心

//Core 0
#define RTOS_PID_LEVEL 16
#define RTOS_SPEED_SENSOR_LEVEL 15
// INA232 移到 Core 0：優先權低於 PID/測速，不會搶佔控制迴路；
// 且讓 Core 1(Arduino loop() + UI)完全不受軟體 I2C bit-bang 影響，
// 避免 START 按鈕輪詢被餓死。
#define RTOS_INA232_LEVEL 4 // 電壓/電流感測：優先權低，僅在 PID/測速閒置時執行

//Core 1
#define RTOS_UI_LEVEL 8 // OLED：顯示，優先權高於 Arduino loop()(預設 1)

//不指定執行核心
