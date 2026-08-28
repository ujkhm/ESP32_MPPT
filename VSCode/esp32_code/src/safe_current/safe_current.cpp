#include "safe_current.h"
#include "measure_seq/measure_seq.h"
#include "pins/pins.h"

// 本檔案只在自己的任務(measure_seq 建立)裡被呼叫，不會被其他任務並行呼叫，
// 因此這幾個「本檔案內部才需要知道」的觀察窗變數用純 static 區域變數即可，
// 不必放進共享結構(meas_settings 已經有 safe_droop_ratio 這個「結果」可供對外顯示)。
static uint32_t judge_window_start_ms = 0;
static float judge_window_start_A = 0.0f;

static void goto_phase(uint8_t ph)
{
    meas_set_safe_phase(ph);
    meas_set_safe_phase_start_ms(millis());
}

void safe_current_reset()
{
    load_switch_set(false);
    // ★第 0 檔的起點必須是「這次開機、自動調參後系統自然停下來的轉速」(pid_get_keep_rpm())，
    // 不可以用 MEASURE_MIN_RPM 這個常數：本機台增益極不均勻，sTune 是對自然停下來的那個
    // 工作點做特性化，硬把系統拉去另一個沒被特性化過的轉速，實測會長時間震盪不收斂。
    const float start_rpm = pid_get_keep_rpm();
    {
        SettingsLockGuard lock(g_meas_mux);
        meas_settings.safe_phase = SAFE_PH_PREP;
        meas_settings.safe_start_rpm = start_rpm;
        meas_settings.safe_target_rpm = start_rpm;
        meas_settings.safe_oc_voltage_V = 0.0f;
        meas_settings.safe_electrical_A = 0.0f;
        meas_settings.safe_hot_A = 0.0f;
        meas_settings.safe_droop_ratio = 0.0f;
        meas_settings.safe_phase_start_ms = millis();
        meas_settings.safe_done = false;
        meas_settings.safe_pass_any = false;
        meas_settings.safe_i_cont_A = 0.0f;
        meas_settings.safe_i_cont_rpm = 0.0f;
        meas_settings.safe_last_pass_rpm = 0.0f;
        meas_settings.safe_last_pass_A = 0.0f;
    }
    judge_window_start_ms = 0;
    judge_window_start_A = 0.0f;
}

static void finish_with_result(float i_cont_A, float i_cont_rpm)
{
    SettingsLockGuard lock(g_meas_mux);
    meas_settings.safe_i_cont_A = i_cont_A;
    meas_settings.safe_i_cont_rpm = i_cont_rpm;
    meas_settings.safe_done = true;
}

// 完全失敗(連第一檔都不通過，或途中偵測到接線/量測異常)：整機安全鎖定，比照 ESTOP，需重開機。
// ★呼叫 speed_trigger_fault() 後立即自行 ledcWrite(0)：不等 motor_PID 任務下一輪才反應。
static void fail_hard(const char *reason)
{
    load_switch_set(false);
    Serial.printf("[SAFE_I] HARD FAIL: %s\n", reason);
    speed_trigger_fault(FAULT_MEASURE_SAFETY);
    ledcWrite(MOTOR_PWM_PIN, 0);
}

bool safe_current_step(uint32_t now_ms)
{
    const uint8_t ph = meas_get_safe_phase();
    const uint32_t elapsed = now_ms - meas_get_safe_phase_start_ms();

    switch (ph)
    {
    case SAFE_PH_PREP:
    {
        load_switch_set(false);
        const float target = meas_get_safe_target_rpm();
        if (target > (float)SAFE_RPM_MAX_CEILING)
        {
            if (meas_get_safe_pass_any())
            {
                finish_with_result(meas_get_safe_last_pass_A(), meas_get_safe_last_pass_rpm());
                return true;
            }
            fail_hard("reached SAFE_RPM_MAX_CEILING before any rung passed (ceiling set too low?)");
            return true;
        }
        pid_set_keep_rpm(target);
        goto_phase(SAFE_PH_WAIT_SPEED);
        return false;
    }

    case SAFE_PH_WAIT_SPEED:
        if (speed_get_fault())
        {
            return false; // 交給 measure_seq 外層的 fault 檢查處理，這裡不重複判斷
        }
        if (speed_get_speed_stable())
        {
            goto_phase(SAFE_PH_OC_SAMPLE);
            return false;
        }
        if (elapsed > speed_wait_timeout_ms(meas_get_safe_target_rpm()))
        {
            fail_hard("timeout waiting for speed_stable");
            return true;
        }
        return false;

    case SAFE_PH_OC_SAMPLE:
        // 負載在 PREP 已確保斷開；這裡只需等電氣穩定夠久再讀 Voc，並確認負載真的沒接上
        if (elapsed < (uint32_t)SAFE_OC_SAMPLE_MS)
        {
            return false;
        }
        if (!(ina_get_online() && ina_get_data_valid()))
        {
            fail_hard("INA offline during open-circuit sample");
            return true;
        }
        {
            const float oc_a = fabsf(ina_get_current_A());
            if (oc_a > (float)SAFE_OC_MAX_CURRENT_A)
            {
                fail_hard("open-circuit current not near zero (load stuck closed?)");
                return true;
            }
            // 補償發電機→防反接 SS54→INA232 這條路徑上的順向壓降，換回發電機端子的
            // 真實 Voc(見 settings.h 的 ss54_compensate_voltage_V() 說明)。開路時電流
            // 很小，補償量本來就不大，但一起做才能跟帶載那筆用同一套基準。
            meas_set_safe_oc_voltage_V(ss54_compensate_voltage_V(ina_get_bus_V(), oc_a));
        }
        goto_phase(SAFE_PH_CONNECT);
        return false;

    case SAFE_PH_CONNECT:
        if (!load_switch_is_connected())
        {
            load_switch_set(true);
            return false; // 下一輪開始累計 LOAD_SWITCH_SETTLE_MS+SAFE_ELECTRICAL_SETTLE_MS
        }
        // 已接通：settle 期間也持續監看硬電流，不必等窗口結束才檢查
        if (ina_get_online() && ina_get_data_valid())
        {
            const float i_now = fabsf(ina_get_current_A());
            if (i_now > (float)SAFE_I_HARD_CEILING_A)
            {
                fail_hard("hard current ceiling exceeded while settling");
                return true;
            }
        }
        if (elapsed < (uint32_t)(LOAD_SWITCH_SETTLE_MS + SAFE_ELECTRICAL_SETTLE_MS))
        {
            return false;
        }
        if (!(ina_get_online() && ina_get_data_valid()))
        {
            fail_hard("INA offline right after connecting load");
            return true;
        }
        {
            const float i_elec = fabsf(ina_get_current_A());
            if (i_elec < (float)SAFE_I_MIN_VALID_A)
            {
                fail_hard("current too small right after connecting (load not making contact?)");
                return true;
            }
            meas_set_safe_electrical_A(i_elec);
            judge_window_start_ms = now_ms;
            judge_window_start_A = i_elec;
        }
        goto_phase(SAFE_PH_THERMAL_SOAK);
        return false;

    case SAFE_PH_THERMAL_SOAK:
    {
        if (speed_get_fault())
        {
            return false;
        }
        if (!(ina_get_online() && ina_get_data_valid()))
        {
            fail_hard("INA offline during thermal soak");
            return true;
        }
        const float i_now = fabsf(ina_get_current_A());
        if (i_now > (float)SAFE_I_HARD_CEILING_A)
        {
            fail_hard("hard current ceiling exceeded during thermal soak");
            return true;
        }

        if ((now_ms - judge_window_start_ms) >= (uint32_t)SAFE_THERMAL_CHECK_WINDOW_MS)
        {
            const float drop = judge_window_start_A - i_now;
            const float ratio = (judge_window_start_A > 1e-6f) ? (drop / judge_window_start_A) : 0.0f;
            if (fabsf(ratio) <= (float)SAFE_I_PASS_DROOP_RATIO)
            {
                meas_set_safe_hot_A(i_now);
                meas_set_safe_droop_ratio(ratio);
                goto_phase(SAFE_PH_JUDGE);
                return false;
            }
            // 還沒打平：滑動觀察窗，繼續看，直到打平或撞到 SAFE_THERMAL_SOAK_MAX_MS 總逾時
            judge_window_start_ms = now_ms;
            judge_window_start_A = i_now;
        }

        const uint32_t phase_elapsed = now_ms - meas_get_safe_phase_start_ms();
        if (phase_elapsed >= (uint32_t)SAFE_THERMAL_SOAK_MAX_MS)
        {
            const float drop = judge_window_start_A - i_now;
            const float ratio = (judge_window_start_A > 1e-6f) ? (drop / judge_window_start_A) : 1.0f;
            meas_set_safe_hot_A(i_now);
            meas_set_safe_droop_ratio(ratio); // 逾時仍未打平：無論算出多少一律走 JUDGE 的「不通過」分支
            goto_phase(SAFE_PH_JUDGE);
            return false;
        }
        return false;
    }

    case SAFE_PH_JUDGE:
    {
        load_switch_set(false); // 判定前先斷開，不再繼續加熱
        // safe_droop_ratio 在 THERMAL_SOAK 只有兩種寫入時機：觀察窗內打平(ratio 已 ≤ 通過門檻)，
        // 或總逾時仍未打平(ratio 當時就已經 > 通過門檻，才會落到逾時分支)——因此這裡單看
        // ratio 是否 ≤ 通過門檻，兩種情況都能正確分流，不需要另外分辨「是否逾時」。
        const float ratio = fabsf(meas_get_safe_droop_ratio());
        const float rpm_now = meas_get_safe_target_rpm();
        const float hot_a = meas_get_safe_hot_A();

        if (ratio <= (float)SAFE_I_PASS_DROOP_RATIO)
        {
            // 本檔通過
            {
                SettingsLockGuard lock(g_meas_mux);
                meas_settings.safe_pass_any = true;
                meas_settings.safe_last_pass_rpm = rpm_now;
                meas_settings.safe_last_pass_A = hot_a;
            }
            const float next_rpm = rpm_now + (float)SAFE_RPM_STEP;
            // 電流約正比轉速外推下一檔，預估是否已經會撞硬天花板；撞到或已達最高轉速就收尾，
            // 不必真的接上去撞
            const float predicted_next_a = (rpm_now > 1.0f) ? (hot_a * (next_rpm / rpm_now)) : hot_a;
            if (predicted_next_a >= (float)SAFE_I_HARD_CEILING_A || next_rpm > (float)SAFE_RPM_MAX_CEILING)
            {
                finish_with_result(hot_a, rpm_now);
                return true;
            }
            meas_set_safe_target_rpm(next_rpm);
            goto_phase(SAFE_PH_COOLDOWN);
            return false;
        }

        // 本檔不通過(打平但下垂偏大，或逾時仍未打平)
        if (meas_get_safe_pass_any())
        {
            finish_with_result(meas_get_safe_last_pass_A(), meas_get_safe_last_pass_rpm());
            return true;
        }
        fail_hard("first rung failed thermal droop check — cannot establish any safe continuous current");
        return true;
    }

    case SAFE_PH_COOLDOWN:
        if (elapsed >= (uint32_t)SAFE_COOLDOWN_MS)
        {
            goto_phase(SAFE_PH_PREP);
        }
        return false;

    default:
        goto_phase(SAFE_PH_PREP);
        return false;
    }
}
