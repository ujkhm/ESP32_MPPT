#pragma once

#include <Arduino.h>
#include "driver/gpio.h"

// 簡易軟體 I2C(硬體開汲極輸出 + 內部上拉)：
// 用來避開 Arduino-ESP32 3.x 硬體 I2C(NG) 在 NACK 後卡死 ESP_ERR_INVALID_STATE 的問題。
// 板端漏裝外接上拉時，靠 GPIO 內部上拉 + 較慢時序仍可短距通訊。
//
// ★效能關鍵：begin() 時用 ESP-IDF gpio_config() 把腳位設成
// GPIO_MODE_INPUT_OUTPUT_OD(硬體開汲極 + 可讀回)「一次性」設定好；
// 熱路徑(每個 bit)只呼叫 gpio_set_level()/gpio_get_level()(純暫存器存取，
// 數十奈秒等級)，絕不在每個 bit 呼叫 Arduino 的 pinMode()。
// 舊版每個 bit 都呼叫 pinMode() 切換 INPUT_PULLUP/OUTPUT，
// 但 Arduino-ESP32 3.x 的 pinMode() 會經過 periman(週邊管理層)鎖與檢查，
// 開銷極大：實測整張 128x64 OLED 畫面(sendBuffer ~1024+ bytes)
// 用 pinMode() bit-bang 可能耗時 500~700ms，
// 且 board_ui 任務優先權高於 loop()，等於把整個 Core1 卡住，
// 讓 loop() 裡的 START 按鈕輪詢幾乎偵測不到、[DBG] 週期也被拖成 667ms。
class SoftI2C
{
public:
    void begin(uint8_t sda, uint8_t scl, uint32_t freq_hz = 100000)
    {
        sda_ = (gpio_num_t)sda;
        scl_ = (gpio_num_t)scl;

        gpio_config_t cfg = {};
        cfg.pin_bit_mask = (1ULL << sda_) | (1ULL << scl_);
        cfg.mode = GPIO_MODE_INPUT_OUTPUT_OD; // 硬體開汲極，寫1=放開(浮動)、寫0=拉低，且可讀回電平
        cfg.pull_up_en = GPIO_PULLUP_ENABLE;   // 板端無外接上拉 → 靠內部上拉
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&cfg);

        // 半週期延遲(µs)；下限 2µs ≈ 250kHz 理論，無外接上拉建議 <=100kHz
        const uint32_t half = (freq_hz >= 500000) ? 1u : (500000u / freq_hz);
        delay_us_ = (uint8_t)constrain(half, 2u, 20u);

        release_(sda_);
        release_(scl_);
        delayMicroseconds(delay_us_ * 2);
    }

    bool writeReg16(uint8_t addr7, uint8_t reg, uint16_t value)
    {
        if (!start_())
        {
            return false;
        }
        if (!writeByte_((uint8_t)(addr7 << 1))) // W
        {
            stop_();
            return false;
        }
        if (!writeByte_(reg) ||
            !writeByte_((uint8_t)(value >> 8)) ||
            !writeByte_((uint8_t)(value & 0xFF)))
        {
            stop_();
            return false;
        }
        stop_();
        return true;
    }

    bool readReg16(uint8_t addr7, uint8_t reg, uint16_t &value)
    {
        // 寫暫存器指標
        if (!start_())
        {
            return false;
        }
        if (!writeByte_((uint8_t)(addr7 << 1)) || !writeByte_(reg))
        {
            stop_();
            return false;
        }
        stop_();

        // 再讀 2 bytes
        if (!start_())
        {
            return false;
        }
        if (!writeByte_((uint8_t)((addr7 << 1) | 0x01))) // R
        {
            stop_();
            return false;
        }
        const uint8_t msb = readByte_(true);  // ACK
        const uint8_t lsb = readByte_(false); // NACK
        stop_();
        value = (uint16_t)((msb << 8) | lsb);
        return true;
    }

    // 連續寫入(OLED 畫面緩衝用)：addr7 + payload
    bool writeBytes(uint8_t addr7, const uint8_t *data, size_t len)
    {
        if (!startWrite(addr7))
        {
            return false;
        }
        for (size_t i = 0; i < len; i++)
        {
            if (!writeRaw(data[i]))
            {
                endWrite();
                return false;
            }
        }
        endWrite();
        return true;
    }

    // 交易式 API(給 U8g2 byte callback 用)
    bool startWrite(uint8_t addr7)
    {
        if (!start_())
        {
            return false;
        }
        if (!writeByte_((uint8_t)(addr7 << 1)))
        {
            stop_();
            return false;
        }
        return true;
    }

    bool writeRaw(uint8_t data)
    {
        return writeByte_(data);
    }

    void endWrite()
    {
        stop_();
    }

    // 探測單一 7-bit 位址是否 ACK
    bool probe(uint8_t addr7)
    {
        if (!start_())
        {
            return false;
        }
        const bool ack = writeByte_((uint8_t)(addr7 << 1));
        stop_();
        return ack;
    }

    // 掃描 0x03~0x77，找到第一個 ACK 的 7-bit 位址；找不到回傳 0xFF
    uint8_t scanFirst()
    {
        for (uint8_t a = 0x03; a <= 0x77; a++)
        {
            if (probe(a))
            {
                return a;
            }
            delayMicroseconds(50);
        }
        return 0xFF;
    }

private:
    gpio_num_t sda_ = GPIO_NUM_NC;
    gpio_num_t scl_ = GPIO_NUM_NC;
    uint8_t delay_us_ = 5;

    inline void delay_() const
    {
        delayMicroseconds(delay_us_);
    }

    // 開汲極高：放開(浮動)，靠上拉拉高——純暫存器寫入，不呼叫 pinMode()
    inline void release_(gpio_num_t pin) const
    {
        gpio_set_level(pin, 1);
    }

    // 開汲極低：主動拉低——純暫存器寫入
    inline void driveLow_(gpio_num_t pin) const
    {
        gpio_set_level(pin, 0);
    }

    inline bool readLevel_(gpio_num_t pin) const
    {
        return gpio_get_level(pin) != 0;
    }

    bool readBit_() const
    {
        release_(sda_);
        delay_();
        release_(scl_);
        delay_();
        const bool bit = readLevel_(sda_);
        driveLow_(scl_);
        delay_();
        return bit;
    }

    void writeBit_(bool bit) const
    {
        if (bit)
        {
            release_(sda_);
        }
        else
        {
            driveLow_(sda_);
        }
        delay_();
        release_(scl_);
        delay_();
        // 從機時鐘拉伸(簡化：最多等 ~1ms)
        uint16_t stretch = 0;
        while (!readLevel_(scl_) && stretch < 1000)
        {
            delayMicroseconds(1);
            stretch++;
        }
        driveLow_(scl_);
        delay_();
    }

    bool start_()
    {
        release_(sda_);
        release_(scl_);
        delay_();
        if (!readLevel_(sda_) || !readLevel_(scl_))
        {
            // bus 被拉低：嘗試 clock-out 解鎖
            for (int i = 0; i < 9; i++)
            {
                driveLow_(scl_);
                delay_();
                release_(scl_);
                delay_();
            }
            release_(sda_);
            delay_();
            if (!readLevel_(sda_) || !readLevel_(scl_))
            {
                return false;
            }
        }
        driveLow_(sda_);
        delay_();
        driveLow_(scl_);
        delay_();
        return true;
    }

    void stop_()
    {
        driveLow_(sda_);
        delay_();
        release_(scl_);
        delay_();
        release_(sda_);
        delay_();
    }

    bool writeByte_(uint8_t data)
    {
        for (int i = 7; i >= 0; i--)
        {
            writeBit_((data >> i) & 0x01);
        }
        // ACK = SDA 被從機拉低
        return !readBit_();
    }

    uint8_t readByte_(bool ack)
    {
        uint8_t data = 0;
        for (int i = 7; i >= 0; i--)
        {
            if (readBit_())
            {
                data |= (uint8_t)(1u << i);
            }
        }
        writeBit_(!ack); // ACK=0, NACK=1
        return data;
    }
};
