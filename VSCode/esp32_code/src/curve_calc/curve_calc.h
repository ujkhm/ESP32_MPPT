#pragma once

#include <Arduino.h>
#include "settings/settings.h"

// 曲線＋極限轉速計算(合併 LIMIT_RPM_ARCH.md 與最初的曲線需求)：
// 純計算，不驅動馬達、不碰負載開關、不改變任何轉速。
// 讀取內阻模組的 R_th/k_e 與安全電流模組的 I_cont，算出：
//   n_knee = 2*I_cont*R_th/k_e         (最大功率點仍合法的轉速上限)
//   n_voc  = CURVE_V_ALLOW/k_e         (開路耐壓上限；本機會開路，故取這個而非 n_sat)
//   n_rl   = I_cont*(R_th+LOAD_TEST_RESISTOR_OHM)/k_e (固定測試電阻的功率封頂，僅供參考)
//   n_lim  = min(n_knee, n_voc, SAFE_RPM_MAX_CEILING, 若有跳刷／主動力轉不到則再與上一通過檔取較嚴)
// 再從(無條件進位到 100 的倍數之)最低可用轉速起，每 CURVE_RPM_STEP(100RPM)一步算到 n_lim，
// 每點依「是否還在最大功率點合法範圍」分兩種公式，寫入 curve_rpm/v_at_maxp/p_max/r_opt。
// 呼叫端(measure_seq)須確保呼叫本函式時安全電流與內阻模組都已成功完成。
void curve_calc_run();
