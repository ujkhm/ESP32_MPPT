#include "measure_seq.h"
#include "safe_current/safe_current.h"
#include "gen_resistance/gen_resistance.h"
#include "curve_calc/curve_calc.h"

// 共享狀態實體(其他模組只讀，本檔案所在任務是唯一寫入者)
volatile measure_settings meas_settings{};

// ★記憶體保護★：本檔案對 meas_settings 的存取一律透過 settings.h 提供的
// meas_get_x()/meas_set_x() 介面；一次要更新好幾個彼此相關欄位時，用
// SettingsLockGuard 包住整段直接寫欄位。對其他模組(speed_sensor/motor_PID/ina232)
// 一律只用它們既有的 get 介面讀取，需要改變馬達行為時只透過 pid_set_keep_rpm()/
// meas_set_pause_request() 這種「請求」而非直接動它們的鎖，避免巢狀持有兩把鎖。

static void load_switch_apply(bool connected)
{
#if LOAD_SWITCH_ACTIVE_HIGH
    digitalWrite(SERVO_PIN, connected ? HIGH : LOW);
#else
    digitalWrite(SERVO_PIN, connected ? LOW : HIGH);
#endif
}

void load_switch_init()
{
    pinMode(SERVO_PIN, OUTPUT);
    load_switch_apply(false);
    meas_set_load_connected(false);
}

void load_switch_set(bool connected)
{
    load_switch_apply(connected);
    meas_set_load_connected(connected);
}

bool load_switch_is_connected()
{
    return meas_get_load_connected();
}

// ---- 發電機斷線(鱷魚夾脫落)偵測 ----
// 只在本檔案所在任務內使用，不需要放進共享結構。
static bool link_armed = false;
static uint32_t link_low_v_since_ms = 0;

static void reset_link_monitor()
{
    link_armed = false;
    link_low_v_since_ms = 0;
    meas_set_armed(false);
}

// 回傳 true 表示此刻應觸發斷線暫停
static bool check_generator_link(uint32_t now_ms)
{
    if (!link_armed)
    {
        if (speed_get_speed_stable())
        {
            link_armed = true;
            meas_set_armed(true);
        }
        else
        {
            link_low_v_since_ms = 0;
            return false; // 還沒穩過一次，尚不信任電壓判斷(正常爬升期間電壓本來就低)
        }
    }

    const bool ina_ok = ina_get_online() && ina_get_data_valid();
    const float v = ina_ok ? ina_get_bus_V() : 0.0f;
    const bool no_output = (!ina_ok) || (v < (float)GEN_LINK_LOST_V_MAX);

    if (!no_output)
    {
        link_low_v_since_ms = 0;
        return false;
    }
    if (link_low_v_since_ms == 0)
    {
        link_low_v_since_ms = now_ms;
        return false;
    }
    return (now_ms - link_low_v_since_ms) >= (uint32_t)GEN_LINK_LOST_TIMEOUT_MS;
}

static void enter_pause(uint8_t phase_to_resume)
{
    meas_set_resume_phase(phase_to_resume);
    meas_set_pause_request(true); // 通知 motor_PID：立即把 PWM 歸零、停在 PID_PAUSED
    load_switch_set(false);       // 暫停中一律斷開測試負載
    {
        SettingsLockGuard lock(g_meas_mux);
        meas_settings.link_lost = true;
        meas_settings.phase = MEAS_LINK_LOST;
    }
    Serial.printf("[MEAS] generator link lost during phase=%u -> paused, load opened\n",
                  (unsigned)phase_to_resume);
}

// 等待恢復後重新爬升至 speed_stable；回傳 true=已恢復可以繼續，false=逾時或途中故障
static bool wait_for_resume_stable()
{
    const uint32_t wait_start = millis();
    while ((millis() - wait_start) < (uint32_t)SAFE_SPEED_WAIT_TIMEOUT_MS)
    {
        if (speed_get_fault())
        {
            return false;
        }
        if (speed_get_speed_stable())
        {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

static void handle_link_lost_state()
{
    if (!meas_get_resume_requested())
    {
        return;
    }
    meas_set_resume_requested(false);
    meas_set_pause_request(false); // 放行 motor_PID：比照真停轉，會自動重新爬升(不需重新調參)
    reset_link_monitor();
    const uint8_t target = meas_get_resume_phase();
    Serial.printf("[MEAS] resume requested -> waiting speed_stable before restarting phase=%u\n",
                  (unsigned)target);

    if (!wait_for_resume_stable())
    {
        // 還沒等到就逾時或又故障了：留在 LINK_LOST，使用者可以再按一次 START 重試，
        // 不要在轉速還沒回來時就貿然接負載
        meas_set_pause_request(true);
        SettingsLockGuard lock(g_meas_mux);
        meas_settings.link_lost = true;
        meas_settings.phase = MEAS_LINK_LOST;
        return;
    }

    // ★決策：斷線恢復後「整段重新開始目前模組」，不延續中斷前的子步驟/部分進度
    // (已完成的其他模組結果不受影響，例如斷線發生在內阻階段，安全電流的 I_cont 仍然有效)
    if (target == MEAS_SAFE_CURRENT)
    {
        safe_current_reset();
    }
    else if (target == MEAS_RESISTANCE)
    {
        gen_resistance_reset();
    }
    meas_set_phase(target); // MIN_SPEED_HOLD 只是等 speed_stable，不需要額外 reset
}

static void measure_seq_task(void *pvParameters)
{
    (void)pvParameters;

    load_switch_init();
    {
        SettingsLockGuard lock(g_meas_mux);
        meas_settings.phase = MEAS_IDLE;
        meas_settings.resume_phase = MEAS_IDLE;
        meas_settings.session_active = true;
        meas_settings.link_lost = false;
        meas_settings.pause_request = false;
        meas_settings.resume_requested = false;
    }
    safe_current_reset();
    gen_resistance_reset();
    reset_link_monitor();
    meas_set_phase(MEAS_MIN_SPEED_HOLD);
    Serial.println("[MEAS] sequence started, waiting for autotune + speed_stable "
                    "(target rpm is whatever the system naturally settles at, not a fixed constant)");

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        const uint32_t now_ms = millis();

        // 整機硬故障(飛車/看門狗/ESTOP/量測安全鎖定...)：留在原地什麼都不做，等重開機。
        // 不主動碰 pause_request/load，交給既有的 fault 鎖定路徑處理，避免互踩。
        if (speed_get_fault())
        {
            vTaskDelay(pdMS_TO_TICKS(400));
            continue;
        }

        const uint8_t phase_now = meas_get_phase();

        if (phase_now == MEAS_LINK_LOST)
        {
            handle_link_lost_state();
            continue;
        }

        // 斷線監測：只在馬達應該轉、且期待有輸出的三個階段檢查；
        // CURVE_CALC/DONE 馬達已經關閉，MEAS_LINK_LOST 本身已在上面處理，都不需要再看。
        if (phase_now == MEAS_MIN_SPEED_HOLD || phase_now == MEAS_SAFE_CURRENT ||
            phase_now == MEAS_RESISTANCE)
        {
            if (check_generator_link(now_ms))
            {
                enter_pause(phase_now);
                continue;
            }
        }

        switch (phase_now)
        {
        case MEAS_MIN_SPEED_HOLD:
            if (speed_get_speed_stable())
            {
                // ★關鍵：這裡才是「自動調參後系統自然停下來的轉速」第一次真正確立的時刻，
                // 必須在這裡重新呼叫 safe_current_reset() 去捕捉當下的 pid_get_keep_rpm()。
                // task 剛啟動時(見上面 measure_seq_task() 開頭)也呼叫過一次 safe_current_reset()，
                // 但那時開環爬升/自動調參都還沒開始，keep_rpm 當下多半是 0 或殘留的舊值──
                // 若不在這裡重新 reset 一次，SAFE_CURRENT 階段會直接拿那個過期的 0 去命令
                // pid_set_keep_rpm(0)，馬達被慢慢拉停，最終觸發無脈衝看門狗鎖定
                // (實測踩過這個坑，見 SAFETY_MEMORY_REVIEW.md 修訂記錄)。
                safe_current_reset();
                Serial.println("[MEAS] MIN_SPEED_HOLD reached speed_stable -> SAFE_CURRENT");
                meas_set_phase(MEAS_SAFE_CURRENT);
            }
            break;

        case MEAS_SAFE_CURRENT:
            if (safe_current_step(now_ms))
            {
                if (speed_get_fault())
                {
                    break; // 硬故障收尾：外層下一輪的 fault 檢查會接手，這裡不繼續往下推進
                }
                Serial.printf("[MEAS] safe current done: I_cont=%.3fA @ %.0fRPM -> RESISTANCE\n",
                              (double)meas_get_safe_i_cont_A(), (double)meas_get_safe_i_cont_rpm());
                meas_set_phase(MEAS_RESISTANCE);
            }
            break;

        case MEAS_RESISTANCE:
            if (gen_resistance_step(now_ms))
            {
                if (speed_get_fault())
                {
                    break;
                }
                Serial.printf("[MEAS] resistance done: Rth=%.4f ohm ke=%.6f V/RPM -> CURVE_CALC\n",
                              (double)meas_get_res_rth_ohm(), (double)meas_get_res_ke_v_per_rpm());
                // 物理量測到此全部結束：立即安全關閉馬達(比照暫停機制，PWM 立刻歸零，
                // 且本序列後續不會再清除 pause_request，等同永久停在這裡直到重開機/取下發電機)
                load_switch_set(false);
                meas_set_pause_request(true);
                meas_set_phase(MEAS_CURVE_CALC);
            }
            break;

        case MEAS_CURVE_CALC:
            curve_calc_run();
            meas_set_curve_done(true);
            meas_set_phase(MEAS_DONE);
            Serial.println("[MEAS] curve calc done -> DONE, generator can be removed");
            break;

        case MEAS_DONE:
            load_switch_set(false); // 冗餘保險：持續確保斷開
            vTaskDelay(pdMS_TO_TICKS(900));
            break;

        default:
            break;
        }
    }
}

void measure_seq_start()
{
    xTaskCreatePinnedToCore(
        measure_seq_task,
        "measure_seq",
        4096,
        NULL,
        RTOS_MEASURE_SEQ_LEVEL,
        NULL,
        1);
}
