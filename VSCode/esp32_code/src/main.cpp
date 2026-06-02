#include <Arduino.h>
#include "HardwareSerial.h"
#include "speed_sensor/speed_sensor.h"


uint32_t Serial_time;

void setup() {
  Serial.begin(115200);
  Serial_time = millis();
  speed_sensor_start();
}



void loop() {

  if(millis() - Serial_time >= 1000){
    Serial.println(settings.now_speed);
    Serial_time = millis();
  }
}
