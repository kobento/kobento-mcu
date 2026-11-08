// The board's own speaker: an ES8311 codec (I2C 0x18 on the shared bus) on I2S1, DAC only,
// into an FM8002E amplifier enabled by IO1 (low = on). I2S1 is master at 48 kHz, 16-bit
// stereo (the ES8311 plays the left slot), MCLK 256 Fs on IO4, BCLK IO5, LRCK IO7, data IO8.
//
// The register values are those of Espressif's esp_codec_dev es8311 driver for slave mode,
// MCLK from the pin at 12.288 MHz, 48 kHz, 16-bit I2S.
#include "board/board.h"
#include "audio.h"

#include <algorithm>
#include <cmath>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "es8311";

namespace board {

static constexpr gpio_num_t kMclk = GPIO_NUM_4, kBclk = GPIO_NUM_5, kLrck = GPIO_NUM_7,
                            kDout = GPIO_NUM_8, kPaEnable = GPIO_NUM_1;
static constexpr int kRate = 48000;

static i2s_chan_handle_t sTx;
static i2c_master_dev_handle_t sCodec;

static bool w(uint8_t r, uint8_t v) {
    const uint8_t b[2] = {r, v};
    return i2c_master_transmit(sCodec, b, 2, 50) == ESP_OK;
}

void speakerInit() {
    gpio_reset_pin(kPaEnable);
    gpio_set_direction(kPaEnable, GPIO_MODE_OUTPUT);
    gpio_set_level(kPaEnable, 1);   // amplifier off while the codec starts

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    cc.dma_frame_num = 240;
    if (i2s_new_channel(&cc, &sTx, nullptr) != ESP_OK) { ESP_LOGW(TAG, "I2S1 failed"); return; }
    i2s_std_config_t sc = {};
    sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRate);
    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    sc.gpio_cfg.mclk = kMclk;
    sc.gpio_cfg.bclk = kBclk;
    sc.gpio_cfg.ws = kLrck;
    sc.gpio_cfg.dout = kDout;
    sc.gpio_cfg.din = I2S_GPIO_UNUSED;
    if (i2s_channel_init_std_mode(sTx, &sc) != ESP_OK || i2s_channel_enable(sTx) != ESP_OK) {
        ESP_LOGW(TAG, "I2S1 setup failed");
        sTx = nullptr;
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    sCodec = i2cDevice(0x18);
    if (!sCodec) { ESP_LOGW(TAG, "no ES8311 at 0x18"); return; }
    // open
    w(0x44, 0x08); w(0x44, 0x08);   // I2C noise immunity (written twice, as Espressif does)
    w(0x01, 0x30); w(0x02, 0x00); w(0x03, 0x10); w(0x16, 0x24); w(0x04, 0x10); w(0x05, 0x00);
    w(0x0B, 0x00); w(0x0C, 0x00); w(0x10, 0x1F); w(0x11, 0x7F);
    w(0x00, 0x80);                  // slave, power on
    w(0x01, 0x3F);                  // MCLK from the pin, all clocks on
    w(0x13, 0x10); w(0x1B, 0x0A); w(0x1C, 0x6A);
    // clocks: 12.288 MHz MCLK, 48 kHz (pre_div 1, x1, adc/dac div 1, OSR 0x10, LRCK 256)
    w(0x02, 0x00); w(0x05, 0x00); w(0x03, 0x10); w(0x04, 0x10);
    w(0x07, 0x00); w(0x08, 0xFF); w(0x06, 0x03);
    // format: I2S, 16-bit, in and out
    w(0x09, 0x0C); w(0x0A, 0x0C);
    // start the DAC path
    w(0x17, 0xBF); w(0x0E, 0x02); w(0x12, 0x00); w(0x14, 0x1A); w(0x0D, 0x01);
    w(0x15, 0x40); w(0x37, 0x08); w(0x45, 0x00);
    w(0x31, 0x00);                  // DAC unmuted
    w(0x32, 0xB4);                  // DAC volume: 0xBF = 0 dB, 0.5 dB steps (-5.5 dB)
    gpio_set_level(kPaEnable, 0);   // amplifier on
    ESP_LOGI(TAG, "ES8311 up: speaker at 48 kHz");
}

bool speakerReady() { return sTx && sCodec; }

void speakerVolume(float v) {   // 0..1 -> -95.5 dB .. 0 dB
    if (!sCodec) return;
    w(0x32, (uint8_t) std::lround(std::fmax(0, std::fmin(1, v)) * 0xBF));
}

void speakerWrite(const int16_t* stereo, int frames) {
    if (!sTx) return;
    size_t written = 0;
    i2s_channel_write(sTx, stereo, (size_t) frames * 4, &written, portMAX_DELAY);
}

void speakerClock(bool on) {   // a test: stop I2S1 (and with it the speaker codec's MCLK)
    if (sTx) on ? i2s_channel_enable(sTx) : i2s_channel_disable(sTx);
}

}  // namespace board
