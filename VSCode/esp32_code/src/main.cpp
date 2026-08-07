#include <Arduino.h>
#include "HardwareSerial.h"
#include "driver/gpio.h"
#include "speed_sensor/speed_sensor.h"
#include "motor_PID/motor_PID.h"
#include "ina232/ina232.h"
#include "board_ui/board_ui.h"

static constexpr uint32_t SERIAL_DEBUG_MS = 250;
static constexpr uint32_t SERIAL_RPM_MS = 1000;

uint32_t Serial_debug_time;
uint32_t Serial_rpm_time;

static uint8_t last_phase = 0xFF;
static uint8_t last_fault_code = 0xFF;
static bool last_ol_hold = false;
static bool last_ready = false;
static bool last_probe_ready = false;
static bool last_tune_active = false;
static bool last_tune_done = false;
static bool last_speed_stable = false;
static uint8_t last_ui_state = 0xFF;

// ---- START 鈕(SW2 → IO15，對 GND；板端無外接上拉) ----
static volatile bool start_irq_pending = false;
static uint32_t start_last_accept_ms = 0;
static bool start_raw_last = false;
static bool start_stable = false;
static bool start_prev = false;
static uint32_t start_last_change_ms = 0;

static void IRAM_ATTR start_isr()
{
  start_irq_pending = true;
}

static bool start_raw_pressed()
{
#if START_ACTIVE_LOW
  return digitalRead(START_PIN) == LOW;
#else
  return digitalRead(START_PIN) == HIGH;
#endif
}

static void start_pin_init()
{
  pinMode(START_PIN, INPUT_PULLUP);
  gpio_pullup_en((gpio_num_t)START_PIN);
  gpio_pulldown_dis((gpio_num_t)START_PIN);
  delay(5);

  start_raw_last = start_raw_pressed();
  start_stable = start_raw_last;
  start_prev = start_stable;
  start_last_change_ms = millis();

  attachInterrupt(digitalPinToInterrupt(START_PIN), start_isr, CHANGE);

  Serial.printf("[MAIN] START pin=IO%u pullup ON level=%d pressed=%d "
                "(idle expect level=1; press→0)\n",
                (unsigned)START_PIN, digitalRead(START_PIN), (int)start_raw_pressed());
}

static void trigger_estop()
{
  settings.fault = true;
  settings.fault_code = FAULT_ESTOP;
  settings.const_speed_ready = false;
  settings.ol_hold = false;
  settings.ol_probe_request = false;
  settings.ol_probe_ready = false;
  settings.init_phase = SPEED_PHASE_FAULT;
  settings.speed_valid = false;
  settings.speed_stable = false;
  settings.ol_pwm_cmd = 0;
  PID_settings.autotune_active = false;
  ledcWrite(MOTOR_PWM_PIN, 0);

  ui_settings.state = UI_ESTOP;
  Serial.println("[MAIN] ESTOP locked — reboot required");
}

static void start_motor_control()
{
  if (ui_settings.motor_started)
  {
    return;
  }
  motor_PID_start();
  speed_sensor_start();
  ui_settings.motor_started = true;
  ui_settings.state = UI_RUNNING;
  Serial.println("[MAIN] START → motor_PID + speed_sensor started");
}

// 去彈跳後偵測按下邊緣；ISR 旗標加速喚醒，仍以穩定電平為準
static void handle_start_button(uint32_t now_ms)
{
  if (start_irq_pending)
  {
    start_irq_pending = false;
  }

  const bool raw = start_raw_pressed();
  if (raw != start_raw_last)
  {
    Serial.printf("[MAIN] START raw %d -> %d @%lums (gpio=%d)\n",
                  (int)start_raw_last, (int)raw, (unsigned long)now_ms,
                  digitalRead(START_PIN));
    start_raw_last = raw;
    start_last_change_ms = now_ms;
  }
  else if ((now_ms - start_last_change_ms) >= (uint32_t)START_DEBOUNCE_MS)
  {
    start_stable = start_raw_last;
  }

  const bool edge = (start_stable && !start_prev);
  start_prev = start_stable;
  if (!edge)
  {
    return;
  }

  // 額外保護：兩次有效動作至少間隔 debounce*2
  if ((now_ms - start_last_accept_ms) < (uint32_t)(START_DEBOUNCE_MS * 2))
  {
    return;
  }
  start_last_accept_ms = now_ms;

  Serial.printf("[MAIN] START edge accepted, state=%u\n", (unsigned)ui_settings.state);

  if (ui_settings.state == UI_WAIT_START)
  {
    start_motor_control();
  }
  else if (ui_settings.state == UI_RUNNING)
  {
    trigger_estop();
  }
}

static const char *phase_name(uint8_t phase)
{
  switch (phase)
  {
  case SPEED_PHASE_STEP_UP:
    return "STEP_UP";
  case SPEED_PHASE_PROBE:
    return "PROBE";
  case SPEED_PHASE_LEARNING:
    return "LEARNING";
  case SPEED_PHASE_READY:
    return "READY";
  case SPEED_PHASE_FAULT:
    return "FAULT";
  case SPEED_PHASE_PID_TUNE:
    return "PID_TUNE";
  case SPEED_PHASE_PID_RUN:
    return "PID_RUN";
  default:
    return "UNKNOWN";
  }
}

static const char *fault_name(uint8_t code)
{
  switch (code)
  {
  case FAULT_NONE:
    return "NONE";
  case FAULT_RUNAWAY_RPM:
    return "RUNAWAY_RPM";
  case FAULT_NO_PULSE_TIMEOUT:
    return "NO_PULSE_TIMEOUT";
  case FAULT_PWM_MAX_NO_SPEED:
    return "PWM_MAX_NO_SPEED";
  case FAULT_AUTOTUNE_FAIL:
    return "AUTOTUNE_FAIL";
  case FAULT_ESTOP:
    return "ESTOP";
  default:
    return "UNKNOWN";
  }
}

static const char *ui_state_name(uint8_t st)
{
  switch (st)
  {
  case UI_WAIT_START:
    return "WAIT_START";
  case UI_RUNNING:
    return "RUNNING";
  case UI_ESTOP:
    return "ESTOP";
  default:
    return "UNKNOWN";
  }
}

static void serial_print_events()
{
  const uint8_t phase = settings.init_phase;
  const uint8_t fcode = settings.fault_code;
  const bool hold = settings.ol_hold;
  const bool ready = settings.const_speed_ready;
  const bool probe_rdy = settings.ol_probe_ready;

  if (ui_settings.state != last_ui_state)
  {
    Serial.printf("[EVT] ui %s -> %s\n",
                  ui_state_name(last_ui_state), ui_state_name(ui_settings.state));
    last_ui_state = ui_settings.state;
  }
  if (phase != last_phase)
  {
    Serial.printf("[EVT] phase %s -> %s\n",
                  phase_name(last_phase), phase_name(phase));
    last_phase = phase;
  }
  if (hold != last_ol_hold)
  {
    Serial.printf("[EVT] ol_hold %d -> %d (pwm=%u probe_rpm=%.1f)\n",
                  (int)last_ol_hold, (int)hold,
                  (unsigned)settings.ol_pwm_cmd,
                  (double)settings.ol_probe_rpm);
    last_ol_hold = hold;
  }
  if (probe_rdy != last_probe_ready)
  {
    if (probe_rdy)
    {
      Serial.printf("[EVT] probe latched rpm=%.1f (need>=%.1f)\n",
                    (double)settings.ol_probe_rpm,
                    (double)(RPM_INIT_READABLE + RPM_INIT_MARGIN));
    }
    last_probe_ready = probe_rdy;
  }
  if (ready != last_ready)
  {
    Serial.printf("[EVT] const_speed_ready %d -> %d\n",
                  (int)last_ready, (int)ready);
    last_ready = ready;
  }
  if (PID_settings.autotune_active != last_tune_active)
  {
    Serial.printf("[EVT] autotune_active %d -> %d\n",
                  (int)last_tune_active, (int)PID_settings.autotune_active);
    last_tune_active = PID_settings.autotune_active;
  }
  if (PID_settings.autotune_done != last_tune_done)
  {
    Serial.printf("[EVT] autotune_done %d -> %d (Kp=%.5f Ki=%.5f Kd=%.5f)\n",
                  (int)last_tune_done, (int)PID_settings.autotune_done,
                  (double)PID_settings.Kp, (double)PID_settings.Ki,
                  (double)PID_settings.Kd);
    last_tune_done = PID_settings.autotune_done;
  }
  if (settings.speed_stable != last_speed_stable)
  {
    Serial.printf("[EVT] speed_stable %d -> %d (rpm=%.1f keep=%.1f)\n",
                  (int)last_speed_stable, (int)settings.speed_stable,
                  (double)settings.now_speed, (double)PID_settings.keep_rpm);
    last_speed_stable = settings.speed_stable;
  }
  if (fcode != last_fault_code)
  {
    if (settings.fault)
    {
      Serial.printf("[EVT] FAULT code=%u (%s) rpm=%.1f pwm=%u\n",
                    (unsigned)fcode, fault_name(fcode),
                    (double)settings.now_speed,
                    (unsigned)settings.ol_pwm_cmd);
    }
    else if (last_fault_code != 0xFF)
    {
      Serial.printf("[EVT] fault cleared\n");
    }
    last_fault_code = fcode;
  }
}

static void serial_print_debug_status()
{
  Serial.printf(
      "[DBG] t=%lu ui=%s start=%d/%d phase=%s rpm=%.1f valid=%d pwm=%u hold=%d "
      "probe_req=%d probe_rdy=%d probe_rpm=%.1f "
      "settle=%u/%u ready=%d ol_hold_ok=%d speed_stable=%d fault=%d(%s) edges=%lu "
      "keep=%.1f tune=%d/%d tunedNVS=%d Kp=%.4f Ki=%.4f Kd=%.4f "
      "V=%.2f I=%.3f P=%.2f ina=%d/%d n=%lu\n",
      (unsigned long)millis(),
      ui_state_name(ui_settings.state),
      digitalRead(START_PIN),
      (int)start_raw_pressed(),
      phase_name(settings.init_phase),
      (double)settings.now_speed,
      (int)settings.speed_valid,
      (unsigned)settings.ol_pwm_cmd,
      (int)settings.ol_hold,
      (int)settings.ol_probe_request,
      (int)settings.ol_probe_ready,
      (double)settings.ol_probe_rpm,
      (unsigned)settings.settle_samples,
      (unsigned)READY_SETTLE_MIN_SAMPLES,
      (int)settings.const_speed_ready,
      (int)settings.rpm_stable,
      (int)settings.speed_stable,
      (int)settings.fault,
      fault_name(settings.fault_code),
      (unsigned long)settings.edge_count,
      (double)PID_settings.keep_rpm,
      (int)PID_settings.autotune_active,
      (int)PID_settings.autotune_done,
      (int)PID_settings.tuned,
      (double)PID_settings.Kp,
      (double)PID_settings.Ki,
      (double)PID_settings.Kd,
      (double)ina_settings.bus_V,
      (double)ina_settings.current_A,
      (double)ina_settings.power_W,
      (int)ina_settings.online,
      (int)ina_settings.data_valid,
      (unsigned long)ina_settings.sample_count);
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("[BOOT] ESP32 micro-generator firmware");
  Serial.printf("[BOOT] init_need_rpm>=%.1f settle=%ums probe_timeout=%ums "
                "pwm_step=%.0f%% max_ol=%.0f%% runaway=%.0f\n",
                (double)(RPM_INIT_READABLE + RPM_INIT_MARGIN),
                (unsigned)MOTOR_STEP_SETTLE_MS,
                (unsigned)MOTOR_PROBE_TIMEOUT_MS,
                (double)(MOTOR_PWM_STEP_RATIO * 100.0f),
                (double)(MOTOR_STARTUP_PWM_MAX_RATIO * 100.0f),
                (double)RPM_RUNAWAY_MAX);
  Serial.printf("[BOOT] PID autotune en=%d every_boot=%d settle=%us test=%us "
                "samples=%u step=%.0f%% rule=NoOvershoot_PI action=direct5T pmode=pOnError "
                "gain_units=real verbose=%d\n",
                (int)PID_AUTOTUNE_ENABLE, (int)PID_AUTOTUNE_EVERY_BOOT,
                (unsigned)PID_TUNE_SETTLE_SEC, (unsigned)PID_TUNE_TEST_SEC,
                (unsigned)PID_TUNE_SAMPLES,
                (double)(PID_TUNE_STEP_RATIO * 100.0f),
                (int)PID_TUNE_SERIAL_VERBOSE);
  Serial.printf("[BOOT] tune_estop=%.0f pre_settle=%ums gain_scale=%.2f "
                "slew_max=%u/cycle speed_filter_alpha=%.2f\n",
                (double)(RPM_RUNAWAY_MAX * PID_TUNE_ESTOP_RATIO),
                (unsigned)PID_TUNE_PRE_SETTLE_MS,
                (double)PID_TUNE_GAIN_SCALE,
                (unsigned)PID_OUTPUT_SLEW_MAX,
                (double)SPEED_FILTER_ALPHA);
  Serial.printf("[BOOT] I2C OLED=soft INA=soft@%uHz | INA addr=0x%02X Rshunt=%.3fΩ Imax=%.2fA\n",
                (unsigned)I2C_INA_FREQ_HZ,
                (unsigned)INA232_I2C_ADDR,
                (double)INA232_RSHUNT_OHM, (double)INA232_IMAX_A);
  Serial.println("[BOOT] START handled in main; UI is display-only");

  Serial_debug_time = millis();
  Serial_rpm_time = millis();

  start_pin_init();
  board_ui_oled_init();
  board_ui_start();
  ina232_start();
  Serial.println("[BOOT] tasks started (board_ui display + ina232)");
}

void loop()
{
  const uint32_t now = millis();

  // START：起動 / 急停
  handle_start_button(now);

  // 其他模組 fault → UI 跟著進急停畫面
  if ((ui_settings.state == UI_RUNNING) && settings.fault)
  {
    ui_settings.state = UI_ESTOP;
  }

  serial_print_events();

  if (now - Serial_debug_time >= SERIAL_DEBUG_MS)
  {
    serial_print_debug_status();
    Serial_debug_time = now;
  }

  if (now - Serial_rpm_time >= SERIAL_RPM_MS)
  {
    Serial.println(settings.now_speed);
    Serial_rpm_time = now;
  }

  // ★關鍵：loop() 之前完全沒有任何讓出 CPU 的呼叫(無 delay/yield)，
  // 會把 Core1 的 IDLE 任務餓死 → 觸發 Task Watchdog，造成週期性長時間卡頓
  // (訊息斷斷續續、間隔越來越長，且 START 按鍵在卡頓期間完全偵測不到)。
  // 1ms 對按鈕手動操作毫無影響，但足以餵飽 WDT、讓排程恢復正常。
  vTaskDelay(pdMS_TO_TICKS(1));
}
