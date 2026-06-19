#include "motor_PID.h"

// 算完脈衝數後用於呼叫PID任務的旗標實體
TaskHandle_t xPIDTaskHandle = NULL;
// 載入設定
volatile motor_PID PID_settings{};
// 快照用變數
static float pid_output;          // 輸出到驅動主動力馬達PWM的定時器值
static float pid_input_bridge;    // 輸入到PID模組的(RPM)
static float pid_setpoint_bridge; // 要維持的值(RPM)
// PWM最大值限制
float max_pwm_value = (float)((1 << PID_settings.pwm_res) - 1);

void motor_PID_init(void *pvParameters)
{
    // 基礎初始化

    // 載入初始資料
    PID_settings.save();

    // 配置PID基本參數(輸入,輸出,目標值)
    static QuickPID myPID(&pid_input_bridge, &pid_output, &pid_setpoint_bridge);
    // 配置PID輸出範圍
    myPID.SetOutputLimits(0, ((1 << PID_settings.pwm_res) - 1));
    // 初始化輸出PWM之定時器
    ledcAttach(MOTOR_PWM_PIN, PID_settings.pwm_freq, PID_settings.pwm_res);
    while (1)
    {
        // 等待任務通知(卡住while迴圈讓cpu時間被釋放)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // 底下放每一次要執行的pid計算

        // 先做快照
        pid_input_bridge = settings.now_speed;
        pid_setpoint_bridge = PID_settings.keep_rpm;

        // 開始計算
        myPID.Compute();

        // 將計算結果寫入控制馬達輸出的變數
        uint32_t final_pwm = (uint32_t)constrain(pid_output, 0.0f, max_pwm_value);

        // 輸出對應PWM
        ledcWrite(MOTOR_PWM_PIN, final_pwm);
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