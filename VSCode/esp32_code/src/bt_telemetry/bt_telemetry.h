#pragma once

#include <Arduino.h>
#include "settings/settings.h"
#include "RTOS/RTOS.h"

// 藍牙序列遙測：用 BluetoothSerial(傳統 SPP，Windows 端配對後會出現一個虛擬 COM 埠，
// 上位機可直接用既有的序列埠邏輯開啟)，單向推播 JSON 快照，讓上位機能讀到
// settings.h 內所有跟顯示/報表相關的數據。純推播，不接受/不解析上位機送來的指令
// (本機唯一的實體輸入是板上 START 鈕)。
//
// 訊息格式(每行一個 JSON 物件，換行分隔，供上位機用逐行讀取即可解析)：
//   {"t":"live", ...} 每約 200ms 送一次：所有「即時會變」的欄位
//   {"t":"curve", "n":<count>, "pts":[[rpm,v,p,r_opt], ...]} 曲線算完後每約 3 秒重送一次整張表，
//   讓中途才連上或重新連上的上位機也能拿到完整結果(推播式協定沒有「上位機主動查詢」這個管道)。
void bt_telemetry_start();
