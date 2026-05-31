#include "speed_sensor.h"


void speed_sensor_init() {
 
 pcnt_config_t pcnt_config = {
    .pulse_gpio_num = SPEED_SENSOR_PIN,
    .ctrl_gpio_num = PCNT_PIN_NOT_USED,
    .lctrl_mode = PCNT_MODE_KEEP,
    .hctrl_mode = PCNT_MODE_KEEP,
    .pos_mode = PCNT_COUNT_INC,
    .neg_mode = PCNT_COUNT_DIS,
    .counter_h_lim = 32767,
    .counter_l_lim = -32768,
    .unit = PCNT_UNIT_0,
    .channel = PCNT_CHANNEL_0,
 };
 
 
    Serial.println("Speed sensor initialized");
}