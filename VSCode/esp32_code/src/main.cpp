#include <Arduino.h>
#include "HardwareSerial.h"
#include "speed_sensor/speed_sensor.h"
#include "motor_PID/motor_PID.h"

// 除錯輸出週期(ms)；純 RPM 行仍每秒一筆，供上位機繪圖
static constexpr uint32_t SERIAL_DEBUG_MS = 250;
static constexpr uint32_t SERIAL_RPM_MS = 1000;

uint32_t Serial_debug_time;
uint32_t Serial_rpm_time;

// 上次狀態(用於事件觸發列印，避免刷屏)
static uint8_t last_phase = 0xFF;
static uint8_t last_fault_code = 0xFF;
static bool last_ol_hold = false;
static bool last_ready = false;
static bool last_probe_ready = false;
static bool last_tune_active = false;
static bool last_tune_done = false;
static bool last_speed_stable = false;

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
  case FAULT_STALL_WITH_PWM:
    return "STALL_WITH_PWM";
  case FAULT_PWM_MAX_NO_SPEED:
    return "PWM_MAX_NO_SPEED";
  case FAULT_AUTOTUNE_FAIL:
    return "AUTOTUNE_FAIL";
  default:
    return "UNKNOWN";
  }
}

// 狀態變化時立刻印一行事件，方便抓起動/故障瞬間
static void serial_print_events()
{
  const uint8_t phase = settings.init_phase;
  const uint8_t fcode = settings.fault_code;
  const bool hold = settings.ol_hold;
  const bool ready = settings.const_speed_ready;
  const bool probe_rdy = settings.ol_probe_ready;

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

// 週期性狀態總覽(上位機會略過非純數字行)
static void serial_print_debug_status()
{
  Serial.printf(
      "[DBG] t=%lu phase=%s rpm=%.1f valid=%d pwm=%u hold=%d "
      "probe_req=%d probe_rdy=%d probe_rpm=%.1f "
      "settle=%u/%u ready=%d ol_hold_ok=%d speed_stable=%d fault=%d(%s) edges=%lu "
      "keep=%.1f tune=%d/%d tunedNVS=%d Kp=%.4f Ki=%.4f Kd=%.4f\n",
      (unsigned long)millis(),
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
      (double)PID_settings.Kd);
}

void setup()
{
  Serial.begin(115200);
  delay(200); // 等 USB 序列埠就緒，避免開頭訊息被吃掉
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

  Serial_debug_time = millis();
  Serial_rpm_time = millis();

  motor_PID_start(); // 一定要先初始化PID模組再呼叫速度感測模組
  speed_sensor_start();
  Serial.println("[BOOT] tasks started (motor_PID + speed_sensor)");
}

void loop()
{
  // 事件：階段 / hold / probe / ready / fault 變化立刻輸出
  serial_print_events();

  const uint32_t now = millis();

  // 週期除錯狀態
  if (now - Serial_debug_time >= SERIAL_DEBUG_MS)
  {
    serial_print_debug_status();
    Serial_debug_time = now;
  }

  // 上位機相容：每秒一行純 RPM 數值
  if (now - Serial_rpm_time >= SERIAL_RPM_MS)
  {
    Serial.println(settings.now_speed);
    Serial_rpm_time = now;
  }
}
