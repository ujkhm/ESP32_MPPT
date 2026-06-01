#pragma once

// GPIO36 (VP). Older docs used SENSOR_VP; esp32dev pins_arduino.h only defines A0 (=36).
#define SPEED_SENSOR_PIN 12                //36
#define MOTOR_PWM_PIN 26
#define SERVO_PIN 23
#define BUCK_PWM_PIN 14
#define SDA_PIN 21                  //OLED SDA
#define SCL_PIN 22                  //OLED SCL
#define SDA2_PIN 33                 //INA232 SDA
#define SCL2_PIN 25                 //INA232 SCL
#define START_PIN 15                
