#include "speed_sensor.h"
#include "driver/pcnt.h"
#include "esp32-hal.h"
#include <cstdint>


speed_sensor settings{};


uint64_t wait_for_time;                              //時間旗標
int16_t now_count_number;                            //現在計數器的值
int16_t last_count_number = 0;                       //上一次的計數值
uint64_t now_overflow_number;                        //現在的溢出值
uint64_t last_overflow_number;                       //上一次的溢出值
void speed_sensor_init() {
  
 pcnt_unit_config_t pcnt_config = {
    .pulse_gpio_num = SPEED_SENSOR_PIN,           //設定使用的腳位
    .ctrl_gpio_num = PCNT_PIN_NOT_USED,
    .lctrl_mode = PCNT_MODE_KEEP,
    .hctrl_mode = PCNT_MODE_KEEP,
    .pos_mode = PCNT_COUNT_INC,
    .neg_mode = PCNT_COUNT_DIS,
    .counter_h_lim = 32767,                       //計數器最大值
    .counter_l_lim = -1,                          //計數器最小值
    .unit = PCNT_UNIT_0,                          //使用通道
    .channel = PCNT_CHANNEL_0,
    .flags = {
    .accum_count = true
    }
  };
 
pcnt_unit_config(&pcnt_config);      //啟用配置
pcnt_counter_pause(PCNT_UNIT_0);       //暫停計數
pcnt_counter_clear(PCNT_UNIT_0);       //清除計數
Serial.println("Speed sensor ready.");
}



void speed_sensor_start() {
wait_for_time = millis();                                     //定位時間
pcnt_counter_clear(PCNT_UNIT_0);                   //清除計數
pcnt_counter_resume(PCNT_UNIT_0);                  //開始計數
Serial.println("Speed sensor started.");
  while (1) {                                                //轉速判斷
    if (millis() - wait_for_time >= settings.read_space) {
    pcnt_get_counter_value(PCNT_UNIT_0, &now_count_number);  //讀取計數器值
    
    settings.now_speed = ( (now_count_number - last_count_number)                //計算轉速
                       + (now_overflow_number - last_overflow_number) *32768.0f ) * 1000.0f * 60.0f
                       /(settings.read_space * settings.space_number);

    last_count_number = now_count_number;                    //更新計數器旗標 
    wait_for_time = millis();                                //更新時間旗標
    
    }
  }
}