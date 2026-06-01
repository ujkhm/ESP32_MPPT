#include <Arduino.h>
#include "HardwareSerial.h"
#include "speed_sensor/speed_sensor.h"


uint32_t Serial_time;

void setup() {
  Serial.begin(115200);
  speed_sensor_init();
  Serial_time = millis();
}



void loop() {
  speed_sensor_start();
  if(millis() - Serial_time >= 1000){
    Serial.println(settings.now_speed);
    Serial_time = millis();
  }
}
