#include "motor_PID.h"

// 算完脈衝數後用於呼叫PID任務的旗標實體
TaskHandle_t xPIDTaskHandle = NULL;

volatile motor_PID PID_settings{};

void motor_PID_init(void *pvParameters)
{
    // 基礎初始化
    ledcAttach(MOTOR_PWM_PIN, PID_settings.pwm_freq, PID_settings.pwm_res);
    while (1)
    {
        // 這裡放每一次要執行的pid計算
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // 等待任務通知(卡住while迴圈讓cpu時間被釋放)
    }
}

void motor_PID_start()
{
    xTaskCreatePinnedToCore(
        motor_PID_init,
        "motor_PID",
        4096,
        NULL,
        RTOS_PID_LEVEL,
        &xPIDTaskHandle,
        0);
}