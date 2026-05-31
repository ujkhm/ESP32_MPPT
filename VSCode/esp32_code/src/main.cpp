#include <Arduino.h>
#include "speed_sensor/speed_sensor.h"

void setup() {
  Serial.begin(115200);
  speed_sensor_init();
}



void loop() {
  Serial.println("Hello, ESP32!");
  delay(1000);
}
