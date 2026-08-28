#pragma once

#include <Arduino.h>
#include "settings/settings.h"

// 內阻識別(INTERNAL_RESISTANCE_ARCH.md)：
// 在高/中/低三個轉速點(取自安全電流已驗證安全的範圍：最高通過檔、中間、起始檔；
// 量測順序為高→中→低，避免安全電流結束後立刻大幅降速)，
// 各自「先量開路電壓 Voc，再短時間接 LOAD_TEST_RESISTOR_OHM 量 V、I，
// R_th=(Voc-V)/I」，取完立刻斷開，不做熱浸泡。三點取平均得到 R_th、k_e(=Voc/rpm)。
// 呼叫端(measure_seq)須確保呼叫本函式時已經在 MEAS_RESISTANCE 階段，
// 且安全電流模組已完成(meas_get_safe_done()==true，提供 I_cont 供接通前預檢)。
void gen_resistance_reset();               // 重置回本模組最初狀態(全新開始或斷線續測都呼叫)
bool gen_resistance_step(uint32_t now_ms); // 執行一次狀態機步進；回傳 true=識別已結束(成功或硬故障)
