#include <Arduino.h>
#include "HardwareSerial.h"
#include "speed_sensor/speed_sensor.h"
#include "motor_PID/motor_PID.h"

uint32_t Serial_time;

void setup()
{
  Serial.begin(115200);
  Serial_time = millis();
  motor_PID_start(); // 一定要先初始化PID模組再呼叫速度感測模組
  speed_sensor_start();
}

void loop()
{

  if (millis() - Serial_time >= 1000)
  {
    Serial.println(settings.now_speed);
    Serial_time = millis();
  }
}
