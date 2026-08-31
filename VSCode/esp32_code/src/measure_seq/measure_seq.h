#pragma once

#include <Arduino.h>
#include "settings/settings.h"
#include "pins/pins.h"
#include "RTOS/RTOS.h"

// 量測序列總管：
//   等自動調參後系統自然穩定(speed_stable) → 安全電流 → 內阻 → 曲線＋極限轉速計算 → 關閉馬達。
//   ★不會、也不可以硬把轉速命令去 MEASURE_MIN_RPM 這個常數，理由見 settings.h 該常數註解。
// 同一個任務裡也監看發電機／負載鱷魚夾脫落：非故障性暫停，重按 START 從中斷的那一檔／那一點續測
// (已通過的安全電流檔與已量到的內阻點都保留)。須在 START 按下、motor/speed 任務已啟動後再呼叫。
void measure_seq_start();
// 負載沒接上／INA 沒讀到電流等夾子鬆脫：暫停馬達與負載，等 START 續測。不可當硬故障鎖定。
void meas_request_contact_pause(const char *reason);
// 帶載電壓還在、電流與 V/R 對不上：同樣暫停續測，但不當夾子鬆脫；本檔／本點數據作廢重做。
void meas_request_ina_mismatch_pause(const char *reason);

// ---- 負載開關(沿用 pins.h 的 SERVO_PIN，不改名；高電位=接上 LOAD_TEST_RESISTOR_OHM) ----
// 供 safe_current / gen_resistance 兩個子模組共用；本檔(measure_seq.cpp)是唯一的
// 實體操作者，其餘模組一律透過這 3 個函式間接存取，不直接碰 SERVO_PIN。
void load_switch_init();              // 上電/重設用，確保腳位方向與預設「斷開」電位
void load_switch_set(bool connected); // 切換測試負載通斷(冪等，可重複呼叫同一狀態)
bool load_switch_is_connected();      // 目前是否已接通(等同 meas_get_load_connected())
