// LCDWIKI ES3C28P, "2.8inch ESP32-S3 Display": display (ILI9341V on SPI, turned to
// landscape), touch (FT6336G on I2C), backlight (LEDC PWM), the BOOT key, console (USB
// Serial/JTAG).
//
// Pins are from the LCDWIKI page "2.8inch ESP32-S3 Display" (ESP32 Pin Parameters); the
// panel init follows LCDWIKI's ILI9341V_Init.txt (INVON, BGR, RGB565). The LCD reset is wired
// to the chip's EN, so the panel is reset in software (0x01).
#include "board/board.h"
#include "audio.h"

#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"

static const char* TAG = "es3c28p";

namespace board {

static constexpr gpio_num_t kSclk = GPIO_NUM_12, kMosi = GPIO_NUM_11, kLcdCs = GPIO_NUM_10,
                            kLcdDc = GPIO_NUM_46, kBacklight = GPIO_NUM_45,
                            kSda = GPIO_NUM_16, kScl = GPIO_NUM_15, kTouchRst = GPIO_NUM_18,
                            kTouchInt = GPIO_NUM_17, kBootKey = GPIO_NUM_0;
static constexpr uint8_t kTouchAddr = 0x38;

// MADCTL: MV (rows and columns swapped: landscape) + MY + BGR, LCDWIKI's landscape; turned 180°
// (MV + MX + BGR, 0x68) for the way the board stands here (2026-11-14). Flip
// kRotate180 if the picture is upside down for the way the board is held.
static constexpr bool kRotate180 = true;
static constexpr uint8_t kMadctl = kRotate180 ? 0x68 : 0xA8;

static constexpr int kLcdHz = 40 * 1000 * 1000;
static constexpr int kStripLines = 16;
static constexpr size_t kStripBytes = (size_t) kWidth * kStripLines * 2;

static esp_lcd_panel_io_handle_t sIo;
static uint16_t* sStrip[2];
static SemaphoreHandle_t sFree;   // counts strip buffers not on the wire
static const uint32_t* sLast;     // last presented frame, for dumpFrame
static int sLastW, sLastH;
static i2c_master_bus_handle_t sBus;
static i2c_master_dev_handle_t sTouch;

// ── I2C ──

i2c_master_dev_handle_t i2cDevice(uint8_t addr) {
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

static void output(gpio_num_t pin, int level) {
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, level);
}

static void touchInit() {
    output(kTouchRst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(kTouchRst, 1);
    vTaskDelay(pdMS_TO_TICKS(300));   // the FT6336 needs ~300 ms after reset
    sTouch = i2cDevice(kTouchAddr);
    gpio_config_t in = {};
    in.pin_bit_mask = 1ULL << kTouchInt;
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&in);
    ESP_LOGI(TAG, "touch %s", sTouch ? "FT6336G at 0x38" : "not found");
}

// ── Panel ──

static bool IRAM_ATTR onColorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(sFree, &woken);
    return woken == pdTRUE;
}

static void cmd(uint8_t c, std::initializer_list<uint8_t> data = {}) {
    esp_lcd_panel_io_tx_param(sIo, c, data.size() ? data.begin() : nullptr, data.size());
}

static void window(int x0, int y0, int x1, int y1) {
    cmd(0x2A, {(uint8_t) (x0 >> 8), (uint8_t) x0, (uint8_t) (x1 >> 8), (uint8_t) x1});
    cmd(0x2B, {(uint8_t) (y0 >> 8), (uint8_t) y0, (uint8_t) (y1 >> 8), (uint8_t) y1});
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

static void drain() {
    xSemaphoreTake(sFree, portMAX_DELAY);
    xSemaphoreTake(sFree, portMAX_DELAY);
    xSemaphoreGive(sFree);
    xSemaphoreGive(sFree);
}

static void fillBlack() {
    memset(sStrip[0], 0, kStripBytes);
    for (int y = 0; y < kHeight; y += kStripLines) {
        xSemaphoreTake(sFree, portMAX_DELAY);
        const int y1 = std::min(kHeight, y + kStripLines) - 1;
        window(0, y, kWidth - 1, y1);
        esp_lcd_panel_io_tx_color(sIo, 0x2C, sStrip[0], (size_t) (y1 - y + 1) * kWidth * 2);
    }
    drain();
}

bool displayInit() {
    backlightInit();
    touchInit();

    gpio_config_t key = {};
    key.pin_bit_mask = 1ULL << kBootKey;
    key.mode = GPIO_MODE_INPUT;
    key.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&key);

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

    // ILI9341V, LCDWIKI's init.
    cmd(0x01);   // SWRESET: the RST line is the chip's EN
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd(0xCF, {0x00, 0xC1, 0x30});
    cmd(0xED, {0x64, 0x03, 0x12, 0x81});
    cmd(0xE8, {0x85, 0x00, 0x78});
    cmd(0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02});
    cmd(0xF7, {0x20});
    cmd(0xEA, {0x00, 0x00});
    cmd(0xC0, {0x13});         // power control 1
    cmd(0xC1, {0x13});         // power control 2
    cmd(0xC5, {0x22, 0x35});   // VCOM
    cmd(0xC7, {0xBD});
    cmd(0x21);                 // INVON
    cmd(0x36, {kMadctl});
    cmd(0xB6, {0x0A, 0xA2});
    cmd(0x3A, {0x55});         // RGB565
    cmd(0xF6, {0x01, 0x30});
    cmd(0xB1, {0x00, 0x1B});   // frame rate 70 Hz
    cmd(0xF2, {0x00});
    cmd(0x26, {0x01});
    cmd(0xE0, {0x0F, 0x35, 0x31, 0x0B, 0x0E, 0x06, 0x49, 0xA7, 0x33, 0x07, 0x0F, 0x03, 0x0C, 0x0A, 0x00});
    cmd(0xE1, {0x00, 0x0A, 0x0F, 0x04, 0x11, 0x08, 0x36, 0x58, 0x4D, 0x07, 0x10, 0x0C, 0x32, 0x34, 0x0F});
    cmd(0x11);   // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(120));
    cmd(0x29);   // DISPON
    vTaskDelay(pdMS_TO_TICKS(20));

    fillBlack();
    setBrightness(100);
    audioInit();   // the Teensy Audio Shield on the expansion headers, when wired
    ESP_LOGI(TAG, "panel up: %dx%d ILI9341V at %d MHz", kWidth, kHeight, kLcdHz / 1000000);
    return true;
}

// Premultiplied, opaque ARGB8888 -> RGB565, byte-swapped for the panel.
static inline uint16_t toPanel565(uint32_t p) {
    const uint16_t v = (uint16_t) (((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
    return (uint16_t) ((v << 8) | (v >> 8));
}

static void sendFrame(const uint32_t* argb, int srcW, int srcH) {
    const int k = std::max(1, kWidth / std::max(1, srcW));   // integer upscale factor
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
        window(0, y0, kWidth - 1, y0 + lines - 1);
        esp_lcd_panel_io_tx_color(sIo, 0x2C, dst, (size_t) lines * kWidth * 2);
        which ^= 1;
    }
    drain();
}

// Only the regions given (a full-size frame): each converted and sent in strips of the strip
// buffers' size, behind a window of its own. The panel keeps the rest.
static void sendRegions(const uint32_t* argb, const std::vector<int32_t>& r) {
    int which = 0;
    for (size_t i = 0; i + 3 < r.size(); i += 4) {
        const int x = std::max(0, (int) r[i]), y = std::max(0, (int) r[i + 1]);
        const int w = std::min(kWidth - x, (int) r[i + 2]), h = std::min(kHeight - y, (int) r[i + 3]);
        if (w <= 0 || h <= 0) continue;
        const int per = std::max(1, std::min(h, kWidth * kStripLines / w));   // rows a strip holds
        for (int y0 = y; y0 < y + h; y0 += per) {
            xSemaphoreTake(sFree, portMAX_DELAY);
            uint16_t* dst = sStrip[which];
            const int lines = std::min(per, y + h - y0);
            for (int row = 0; row < lines; row++) {
                const uint32_t* src = argb + (size_t) (y0 + row) * kWidth + x;
                uint16_t* d = dst + (size_t) row * w;
                for (int k = 0; k < w; k++) d[k] = toPanel565(src[k]);
            }
            window(x, y0, x + w - 1, y0 + lines - 1);
            esp_lcd_panel_io_tx_color(sIo, 0x2C, dst, (size_t) lines * w * 2);
            which ^= 1;
        }
    }
    drain();
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

// ── Touch: FT6336G, reg 0x02 = finger count, 0x03..0x06 = X and Y in the portrait panel's
// coordinates (240 x 320), turned here like the picture. ──

static bool touchRaw(int* x, int* y) {
    uint8_t d[5] = {};
    if (!sTouch || !i2cRead(sTouch, 0x02, d, sizeof d)) return false;
    const int n = d[0] & 0x0F;
    if (n == 0 || n > 2) return false;
    *x = ((d[1] & 0x0F) << 8) | d[2];
    *y = ((d[3] & 0x0F) << 8) | d[4];
    return true;
}

Touch readTouch() {
    Touch t;
    int rx, ry;
    if (!touchRaw(&rx, &ry)) return t;
    // MV + MY (0xA8): screen x runs down the portrait panel's y, reversed; screen y along x.
    int x = kRotate180 ? ry : kWidth - 1 - ry;
    int y = kRotate180 ? kHeight - 1 - rx : rx;
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return t;
    t.down = true;
    t.x = x;
    t.y = y;
    return t;
}

static void touchProbe() {
    if (!sTouch) { printf("touch: none\n"); return; }
    printf("touch: raw and screen coordinates for 15 s\n");
    int lastX = -1, lastY = -1;
    for (int64_t end = esp_timer_get_time() + 15000000; esp_timer_get_time() < end;) {
        int x, y;
        if (touchRaw(&x, &y) && (std::abs(x - lastX) > 3 || std::abs(y - lastY) > 3)) {
            const Touch t = readTouch();
            printf("touch raw x=%d y=%d -> screen x=%d y=%d\n", x, y, t.x, t.y);
            lastX = x;
            lastY = y;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    printf("touch: done\n");
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

// The BOOT key (GPIO0, low when pressed): +1 per press, debounced by the 30 ms poll gap.
int buttonPressed() {
    static int64_t lastPoll;
    static bool wasDown;
    const int64_t now = esp_timer_get_time();
    if (now - lastPoll < 30000) return 0;
    lastPoll = now;
    const bool down = gpio_get_level(kBootKey) == 0;
    const bool pressed = down && !wasDown;
    wasDown = down;
    return pressed ? 1 : 0;
}

Motion readMotion() { return {}; }

// The battery divider on GPIO9 isn't documented: not read.
void powerInit() {}
Power readPower() { return {}; }

bool console(const std::string& line) {
    if (line == "touch") { touchProbe(); return true; }
    if (audioConsole(line)) return true;
    if (line == "i2c") {
        printf("i2c (SDA 16, SCL 15):");
        i2cDevice(kTouchAddr);   // brings the bus up
        for (int a = 0x08; a < 0x78; a++)
            if (sBus && i2c_master_probe(sBus, a, 20) == ESP_OK) printf(" 0x%02X", a);
        printf("\n");
        return true;
    }
    return false;
}

// Not on this board (yet): no RTC, a panel that needs no refresh. beep() is in audio.cpp.
bool rtcRead(int64_t*) { return false; }
bool rtcWrite(int64_t) { return false; }
bool displayRefresh() { return false; }

const char* wakeReason() { return "power_on"; }
bool sleepFor(int, Sleep) { return false; }

}  // namespace board
