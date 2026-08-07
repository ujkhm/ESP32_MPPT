#pragma once

// GPIO36 (VP). Older docs used SENSOR_VP; esp32dev pins_arduino.h only defines A0 (=36).
#define SPEED_SENSOR_PIN 36
#define MOTOR_PWM_PIN 26
#define SERVO_PIN 23
#define BUCK_PWM_PIN 14

// ★依 KiCad PCB 實際網路(曾與直覺命名對調)：
//   OLED 模組 → /SDA2 /SCL2 → ESP32 IO33 / IO25
//   INA232 U6 → /SDA  /SCL  → ESP32 IO21 / IO22 (A0→GND → I2C 0x40)
#define SDA_PIN 33  // OLED SDA (/SDA2)
#define SCL_PIN 25  // OLED SCL (/SCL2)
#define SDA2_PIN 21 // INA232 SDA (/SDA；板端無外接上拉 → 軟體 I2C 用內部上拉)
#define SCL2_PIN 22 // INA232 SCL (/SCL)
#define START_PIN 15
