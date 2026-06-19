#pragma once
#include <Arduino.h>
#include <Preferences.h> // 用於將變數存入FLASH
// 常數設定

// speed_sensor.h
#define buf_idx_bit 4  // 用於儲存最後幾個數據(位元)
#define space_number 2 // 用於測速之光柵盤總格數

// 變數設定
// speed sensor settings and shared variables
struct speed_sensor
{
    uint16_t read_space = 15;                     // 多久讀取一次速度感測器的值,單位為毫秒(此值亦是PID之計算間隔)
    float now_speed;                              // 現在主動力馬達的轉速(RPM)
    uint8_t buf_idx : buf_idx_bit = 0;            // 測速時只記錄最後4個數據(2^2=4)
    uint32_t cap_buffer[(1 << buf_idx_bit)];      // 用於儲存最後的幾個數據(取決於上面的buf_idx)
    uint8_t count_number : buf_idx_bit;           // 指標快照
    uint16_t Timestamp_state[(1 << buf_idx_bit)]; // 用於儲存cap_buffer陣列中數值的狀態(0:可用 1:不可用)
};
extern volatile speed_sensor settings; // 實體在speed_sensor.cpp中

struct motor_PID
{
    float keep_rpm = 0;        // 目標轉速對應的脈衝次數
    uint32_t pwm_freq = 20000; // PWM頻率
    uint8_t pwm_res = 10;      // PWM解析度(位元)
    float Kp;                  // 以下參數如要修改預設值請到void load()內修改
    float Ki;
    float Kd;
    // 【結構體自我載入】
    void load()
    {
        Preferences prefs;
        prefs.begin("pid_cfg", true); // 只讀模式
        Kp = prefs.getFloat("Kp", 0.0f);
        Ki = prefs.getFloat("Ki", 0.0f);
        Kd = prefs.getFloat("Kd", 0.0f);
        prefs.end();
    }

    // 【結構體自我儲存】
    void save() volatile
    {
        Preferences prefs;
        prefs.begin("pid_cfg", false); // 讀寫模式
        prefs.putFloat("Kp", Kp);
        prefs.putFloat("Ki", Ki);
        prefs.putFloat("Kd", Kd);
        prefs.end();
    }
};
extern volatile motor_PID PID_settings; // 實體在motor_PID.cpp中
