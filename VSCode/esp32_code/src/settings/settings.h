#pragma once


//speed sensor settings
struct speed_sensor {
    uint16_t read_space = 10; //多久讀取一次速度感測器的值,單位為毫秒
    uint16_t space_number = 20; //用於測速之光柵盤總格數
    float now_speed;         //現在的轉速(RP)
};




