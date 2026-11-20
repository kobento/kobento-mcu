// Elecrow CrowPanel 1.28" rotary: display (GC9A01 on SPI), touch (CST816D on I2C), backlight
// (LEDC PWM), the knob (PCNT quadrature + a push switch), the LED ring (WS2812 over RMT),
// console (USB Serial/JTAG).
//
// Pins are from Elecrow's wiki page "CrowPanel 1.28inch-HMI ESP32 Rotary Display", the
// Makerguides getting-started article for it, and RIOT-OS's board definition
// (elecrow-crowpanel-esp32s3-rotary-128), read 2026-11-05; see ../../README.md. The panel init
// is the Waveshare round board's (the common GC9A01 sequence).
#include "board/board.h"
#include "board/knob.h"

#include <algorithm>
#include <vector>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "driver/rmt_tx.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"

static const char* TAG = "crowpanel";

namespace board {

static constexpr gpio_num_t kSclk = GPIO_NUM_10, kMosi = GPIO_NUM_11, kLcdCs = GPIO_NUM_9,
                            kLcdDc = GPIO_NUM_3, kLcdRst = GPIO_NUM_14, kBacklight = GPIO_NUM_46,
                            kLcdPower = GPIO_NUM_1,    // LCD 3V3 rail, active high
                            kLedPower = GPIO_NUM_2,    // the LED/backlight-anode rail, active high
                            kSda = GPIO_NUM_6, kScl = GPIO_NUM_7, kTouchRst = GPIO_NUM_13,
                            kTouchInt = GPIO_NUM_5,
                            kEncA = GPIO_NUM_45, kEncB = GPIO_NUM_42, kEncSw = GPIO_NUM_41,
                            kLedData = GPIO_NUM_48, kPowerLed = GPIO_NUM_40;

// The panel runs at 80 MHz (the firmware it shipped with did); 240x240 RGB565 is then ~11.5 ms
// a frame on the wire.
static constexpr int kLcdHz = 80 * 1000 * 1000;
static constexpr int kStripLines = 24;

// How the picture is turned on the panel, clockwise in degrees: 0, 90, 180 or 270. The panel
// turns it (MADCTL row/column swap and mirrors), so it costs nothing a frame; touch is turned
// to match in readTouch. Screenshots (dumpFrame, rcstage capture) are taken before the panel,
// so they stay upright.
static constexpr int kRotation = 270;
static_assert(kRotation == 0 || kRotation == 90 || kRotation == 180 || kRotation == 270,
              "kRotation: 0, 90, 180 or 270");
// MADCTL: MY 0x80, MX 0x40, MV (swap rows and columns) 0x20, BGR 0x08.
static constexpr uint8_t kMadctl = 0x08 | (kRotation == 90  ? 0x20 | 0x40
                                         : kRotation == 180 ? 0x40 | 0x80
                                         : kRotation == 270 ? 0x20 | 0x80 : 0);
static constexpr size_t kStripBytes = (size_t) kWidth * kStripLines * 2;

static esp_lcd_panel_io_handle_t sIo;
static uint16_t* sStrip[2];
static SemaphoreHandle_t sFree;   // counts strip buffers not on the wire
static const uint32_t* sLast;     // last presented frame, for dumpFrame
static int sLastW, sLastH;
static i2c_master_bus_handle_t sBus;
static i2c_master_dev_handle_t sTouch;

static void output(gpio_num_t pin, int level) {
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, level);
}

// ── I2C (touch 0x15) ──

static i2c_master_dev_handle_t i2cDevice(uint8_t addr) {
    if (!sBus) {
        i2c_master_bus_config_t cfg = {};
        cfg.i2c_port = I2C_NUM_0;
        cfg.sda_io_num = kSda;
        cfg.scl_io_num = kScl;
        cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        cfg.glitch_ignore_cnt = 7;
        cfg.flags.enable_internal_pullup = true;
        if (i2c_new_master_bus(&cfg, &sBus) != ESP_OK) return nullptr;
    }
    if (i2c_master_probe(sBus, addr, 50) != ESP_OK) return nullptr;
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = addr;
    dev.scl_speed_hz = 400000;
    i2c_master_dev_handle_t h = nullptr;
    i2c_master_bus_add_device(sBus, &dev, &h);
    return h;
}

static bool i2cRead(i2c_master_dev_handle_t d, uint8_t reg, uint8_t* out, size_t n) {
    return d && i2c_master_transmit_receive(d, &reg, 1, out, n, 50) == ESP_OK;
}

static bool i2cWrite(i2c_master_dev_handle_t d, uint8_t reg, uint8_t value) {
    const uint8_t b[2] = {reg, value};
    return d && i2c_master_transmit(d, b, 2, 50) == ESP_OK;
}

// The CST816D at 0x15 answers only after a reset (it sleeps otherwise).
static void touchInit() {
    output(kTouchRst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(kTouchRst, 1);
    vTaskDelay(pdMS_TO_TICKS(60));
    sTouch = i2cDevice(0x15);
    if (sTouch) {
        i2cWrite(sTouch, 0xFE, 0x01);   // DisAutoSleep: stay awake, so polling works
        gpio_config_t in = {};
        in.pin_bit_mask = 1ULL << kTouchInt;
        in.mode = GPIO_MODE_INPUT;
        in.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&in);
    }
    ESP_LOGI(TAG, "touch %s", sTouch ? "CST816D" : "not found");
}

// ── Knob: the encoder on PCNT (x4 decoding: 4 counts a detent), the switch sampled every
// 5 ms and taken once stable for 4 samples ──

static constexpr int kCountsPerDetent = 4;
static pcnt_unit_handle_t sPcnt;
static std::atomic<bool> sSwDown{false};
// The console's stand-ins for a hand ("turn N", "press", "hold"): detents added to the
// position, and a time the switch reads as held until.
static std::atomic<int> sFakeDetents{0};
static std::atomic<int64_t> sFakeDownUntil{0};
static esp_timer_handle_t sSwTimer;

// Every 5 ms, whatever the player is doing (a 3D document can take most of a second a frame):
// debounce the switch and count its presses, short and long, here.
static constexpr int64_t kLongPressUs = 800000;
static std::atomic<int> sShortPresses{0}, sLongPresses{0};

static void swSample(void*) {
    static int stable = 0;
    static bool last = false, down = false, longDone = false;
    static int64_t downAt = 0;
    const int64_t now = esp_timer_get_time();
    const bool raw = gpio_get_level(kEncSw) == 0 || now < sFakeDownUntil;
    if (raw == last) {
        if (++stable >= 4 && raw != down) {
            down = raw;
            if (down) {
                downAt = now;
                longDone = false;
            } else if (!longDone) {
                sShortPresses++;
            }
        }
    } else {
        stable = 0;
        last = raw;
    }
    if (down && !longDone && now - downAt >= kLongPressUs) {
        longDone = true;
        sLongPresses++;
    }
    sSwDown = down;
}

static void knobInit() {
    pcnt_unit_config_t u = {};
    u.low_limit = -1000;
    u.high_limit = 1000;
    u.flags.accum_count = 1;
    if (pcnt_new_unit(&u, &sPcnt) != ESP_OK) { ESP_LOGE(TAG, "no PCNT unit"); return; }
    pcnt_glitch_filter_config_t f = {};
    f.max_glitch_ns = 1000;
    pcnt_unit_set_glitch_filter(sPcnt, &f);
    pcnt_chan_config_t ca = {}, cb = {};
    ca.edge_gpio_num = kEncA;
    ca.level_gpio_num = kEncB;
    cb.edge_gpio_num = kEncB;
    cb.level_gpio_num = kEncA;
    pcnt_channel_handle_t a = nullptr, b = nullptr;
    pcnt_new_channel(sPcnt, &ca, &a);
    pcnt_new_channel(sPcnt, &cb, &b);
    pcnt_channel_set_edge_action(a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_channel_set_edge_action(b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    // The pins float without the pull-ups (the encoder only pulls to ground).
    gpio_pullup_en(kEncA);
    gpio_pullup_en(kEncB);
    pcnt_unit_add_watch_point(sPcnt, u.low_limit);
    pcnt_unit_add_watch_point(sPcnt, u.high_limit);
    // With accum_count, the unit carries the count past these limits itself.
    pcnt_unit_enable(sPcnt);
    pcnt_unit_clear_count(sPcnt);
    pcnt_unit_start(sPcnt);

    gpio_config_t in = {};
    in.pin_bit_mask = 1ULL << kEncSw;
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&in);
    esp_timer_create_args_t t = {};
    t.callback = swSample;
    t.name = "knob_sw";
    esp_timer_create(&t, &sSwTimer);
    esp_timer_start_periodic(sSwTimer, 5000);
}

static int knobCounts() {
    int c = 0;
    if (sPcnt) pcnt_unit_get_count(sPcnt, &c);   // with accum_count, includes the overflow
    return c;
}

// Rounded to the nearest detent, so a knob resting on a detent never flickers between two.
// Clockwise (seen from the front) counts up as wired.
int knobPosition() {
    const int c = knobCounts();
    return (int) std::lround((double) c / kCountsPerDetent) + sFakeDetents;
}

bool knobDown() { return sSwDown; }
int knobShortPresses() { return sShortPresses; }
int knobLongPresses() { return sLongPresses; }

// ── LED ring: 5 WS2812 (GRB, 800 kHz) on RMT at 10 MHz, through the bytes encoder ──

static rmt_channel_handle_t sRmt;
static rmt_encoder_handle_t sRmtEnc;
static uint32_t sLeds[kLedCount];
static bool sLedsSent = false;
static SemaphoreHandle_t sLedLock;

static void ledsInit() {
    sLedLock = xSemaphoreCreateMutex();
    rmt_tx_channel_config_t c = {};
    c.gpio_num = kLedData;
    c.clk_src = RMT_CLK_SRC_DEFAULT;
    c.resolution_hz = 10 * 1000 * 1000;
    c.mem_block_symbols = 64;
    c.trans_queue_depth = 2;
    if (rmt_new_tx_channel(&c, &sRmt) != ESP_OK) { ESP_LOGE(TAG, "no RMT channel"); return; }
    rmt_bytes_encoder_config_t e = {};
    e.bit0 = {.duration0 = 3, .level0 = 1, .duration1 = 9, .level1 = 0};   // 0.3 us high, 0.9 low
    e.bit1 = {.duration0 = 9, .level0 = 1, .duration1 = 3, .level1 = 0};   // 0.9 us high, 0.3 low
    e.flags.msb_first = 1;
    rmt_new_bytes_encoder(&e, &sRmtEnc);
    rmt_enable(sRmt);
    const uint32_t off[kLedCount] = {};
    setLeds(off, kLedCount);
}

void setLeds(const uint32_t* rgb, int n) {
    if (!sRmt) return;
    n = std::min(n, kLedCount);
    xSemaphoreTake(sLedLock, portMAX_DELAY);
    bool changed = !sLedsSent;
    for (int i = 0; i < n; i++) {
        changed |= sLeds[i] != (rgb[i] & 0xFFFFFF);
        sLeds[i] = rgb[i] & 0xFFFFFF;
    }
    if (changed) {
        static uint8_t grb[kLedCount * 3];
        for (int i = 0; i < kLedCount; i++) {
            const uint32_t c = sLeds[i];
            grb[i * 3 + 0] = (uint8_t) (c >> 8);
            grb[i * 3 + 1] = (uint8_t) (c >> 16);
            grb[i * 3 + 2] = (uint8_t) c;
        }
        rmt_transmit_config_t t = {};
        rmt_transmit(sRmt, sRmtEnc, grb, sizeof grb, &t);
        rmt_tx_wait_all_done(sRmt, 50);
        esp_rom_delay_us(80);   // the latch: >50 us low
        sLedsSent = true;
    }
    xSemaphoreGive(sLedLock);
}

void setPowerLed(bool on) { gpio_set_level(kPowerLed, on ? 0 : 1); }

// ── Panel ──

static bool IRAM_ATTR onColorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(sFree, &woken);
    return woken == pdTRUE;
}

static void cmd(uint8_t c, std::initializer_list<uint8_t> data = {}) {
    esp_lcd_panel_io_tx_param(sIo, c, data.size() ? data.begin() : nullptr, data.size());
}

static void backlightInit() {
    ledc_timer_config_t t = {};
    t.speed_mode = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_10_BIT;
    t.timer_num = LEDC_TIMER_0;
    t.freq_hz = 20000;
    t.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&t);
    ledc_channel_config_t c = {};
    c.gpio_num = kBacklight;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel = LEDC_CHANNEL_0;
    c.timer_sel = LEDC_TIMER_0;
    c.duty = 0;
    ledc_channel_config(&c);
}

void setBrightness(int percent) {
    percent = std::max(0, std::min(100, percent));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, percent * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void fillBlack() {
    memset(sStrip[0], 0, kStripBytes);
    cmd(0x2A, {0, 0, 0, kWidth - 1});
    for (int y = 0; y < kHeight; y += kStripLines) {
        xSemaphoreTake(sFree, portMAX_DELAY);
        const int y1 = std::min(kHeight, y + kStripLines) - 1;
        cmd(0x2B, {0, (uint8_t) y, 0, (uint8_t) y1});
        esp_lcd_panel_io_tx_color(sIo, 0x2C, sStrip[0], (size_t) (y1 - y + 1) * kWidth * 2);
    }
    xSemaphoreTake(sFree, portMAX_DELAY);
    xSemaphoreTake(sFree, portMAX_DELAY);
    xSemaphoreGive(sFree);
    xSemaphoreGive(sFree);
}

bool displayInit() {
    // The LCD's 3V3 rail and the LED ring's supply are switched: on before anything else.
    output(kLcdPower, 1);
    output(kLedPower, 1);
    output(kPowerLed, 1);   // the red LED off (active low)
    vTaskDelay(pdMS_TO_TICKS(20));
    touchInit();
    knobInit();
    ledsInit();
    backlightInit();

    output(kLcdCs, 1);
    output(kLcdRst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(kLcdRst, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    spi_bus_config_t bus = {};
    bus.mosi_io_num = kMosi;
    bus.miso_io_num = -1;
    bus.sclk_io_num = kSclk;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = kStripBytes;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

    sFree = xSemaphoreCreateCounting(2, 2);
    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num = kLcdCs;
    io.dc_gpio_num = kLcdDc;
    io.spi_mode = 0;
    io.pclk_hz = kLcdHz;
    io.trans_queue_depth = 4;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    io.on_color_trans_done = onColorDone;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t) SPI2_HOST, &io, &sIo) != ESP_OK) return false;

    for (auto& s : sStrip) {
        s = (uint16_t*) heap_caps_malloc(kStripBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s) return false;
    }

    // GC9A01, the common init (Waveshare's LCD_1in28, TFT_eSPI, esp_lcd_gc9a01): unlock the
    // inter registers, then power, gamma and timing. MADCTL from kRotation.
    cmd(0xEF);
    cmd(0xEB, {0x14});
    cmd(0xFE);
    cmd(0xEF);
    cmd(0xEB, {0x14});
    cmd(0x84, {0x40});
    cmd(0x85, {0xFF});
    cmd(0x86, {0xFF});
    cmd(0x87, {0xFF});
    cmd(0x88, {0x0A});
    cmd(0x89, {0x21});
    cmd(0x8A, {0x00});
    cmd(0x8B, {0x80});
    cmd(0x8C, {0x01});
    cmd(0x8D, {0x01});
    cmd(0x8E, {0xFF});
    cmd(0x8F, {0xFF});
    cmd(0xB6, {0x00, 0x20});
    cmd(0x36, {kMadctl});   // BGR, turned by kRotation
    cmd(0x3A, {0x05});   // RGB565
    cmd(0x90, {0x08, 0x08, 0x08, 0x08});
    cmd(0xBD, {0x06});
    cmd(0xBC, {0x00});
    cmd(0xFF, {0x60, 0x01, 0x04});
    cmd(0xC3, {0x13});
    cmd(0xC4, {0x13});
    cmd(0xC9, {0x22});
    cmd(0xBE, {0x11});
    cmd(0xE1, {0x10, 0x0E});
    cmd(0xDF, {0x21, 0x0C, 0x02});
    cmd(0xF0, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A});
    cmd(0xF1, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F});
    cmd(0xF2, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A});
    cmd(0xF3, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F});
    cmd(0xED, {0x1B, 0x0B});
    cmd(0xAE, {0x77});
    cmd(0xCD, {0x63});
    cmd(0x70, {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03});
    cmd(0xE8, {0x34});
    cmd(0x62, {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70});
    cmd(0x63, {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70});
    cmd(0x64, {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07});
    cmd(0x66, {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00});
    cmd(0x67, {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98});
    cmd(0x74, {0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00});
    cmd(0x98, {0x3E, 0x07});
    cmd(0x35);   // TEON
    cmd(0x21);   // INVON
    cmd(0x11);   // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd(0x29);   // DISPON
    vTaskDelay(pdMS_TO_TICKS(20));

    fillBlack();
    setBrightness(100);
    ESP_LOGI(TAG, "panel up: %dx%d GC9A01 at %d MHz", kWidth, kHeight, kLcdHz / 1000000);
    return true;
}

// Premultiplied, opaque ARGB8888 -> RGB565, byte-swapped for the panel.
static inline uint16_t toPanel565(uint32_t p) {
    const uint16_t v = (uint16_t) (((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
    return (uint16_t) ((v << 8) | (v >> 8));
}

static void sendFrame(const uint32_t* argb, int srcW, int srcH) {
    const int k = std::max(1, kWidth / std::max(1, srcW));   // integer upscale factor
    cmd(0x2A, {0, 0, 0, kWidth - 1});
    int which = 0;
    for (int y0 = 0; y0 < kHeight; y0 += kStripLines) {
        xSemaphoreTake(sFree, portMAX_DELAY);
        uint16_t* dst = sStrip[which];
        const int lines = std::min(kStripLines, kHeight - y0);
        for (int y = 0; y < lines; y++) {
            const uint32_t* src = argb + (size_t) std::min(srcH - 1, (y0 + y) / k) * srcW;
            uint16_t* d = dst + (size_t) y * kWidth;
            if (k == 1) {
                for (int x = 0; x < kWidth; x++) d[x] = toPanel565(src[x]);
            } else {
                for (int x = 0; x < kWidth; x++) d[x] = toPanel565(src[x / k]);
            }
        }
        const int y1 = y0 + lines - 1;
        cmd(0x2B, {0, (uint8_t) y0, 0, (uint8_t) y1});
        esp_lcd_panel_io_tx_color(sIo, 0x2C, dst, (size_t) lines * kWidth * 2);
        which ^= 1;
    }
    xSemaphoreTake(sFree, portMAX_DELAY);
    xSemaphoreTake(sFree, portMAX_DELAY);
    xSemaphoreGive(sFree);
    xSemaphoreGive(sFree);
}

// Only the regions given (a full-size frame): each converted and sent in strips of the strip
// buffers' size, behind a window of its own. The panel keeps the rest.
static void sendRegions(const uint32_t* argb, const std::vector<int32_t>& r) {
    int which = 0;
    for (size_t i = 0; i + 3 < r.size(); i += 4) {
        // Columns in pairs (an even start and width): every transfer a whole number of 32-bit
        // words, so SPI DMA sends from the strip itself rather than through a bounce buffer.
        const int x = std::max(0, (int) r[i]) & ~1, y = std::max(0, (int) r[i + 1]);
        const int w = std::min(kWidth - x, ((int) r[i] + (int) r[i + 2] - x + 1) & ~1);
        const int h = std::min(kHeight - y, (int) r[i + 3]);
        if (w <= 0 || h <= 0) continue;
        const int per = std::max(1, std::min(h, kWidth * kStripLines / w));   // rows a strip holds
        const int x1 = x + w - 1;
        for (int y0 = y; y0 < y + h; y0 += per) {
            xSemaphoreTake(sFree, portMAX_DELAY);
            uint16_t* dst = sStrip[which];
            const int lines = std::min(per, y + h - y0);
            for (int row = 0; row < lines; row++) {
                const uint32_t* src = argb + (size_t) (y0 + row) * kWidth + x;
                uint16_t* d = dst + (size_t) row * w;
                for (int k = 0; k < w; k++) d[k] = toPanel565(src[k]);
            }
            const int y1 = y0 + lines - 1;
            // the whole window before each write, columns and rows, as esp_lcd's GC9A01 driver does
            cmd(0x2A, {(uint8_t) (x >> 8), (uint8_t) x, (uint8_t) (x1 >> 8), (uint8_t) x1});
            cmd(0x2B, {(uint8_t) (y0 >> 8), (uint8_t) y0, (uint8_t) (y1 >> 8), (uint8_t) y1});
            esp_lcd_panel_io_tx_color(sIo, 0x2C, dst, (size_t) lines * w * 2);
            which ^= 1;
        }
    }
    xSemaphoreTake(sFree, portMAX_DELAY);
    xSemaphoreTake(sFree, portMAX_DELAY);
    xSemaphoreGive(sFree);
    xSemaphoreGive(sFree);
}

// A sender task on core 0 converts and sends frame N while the player (core 1) renders N+1.
static TaskHandle_t sSender;
static SemaphoreHandle_t sJob, sIdle;
static const uint32_t* sJobArgb;
static int sJobW, sJobH;
static std::vector<int32_t> sJobRegions;   // empty: the whole frame

static void senderTask(void*) {
    while (true) {
        xSemaphoreTake(sJob, portMAX_DELAY);
        if (sJobRegions.empty()) sendFrame(sJobArgb, sJobW, sJobH);
        else sendRegions(sJobArgb, sJobRegions);
        xSemaphoreGive(sIdle);
    }
}

void presentWait() {
    if (!sIdle) return;
    xSemaphoreTake(sIdle, portMAX_DELAY);
    xSemaphoreGive(sIdle);
}

void presentAsync(const uint32_t* argb, int srcW, int srcH) { presentAsyncRegions(argb, srcW, srcH, nullptr, 0); }

void presentAsyncRegions(const uint32_t* argb, int srcW, int srcH, const int32_t* xywh, int n) {
    if (!sSender) {
        sJob = xSemaphoreCreateBinary();
        sIdle = xSemaphoreCreateBinary();
        xSemaphoreGive(sIdle);
        xTaskCreatePinnedToCore(senderTask, "lcd_send", 3072, nullptr, 6, &sSender, 0);
    }
    xSemaphoreTake(sIdle, portMAX_DELAY);
    if (n > 0 && srcW == kWidth && srcH == kHeight) sJobRegions.assign(xywh, xywh + 4 * n);
    else sJobRegions.clear();
    sJobArgb = argb;
    sJobW = srcW;
    sJobH = srcH;
    sLast = argb;
    sLastW = srcW;
    sLastH = srcH;
    xSemaphoreGive(sJob);
}

int64_t present(const uint32_t* argb, int srcW, int srcH) {
    int64_t t0 = esp_timer_get_time();
    presentAsync(argb, srcW, srcH);
    presentWait();
    return esp_timer_get_time() - t0;
}

const uint16_t* frameBuffer() { return nullptr; }

void dumpFrame(int step) {
    presentWait();
    if (!sLast) { printf("SNAP BEGIN 0 0\nSNAP END\n"); return; }
    const int k = std::max(1, kWidth / std::max(1, sLastW));
    const int ow = kWidth / step, oh = kHeight / step;
    static uint16_t row[kWidth];
    static unsigned char b64[kWidth * 3];
    printf("\nSNAP BEGIN %d %d\n", ow, oh);
    for (int y = 0; y < oh; y++) {
        const uint32_t* src = sLast + (size_t) std::min(sLastH - 1, y * step / k) * sLastW;
        for (int x = 0; x < ow; x++) {
            const uint32_t p = src[std::min(sLastW - 1, x * step / k)];
            row[x] = (uint16_t) (((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
        }
        size_t olen = 0;
        mbedtls_base64_encode(b64, sizeof(b64), &olen, (const unsigned char*) row, ow * 2);
        b64[olen] = 0;
        printf("%s\n", b64);
        if ((y & 15) == 15) fflush(stdout);
    }
    printf("SNAP END\n");
    fflush(stdout);
}

// ── Touch: CST816D, reg 0x02 = finger count, 0x03..0x06 = X and Y (12 bits each) ──

Touch readTouch() {
    Touch t;
    if (!sTouch) return t;
    uint8_t d[5] = {};
    if (!i2cRead(sTouch, 0x02, d, sizeof d) || (d[0] & 0x0F) == 0) return t;
    const int x = ((d[1] & 0x0F) << 8) | d[2];
    const int y = ((d[3] & 0x0F) << 8) | d[4];
    if (x >= kWidth || y >= kHeight) return t;
    // The touch panel reports in the panel's own frame; turn it into the picture's.
    t.down = true;
    switch (kRotation) {
        case 90:  t.x = y;               t.y = kWidth - 1 - x;  break;
        case 180: t.x = kWidth - 1 - x;  t.y = kHeight - 1 - y; break;
        case 270: t.x = kHeight - 1 - y; t.y = x;               break;
        default:  t.x = x;               t.y = y;               break;
    }
    return t;
}

static void touchProbe() {
    if (!sTouch) { printf("touch: none\n"); return; }
    printf("touch: raw coordinates for 15 s\n");
    int lastX = -1, lastY = -1;
    for (int64_t end = esp_timer_get_time() + 15000000; esp_timer_get_time() < end;) {
        uint8_t d[5] = {};
        if (i2cRead(sTouch, 0x02, d, sizeof d) && (d[0] & 0x0F)) {
            const int x = ((d[1] & 0x0F) << 8) | d[2], y = ((d[3] & 0x0F) << 8) | d[4];
            if (std::abs(x - lastX) > 3 || std::abs(y - lastY) > 3) {
                printf("touch raw x=%d y=%d\n", x, y);
                lastX = x;
                lastY = y;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    printf("touch: done\n");
}

// Prints the knob for 15 s: raw counts, detents and the switch, to check kCountsPerDetent and
// the direction.
static void knobProbe() {
    printf("knob: turn and press for 15 s\n");
    int lastC = INT32_MIN;
    bool lastSw = false;
    for (int64_t end = esp_timer_get_time() + 15000000; esp_timer_get_time() < end;) {
        const int c = knobCounts();
        const bool sw = knobDown();
        if (c != lastC || sw != lastSw) {
            printf("knob counts=%d position=%d switch=%s\n", c, knobPosition(), sw ? "down" : "up");
            lastC = c;
            lastSw = sw;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("knob: done\n");
}

void consoleInit() {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 1024;
    cfg.tx_buffer_size = 4096;
    usb_serial_jtag_driver_install(&cfg);
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdin, nullptr, _IONBF, 0);
}

void radioPower() {}

// The knob's switch is read through board/knob.h, by the app.
int buttonPressed() { return 0; }

// No motion sensor.
Motion readMotion() { return {}; }

// Powered over USB only (no battery connector the ESP32 can measure).
void powerInit() {}
Power readPower() {
    Power p;
    p.powerState = 2;
    return p;
}

bool console(const std::string& line) {
    if (line == "touch") { touchProbe(); return true; }
    if (line == "knob") { knobProbe(); return true; }
    if (line.rfind("turn ", 0) == 0) {
        sFakeDetents += (int) std::strtol(line.c_str() + 5, nullptr, 10);
        printf("knob position %d\n", knobPosition());
        return true;
    }
    if (line == "press" || line == "hold") {
        sFakeDownUntil = esp_timer_get_time() + (line == "press" ? 120000 : 1200000);
        printf("knob %s\n", line.c_str());
        return true;
    }
    if (line.rfind("led", 0) == 0) {
        // "led RRGGBB" all, "led off"
        uint32_t c = line.size() > 4 ? (uint32_t) std::strtoul(line.c_str() + 4, nullptr, 16) : 0;
        uint32_t all[kLedCount];
        for (auto& v : all) v = c;
        setLeds(all, kLedCount);
        printf("leds #%06X\n", (unsigned) c);
        return true;
    }
    if (line == "i2c") {
        printf("i2c (SDA 6, SCL 7):");
        i2cDevice(0x15);   // brings the bus up
        for (int a = 0x08; a < 0x78; a++)
            if (sBus && i2c_master_probe(sBus, a, 20) == ESP_OK) printf(" 0x%02X", a);
        printf("\n");
        return true;
    }
    return false;
}

// Not on this board: no RTC, nothing to beep with, a panel that needs no refresh.
bool rtcRead(int64_t*) { return false; }
bool rtcWrite(int64_t) { return false; }
bool beep(int, int) { return false; }
bool displayRefresh() { return false; }

const char* wakeReason() { return "power_on"; }
bool sleepFor(int, Sleep) { return false; }

}  // namespace board
