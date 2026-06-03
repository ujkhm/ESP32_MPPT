#pragma once

// speed sensor settings and shared variables
struct speed_sensor
{
    uint16_t read_space = 10; // 多久讀取一次速度感測器的值,單位為毫秒(此值亦是PID之計算間隔)
    float space_number = 20;  // 用於測速之光柵盤總格數
    float now_speed;          // 現在主動力馬達的轉速(RPM)
    int now_ticks;            // 現在的計數值(脈衝差)
};
extern speed_sensor settings; // 實體在speed_sensor.cpp中

struct motor_PID
{
    int keep_ticls; // 目標轉速對應的脈衝次數
};
extern motor_PID PID_settings;
