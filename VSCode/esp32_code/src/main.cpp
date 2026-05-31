#include <Arduino.h>
#include "speed_sensor/speed_sensor.h"


void setup() {
  Serial.begin(115200);
  speed_sensor_init();
}

void loop() {

} 