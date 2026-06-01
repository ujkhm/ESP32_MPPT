#pragma once


//speed sensor settings and shared variables
struct speed_sensor {
    uint16_t read_space = 10; //多久讀取一次速度感測器的值,單位為毫秒
    float space_number = 20; //用於測速之光柵盤總格數
    float now_speed;         //現在的轉速(RPM)
    int now_count_number;    // 現在計數器的值(脈衝次數)
};
extern speed_sensor settings;



