#include "speed_sensor.h"
#include "speed_sensor/speed_sensor.h"


// 載入設定和共享變數
speed_sensor settings{};

// 宣告全域指標，用來儲存這個計數器的控制代碼
pcnt_unit_handle_t pcnt_unit = NULL;
pcnt_channel_handle_t pcnt_chan = NULL;

// RTOS系統時間(用於延遲)
TickType_t xLastWakeTime = xTaskGetTickCount();

// 算完脈衝數後用於呼叫PID任務的旗標(實體位於motor_PID.h中)
extern TaskHandle_t xPIDTaskHandle;

uint32_t wait_for_time;                              // 時間旗標
uint32_t now_for_time;                               // 時間快照
int last_count_number = 0;                           // 上一次的計數值
int now_count_number;                                // 現在計數器的值(脈衝次數)
void speed_sensor_init(void *pvParameters) {

  //配置計數器
 pcnt_unit_config_t unit_config = {
    .low_limit = -30000,                             // 硬體計數下限
    .high_limit = 30000,                             // 硬體計數上限
    .flags = {
    .accum_count = true                              // 開啟擴展32位累加計數
    }
  };
pcnt_new_unit(&unit_config, &pcnt_unit);             // 套用配置
 
// 配置計數器通道/腳位設定
pcnt_chan_config_t chan_config = {
    .edge_gpio_num = SPEED_SENSOR_PIN,    // 接收脈衝訊號的 GPIO 腳位
    .level_gpio_num = -1                  // 控制方向的 GPIO 腳位 (-1 代表不使用)
  };
pcnt_new_channel(pcnt_unit, &chan_config, &pcnt_chan);       //套用配置

// 設定觸發模式
pcnt_channel_set_edge_action(
        pcnt_chan, 
        PCNT_CHANNEL_EDGE_ACTION_INCREASE, // 遇到上升沿計數值 +1
        PCNT_CHANNEL_EDGE_ACTION_INCREASE  // 遇到下降沿也+1，提升感測精度
    );

// 設定硬體觸發擴展閾值
pcnt_unit_add_watch_point(pcnt_unit, 30000);
pcnt_unit_add_watch_point(pcnt_unit, -30000);

pcnt_unit_enable(pcnt_unit);       // 啟用 PCNT 硬體電源與時脈
pcnt_unit_clear_count(pcnt_unit);  // 歸零計數值
Serial.println("Speed sensor ready.");
wait_for_time = millis();          // 定位時間
pcnt_unit_start(pcnt_unit);        // 開始計數
Serial.println("Speed sensor started.");

// 開始計數
  while (1) {
    //轉速判斷
    now_for_time = millis();         // 時間快照
    pcnt_unit_get_count(pcnt_unit, &now_count_number);  //讀取計數器值
    settings.now_ticks = now_count_number - last_count_number; // 計算設定時間過了幾個脈衝
    if (xPIDTaskHandle != NULL) {  // 算完脈衝後呼叫PID任務算出對應的主動力馬達輸出值
        xTaskNotifyGive(xPIDTaskHandle); 
    }
    settings.now_speed = (settings.now_ticks * 1000.0f * 60.0f) //計算轉速
                       / (settings.read_space * settings.space_number * 2.0f);

    last_count_number = now_count_number;                    // 更新計數器旗標 
    wait_for_time = now_for_time;                            // 更新時間旗標
    
    xTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(settings.read_space));
  }
}


void speed_sensor_start() {
// 任務設定/分配
xTaskCreatePinnedToCore(speed_sensor_init,
                       "speed_sensor_start",
                        4096,
                        NULL,
                        RTOS_SPEED_SENSOR_LEVEL,
                        NULL,
                        0);

                      }