#pragma once

// GPIO36 (VP). Older docs used SENSOR_VP; esp32dev pins_arduino.h only defines A0 (=36).
#define SPEED_SENSOR_PIN 36
#define MOTOR_PWM_PIN 26
// ★2026 量測序列改版：已拿掉變速箱與可變負載，SERVO_PIN 不再驅動 SG90，
// 改為數位輸出控制外接 MOSFET 開關(高電位＝把 LOAD_TEST_RESISTOR_OHM 固定
// 測試負載接上發電機)，供安全電流／內阻兩模組共用。沿用原巨集名稱不改名，
// 詳見 settings.h 的 LOAD_SWITCH_* 常數與 measure_seq 模組。
#define SERVO_PIN 23
// BUCK_PWM_PIN 目前未被任何模組使用(原可變負載相關功能已移除)，保留給日後 MPPT/buck 使用。
#define BUCK_PWM_PIN 14

// ★依 KiCad PCB 實際網路(曾與直覺命名對調)：
//   OLED 模組 → /SDA2 /SCL2 → ESP32 IO33 / IO25
//   INA232 U6 → /SDA  /SCL  → ESP32 IO21 / IO22 (A0→GND → I2C 0x40)
#define SDA_PIN 33  // OLED SDA (/SDA2)
#define SCL_PIN 25  // OLED SCL (/SCL2)
#define SDA2_PIN 21 // INA232 SDA (/SDA；板端無外接上拉 → 軟體 I2C 用內部上拉)
#define SCL2_PIN 22 // INA232 SCL (/SCL)
#define START_PIN 15
