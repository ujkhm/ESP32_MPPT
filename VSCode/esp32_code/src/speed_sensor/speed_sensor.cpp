#include "speed_sensor.h"

// 載入設定和共享變數
volatile speed_sensor settings{};

// 宣告全域指標，用來儲存這個計數器的控制代碼
mcpwm_cap_timer_handle_t cap_timer = NULL;
mcpwm_cap_channel_handle_t cap_chan = NULL;

// RTOS系統時間(用於延遲)
TickType_t xLastWakeTime = xTaskGetTickCount();

// 算完脈衝數後用於呼叫PID任務的旗標(實體位於motor_PID.h中)
extern TaskHandle_t xPIDTaskHandle;

// 宣告一個 ESP-IDF 的多核心鎖（互斥鎖）用於數據快照時關閉中斷
static portMUX_TYPE my_mutex = portMUX_INITIALIZER_UNLOCKED;

uint32_t cap_buffer_Snapshot[(1 << buf_idx_bit)]; // 脈衝時間戳快照

// 編譯時先計算出常數，就不用每次MCU還要再算一遍
constexpr uint64_t RPM_CONSTANT = (80000000ULL * 60ULL * 4ULL) / (2 * space_number);

// 時間差變數
uint64_t time_diff;

// 先宣告回乎函數的原型
static bool mcpwm_cap_cb(mcpwm_cap_channel_handle_t cap_chan,
                         const mcpwm_capture_event_data_t *edata,
                         void *user_data);

void speed_sensor_init(void *pvParameters)
{

  // 配置時鐘
  mcpwm_capture_timer_config_t timer_config = {};
  timer_config.clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT; // 預設 80MHz
  timer_config.group_id = 0;
  mcpwm_new_capture_timer(&timer_config, &cap_timer); // 啟用設定

  // 配置讀取相關設定
  mcpwm_capture_channel_config_t chan_config = {};
  chan_config.gpio_num = SPEED_SENSOR_PIN;
  chan_config.prescale = 1;                                      // 不分頻
  chan_config.flags.pos_edge = 1;                                // 啟用上升沿
  chan_config.flags.neg_edge = 1;                                // 啟用下降沿 (雙沿觸發)
  mcpwm_new_capture_channel(cap_timer, &chan_config, &cap_chan); // 啟用設定

  // 註冊回呼函式 (ISR)
  mcpwm_capture_event_callbacks_t cbs = {};
  cbs.on_cap = mcpwm_cap_cb; // 指定你的 ISR 函式名稱
  mcpwm_capture_channel_register_event_callbacks(cap_chan, &cbs, NULL);

  // 啟用並開始測量
  mcpwm_capture_channel_enable(cap_chan);
  mcpwm_capture_timer_enable(cap_timer);
  mcpwm_capture_timer_start(cap_timer);

  // 開始計數
  while (1)
  {
    // 延時(時間取決於設定值，位於settings.h中)，剛啟動時脈衝沒那麼快進來所以放最前面
    xTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(settings.read_space));
    // 先做快照
    portENTER_CRITICAL(&my_mutex); // 關閉中斷
    settings.count_number = settings.buf_idx;
    for (int i = 0; i < (1 << buf_idx_bit); i++)
    {
      cap_buffer_Snapshot[i] = settings.cap_buffer[i];
    }
    portEXIT_CRITICAL(&my_mutex);                        // 開啟中斷
    constexpr uint8_t idx_mask = (1 << buf_idx_bit) - 1; // 環形遮罩
    // 計算時間戳差
    time_diff = cap_buffer_Snapshot[(settings.count_number - (space_number * 2)) & idx_mask] -
                cap_buffer_Snapshot[settings.count_number];
    // 轉速計算
    if (time_diff != 0) // 預防除以0的狀況
    {
      settings.now_speed = RPM_CONSTANT / time_diff;
    }
    else
    {
      settings.now_speed = 0;
    }
  }
}

// 中斷服務函式 (Callback)
static bool IRAM_ATTR mcpwm_cap_cb(mcpwm_cap_channel_handle_t cap_chan,
                                   const mcpwm_capture_event_data_t *edata,
                                   void *user_data)
{

  // 獲取硬體鎖存的計時器數值 (Ticks)
  settings.cap_buffer[settings.buf_idx] = edata->cap_value;
  // 更新指標
  settings.buf_idx = settings.buf_idx + 1;
  return false;
}

void speed_sensor_start()
{
  // 任務設定/分配
  xTaskCreatePinnedToCore(speed_sensor_init,
                          "speed_sensor_start",
                          4096,
                          NULL,
                          RTOS_SPEED_SENSOR_LEVEL,
                          NULL,
                          0);
}