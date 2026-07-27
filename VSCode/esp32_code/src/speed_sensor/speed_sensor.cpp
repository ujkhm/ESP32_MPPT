#include "speed_sensor.h"

// 載入設定和共享變數
volatile speed_sensor settings{};

// 宣告全域指標，用來儲存這個計數器的控制代碼
mcpwm_cap_timer_handle_t cap_timer = NULL;
mcpwm_cap_channel_handle_t cap_chan = NULL;

// RTOS系統時間(用於延遲)
TickType_t xLastWakeTime = xTaskGetTickCount();

// 算完脈衝數後用於呼叫PID任務的旗標(實體位於motor_PID.cpp中)
extern TaskHandle_t xPIDTaskHandle;

// 宣告一個 ESP-IDF 的多核心鎖（互斥鎖）用於數據快照時關閉中斷
static portMUX_TYPE my_mutex = portMUX_INITIALIZER_UNLOCKED;

uint32_t cap_buffer_Snapshot[(1 << buf_idx_bit)]; // 脈衝時間戳快照

// 編譯時先計算出常數，就不用每次MCU還要再算一遍
// MCPWM Capture 預設時鐘 80MHz → RPM = (80e6 * 60) / 一整圈 ticks
constexpr float RPM_CONSTANT = (80000000ULL * 60ULL);
constexpr uint8_t IDX_MASK = (1 << buf_idx_bit) - 1; // 環形遮罩

// 兩次觸發間隔若小於此 tick 數(換算轉速遠超 RPM_RUNAWAY_MAX)，視為彈跳/雜訊直接丟棄
constexpr uint32_t MIN_EDGE_TICKS =
    (uint32_t)(RPM_CONSTANT / (RPM_RUNAWAY_MAX * (float)EDGES_PER_REV * MIN_EDGE_TICKS_SAFETY_RPM_MULT));

// 任務端狀態(不需給 ISR 使用)
static uint32_t last_edge_count = 0;           // 上次已處理到的絕對邊緣數
static float last_valid_rpm = 0.0f;            // 最近一次可信轉速(正常模式保持用)
static uint32_t last_edge_ms = 0;              // 最近一次邊緣到達的 millis
static uint16_t settle_samples_local = 0;      // 開環固定 PWM 後，已觀察到的同位置整圈樣本數(本地副本)
static uint32_t no_pulse_pwm_ms = 0;           // 有 PWM 卻無脈衝的累計時間(失控保護)
static uint32_t last_protect_ms = 0;
static uint32_t probe_arm_edge = 0;            // probe 請求當下的邊緣計數(只收之後的新資料)
static bool probe_armed = false;
static float speed_filter_ema = 0.0f;          // 同位置整圈轉速(rpm_same)的 EMA 濾波值(給 keep_rpm/PID 使用)
static bool speed_filter_inited = false;       // 濾波器是否已灌入初值

// 時間差變數
uint32_t time_diff;

// 將本地就緒樣本數同步到共享結構(供除錯/上位機觀察)
static void publish_settle_progress()
{
  settings.settle_samples = settle_samples_local;
}

// 先宣告回乎函數的原型
static bool mcpwm_cap_cb(mcpwm_cap_channel_handle_t cap_chan,
                         const mcpwm_capture_event_data_t *edata,
                         void *user_data);

// 重置「開環固定 PWM 後的就緒觀察」進度(尚未固定 PWM，或固定後又真停轉時呼叫)
static void reset_settle_progress()
{
  settle_samples_local = 0;
  speed_filter_inited = false; // 重新起動時，EMA 濾波器也要跟著重新起算
  publish_settle_progress();
}

// 置位失控保護(與 motor_PID 共用；輸出切斷由 PID 任務執行)
static void trip_fault(uint8_t code)
{
  settings.fault = true;
  settings.fault_code = code;
  settings.const_speed_ready = false;
  settings.ol_hold = false;
  settings.ol_probe_request = false;
  settings.ol_probe_ready = false;
  settings.init_phase = SPEED_PHASE_FAULT;
  settings.speed_valid = false;
  settings.speed_stable = false; // 異常時對外穩調旗標必須為 false
  // 立刻歸零並清濾波狀態，避免觸發瞬間的異常值(如彈跳造成的離譜轉速)殘留污染後續輸出
  settings.now_speed = 0.0f;
  last_valid_rpm = 0.0f;
  speed_filter_inited = false;
}

// 開環固定 PWM(LEARNING)後，每來一個新的「同位置整圈」樣本就累計一次觀察進度，
// 達到 READY_SETTLE_MIN_SAMPLES 後才允許離開 LEARNING 進入 READY / 定速
static void count_settle_sample()
{
  if (settle_samples_local < 0xFFFF)
  {
    settle_samples_local++;
  }
  publish_settle_progress();
}

// 同位置整圈量測
static float rpm_from_same_slot(const uint32_t *snap, uint8_t now_idx)
{
  const uint8_t last_idx = (uint8_t)((now_idx - EDGES_PER_REV) & IDX_MASK);
  time_diff = snap[now_idx] - snap[last_idx];
  if (time_diff == 0)
  {
    return 0.0f;
  }
  return RPM_CONSTANT / (float)time_diff;
}

// 初始化測速補丁：
// 低轉速時一般輪詢容易讀到「保持舊值/空窗」，因此在 ol_probe_request 期間
// 只接受「請求當下之後」出現的第一筆新同位置資料作為當前可信轉速
static void try_latch_init_probe(float rpm_same, bool got_new_same_slot, uint32_t edge_count_snap)
{
  if (!settings.ol_probe_request)
  {
    probe_armed = false;
    return;
  }
  if (settings.ol_probe_ready)
  {
    return;
  }

  // 請求剛升起：鎖存當下邊緣數，之後只收更新的資料
  if (!probe_armed)
  {
    probe_arm_edge = edge_count_snap;
    probe_armed = true;
    return; // 本週期不採用(可能仍是 settle 前舊邊緣)
  }

  if (!got_new_same_slot || rpm_same <= 0.0f)
  {
    return;
  }
  if (edge_count_snap <= probe_arm_edge)
  {
    return;
  }

  settings.ol_probe_rpm = rpm_same;
  settings.ol_probe_ready = true;
  settings.now_speed = rpm_same;
  settings.speed_valid = true;
  last_valid_rpm = rpm_same;
  probe_armed = false;
}

// 失控保護檢查(測速與輸出協調)
static void runaway_protect(uint32_t now_ms, bool has_new_edge)
{
  if (settings.fault)
  {
    return;
  }

  // 飛車：轉速超限
  if (settings.speed_valid && settings.now_speed > RPM_RUNAWAY_MAX)
  {
    trip_fault(FAULT_RUNAWAY_RPM);
    return;
  }

  // 「有 PWM 卻無脈衝」只在已固定 duty 或已定速後啟用
  // (階梯升速初期低 duty 可能暫時無邊緣，避免誤跳)
  const uint32_t dt = (last_protect_ms == 0) ? settings.read_space : (now_ms - last_protect_ms);
  last_protect_ms = now_ms;

  const bool protect_stall =
      (settings.ol_hold || settings.const_speed_ready) && (settings.ol_pwm_cmd > 0);

  if (protect_stall && !has_new_edge)
  {
    no_pulse_pwm_ms += dt;
    if (no_pulse_pwm_ms >= (uint32_t)FAULT_STALL_WITH_PWM_MS)
    {
      trip_fault(FAULT_STALL_WITH_PWM);
      return;
    }
  }
  else
  {
    no_pulse_pwm_ms = 0;
  }
}

// 依就緒觀察進度更新階段旗標(階梯/probe 由 motor_PID 主導)
static void update_phase_after_sample(bool got_new_same_slot)
{
  if (settings.fault)
  {
    settings.init_phase = SPEED_PHASE_FAULT;
    settings.const_speed_ready = false;
    settings.speed_stable = false;
    return;
  }

  // 固定 PWM 後進入學習
  if (settings.ol_hold)
  {
    if (settle_samples_local >= READY_SETTLE_MIN_SAMPLES)
    {
      // 測速就緒；若 motor 正在/已調參，保留其 PID_TUNE / PID_RUN 階段不被覆蓋
      if (settings.init_phase != SPEED_PHASE_PID_TUNE &&
          settings.init_phase != SPEED_PHASE_PID_RUN)
      {
        settings.init_phase = SPEED_PHASE_READY;
      }
      settings.const_speed_ready = true;
      settings.rpm_stable = true;
    }
    else
    {
      settings.init_phase = SPEED_PHASE_LEARNING;
      settings.const_speed_ready = false;
      settings.speed_stable = false;
    }
    return;
  }

  // 尚未 hold：由 motor 的 settle / probe 旗標反映階段
  if (settings.ol_probe_request && !settings.ol_probe_ready)
  {
    settings.init_phase = SPEED_PHASE_PROBE;
  }
  else if (!settings.ol_hold)
  {
    settings.init_phase = SPEED_PHASE_STEP_UP;
  }

  settings.const_speed_ready = false;
  settings.speed_stable = false;
  (void)got_new_same_slot;
}

void speed_sensor_init(void *pvParameters)
{
  // 硬體就緒；就緒觀察進度歸零，等開環停在可讀轉速後再開始累計
  reset_settle_progress();
  settings.min_measurable_rpm = RPM_INIT_READABLE;
  settings.init_phase = SPEED_PHASE_STEP_UP;
  settings.const_speed_ready = false;
  settings.rpm_stable = false;
  settings.speed_stable = false;
  settings.fault = false;
  settings.fault_code = FAULT_NONE;
  settings.ol_hold = false;
  settings.ol_probe_request = false;
  settings.ol_probe_ready = false;
  settings.ol_probe_rpm = 0.0f;
  last_edge_ms = millis();
  last_protect_ms = last_edge_ms;
  no_pulse_pwm_ms = 0;

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
    // 延時(時間取決於設定值，位於settings.h中)
    xTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(settings.read_space));

    // 已故障：維持安全值，不再嘗試計算轉速(轉子可能仍在自然減速，避免殘留雜訊污染輸出)，
    // 待重新開機才會重新初始化
    if (settings.fault)
    {
      settings.now_speed = 0.0f;
      settings.speed_valid = false;
      last_valid_rpm = 0.0f;
      speed_filter_inited = false;
      if (xPIDTaskHandle != NULL)
      {
        xTaskNotifyGive(xPIDTaskHandle);
      }
      continue;
    }

    // 先做快照
    uint32_t edge_count_snap = 0;
    portENTER_CRITICAL(&my_mutex); // 關閉中斷
    settings.count_number = settings.buf_idx;
    edge_count_snap = settings.edge_count;
    for (int i = 0; i < (1 << buf_idx_bit); i++)
    {
      cap_buffer_Snapshot[i] = settings.cap_buffer[i];
    }
    portEXIT_CRITICAL(&my_mutex); // 開啟中斷

    const uint8_t now_idx = (uint8_t)((settings.count_number - 1) & IDX_MASK);
    const uint32_t now_ms = millis();
    const bool has_new_edge = (edge_count_snap != last_edge_count);

    float rpm_same = 0.0f;
    bool got_new_same_slot = false;

    if (has_new_edge)
    {
      if (edge_count_snap >= (uint32_t)(EDGES_PER_REV + 1))
      {
        rpm_same = rpm_from_same_slot(cap_buffer_Snapshot, now_idx);
        if (rpm_same > 0.0f)
        {
          got_new_same_slot = true;

          // 初始化 probe 補丁：只在請求後鎖存第一筆「新」同位置轉速
          try_latch_init_probe(rpm_same, got_new_same_slot, edge_count_snap);

          // 輸出轉速策略：一律採用「同位置整圈」量測(rpm_same)，只加一層輕量 EMA 平滑。
          // 「同位置整圈」量測是用同一個物理光柵位置量測整整一圈後的時間，天生就把每格的
          // 加工誤差完全抵銷，精度與原始需求(用同位置保證精度)完全一致；相較於曾經嘗試過的
          // 『單格(1/4 圈)』瞬時補償量測，時間基準短了 4 倍，會把 ISR/彈跳/時鐘量化的相對
          // 抖動、以及定速時馬達真實的瞬時角速度漲落放大暴露出來，配合本機台 PWM→RPM
          // 增益極高(約每 1 count 對應 10+ RPM)，這種被放大的雜訊經比例項直接回饋，正是
          // 先前 PID_RUN 階段轉速劇烈震盪、久久無法收斂的根本原因之一，故不採用。
          if (!speed_filter_inited)
          {
            speed_filter_ema = rpm_same;
            speed_filter_inited = true;
          }
          else
          {
            speed_filter_ema += SPEED_FILTER_ALPHA * (rpm_same - speed_filter_ema);
          }
          const float rpm_out = speed_filter_ema;

          // ★診斷探針★：定速運行中若單一週期轉速跳變異常大，印一行事件方便回頭定位根因，
          // 幫助分辨是感測端(邊緣漏跳/雜訊/debounce誤丟)還是馬達物理上真的轉速突變。
          // 以本機台 process gain(~10RPM/count)搭配 15ms 週期估計，正常動態下單週期變化
          // 很難超過此值，故此閾值僅用於標記「異常大」事件，不影響任何控制邏輯。
          // 必須用「更新前」的 last_valid_rpm 做比較基準，且要在 rpm_out 寫回前判斷。
          if (settings.init_phase == SPEED_PHASE_PID_RUN && last_valid_rpm > 1.0f)
          {
            const float jump = fabsf(rpm_out - last_valid_rpm);
            if (jump > SPEED_JUMP_WARN_RPM)
            {
              Serial.printf("[EVT] speed jump %.1f -> %.1f (d=%.1f) edges=%lu gap_ms=%lu rawSame=%.1f\n",
                            (double)last_valid_rpm, (double)rpm_out, (double)jump,
                            (unsigned long)edge_count_snap,
                            (unsigned long)(now_ms - last_edge_ms),
                            (double)rpm_same);
            }
          }

          // probe 等待期間：未鎖存前不要用「保持舊值」冒充當前轉速
          if (!(settings.ol_probe_request && !settings.ol_probe_ready))
          {
            settings.now_speed = rpm_out;
            last_valid_rpm = rpm_out;
            settings.speed_valid = true;
          }

          settings.Timestamp_state[now_idx] = 1;

          // 固定開環 PWM 後，累計「就緒觀察」樣本數(見 count_settle_sample())
          if (settings.ol_hold && settings.init_phase == SPEED_PHASE_LEARNING)
          {
            count_settle_sample();
          }
        }
      }

      last_edge_count = edge_count_snap;
      last_edge_ms = now_ms;
    }
    else
    {
      // 無新邊緣
      if (settings.ol_probe_request && !settings.ol_probe_ready)
      {
        // 初始化補丁：probe 視窗內不回報過期保持值，避免誤判「已經夠快」
        settings.speed_valid = false;
        // now_speed 維持上次顯示即可，決策以 ol_probe_ready 為準
      }
      else if ((now_ms - last_edge_ms) <= SPEED_STALL_TIMEOUT_MS)
      {
        // ★診斷探針★：定速運行中若一段時間沒有新邊緣(但還沒到 stall 判定)，印一次事件，
        // 方便回頭比對是否與轉速跳變事件同時發生(感測端訊號中斷的直接證據)
        if (settings.init_phase == SPEED_PHASE_PID_RUN)
        {
          const uint32_t gap = now_ms - last_edge_ms;
          if (gap == SPEED_EDGE_GAP_WARN_MS) // 剛跨過門檻時只印一次，避免每輪刷屏
          {
            Serial.printf("[EVT] edge gap warn %lums (last_rpm=%.1f)\n",
                          (unsigned long)gap, (double)last_valid_rpm);
          }
        }
        settings.now_speed = last_valid_rpm;
        settings.speed_valid = (last_valid_rpm > 0.0f);
      }
      else
      {
        settings.now_speed = 0.0f;
        settings.speed_valid = false;
        last_valid_rpm = 0.0f;
        // 就緒觀察未完成就真停轉：清掉進度，下次開環重來
        if (!settings.const_speed_ready && settle_samples_local < READY_SETTLE_MIN_SAMPLES)
        {
          reset_settle_progress();
          settings.ol_hold = false;
          settings.rpm_stable = false;
        }
        if (settings.const_speed_ready)
        {
          settings.const_speed_ready = false;
        }
        settings.speed_stable = false; // 停轉/不可信 → 未穩調
      }
    }

    update_phase_after_sample(got_new_same_slot);
    runaway_protect(now_ms, has_new_edge);

    // 每個測速週期喚醒 PID
    if (xPIDTaskHandle != NULL)
    {
      xTaskNotifyGive(xPIDTaskHandle);
    }
  }
}

// 中斷服務函式 (Callback)
static bool IRAM_ATTR mcpwm_cap_cb(mcpwm_cap_channel_handle_t cap_chan,
                                   const mcpwm_capture_event_data_t *edata,
                                   void *user_data)
{
  // 去彈跳：與上次觸發間隔小於 MIN_EDGE_TICKS 視為機械彈跳/雜訊，直接丟棄不計入，
  // 避免這種極短的假邊緣污染「同位置整圈」的時間基準(進而誤觸發飛車保護)
  static uint32_t last_cap_ticks = 0;
  static bool has_last_cap = false;
  const uint32_t now_ticks = edata->cap_value;
  if (has_last_cap && ((now_ticks - last_cap_ticks) < MIN_EDGE_TICKS))
  {
    return false;
  }
  last_cap_ticks = now_ticks;
  has_last_cap = true;

  // 獲取硬體鎖存的計時器數值 (Ticks)
  settings.cap_buffer[settings.buf_idx] = edata->cap_value;
  // 更新狀態陣列內的標示
  settings.Timestamp_state[settings.buf_idx] = 0;
  // 更新指標
  settings.buf_idx = settings.buf_idx + 1;
  // 絕對邊緣計數
  settings.edge_count = settings.edge_count + 1;
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
