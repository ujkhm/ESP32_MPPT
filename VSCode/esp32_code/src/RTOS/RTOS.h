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
// 量測序列總管(安全電流/內阻/曲線)：只做狀態機、週期性讀值與 GPIO 切換，
// 時間尺度都是幾百 ms 到幾分鐘等級，刻意放低優先權、放 Core1，
// 不與 Core0 的 PID/測速即時控制路徑搶執行時間。
#define RTOS_MEASURE_SEQ_LEVEL 3
#define RTOS_BT_TELEMETRY_LEVEL 2 // 藍牙序列遙測：純推播 JSON，優先權更低

//不指定執行核心
