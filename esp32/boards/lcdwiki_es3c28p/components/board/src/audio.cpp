// An external Teensy Audio Shield (SGTL5000) wired to the expansion headers: I2S0 master at
// 48 kHz with MCLK = 256 Fs (12.288 MHz) on IO43, BCLK IO14 (64 Fs: 16-bit data in 32-bit
// slots), LRCLK IO21, DAC data out IO3, ADC data in IO2; control on the shared I2C bus
// (IO16/IO15) at 0x0A. The SGTL5000 answers I2C only while MCLK runs, so I2S starts first.
//
// The register sequence is the Teensy Audio library's AudioControlSGTL5000::enable(), with
// SYS_FS = 48 kHz. Headphones out; input from the mic (MIC/GND pads) or line in.
#include "board/board.h"
#include "audio.h"
#include "board/mic.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>

#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char* TAG = "sgtl5000";

namespace board {

static constexpr gpio_num_t kMclk = GPIO_NUM_43, kBclk = GPIO_NUM_14, kLrclk = GPIO_NUM_21,
                            kDout = GPIO_NUM_3, kDin = GPIO_NUM_2;
static constexpr int kRate = 48000;
static constexpr uint8_t kAddr = 0x0A;

// Registers
enum : uint16_t {
    CHIP_ID = 0x0000, CHIP_DIG_POWER = 0x0002, CHIP_CLK_CTRL = 0x0004, CHIP_I2S_CTRL = 0x0006,
    CHIP_SSS_CTRL = 0x000A, CHIP_ADCDAC_CTRL = 0x000E, CHIP_DAC_VOL = 0x0010,
    CHIP_ANA_ADC_CTRL = 0x0020, CHIP_ANA_HP_CTRL = 0x0022, CHIP_ANA_CTRL = 0x0024,
    CHIP_LINREG_CTRL = 0x0026, CHIP_REF_CTRL = 0x0028, CHIP_MIC_CTRL = 0x002A,
    CHIP_LINE_OUT_CTRL = 0x002C, CHIP_LINE_OUT_VOL = 0x002E, CHIP_ANA_POWER = 0x0030,
    CHIP_SHORT_CTRL = 0x003C,
};

static i2s_chan_handle_t sTx, sRx;
static i2c_master_dev_handle_t sCodec;
static uint16_t sChipId;
// ANA_CTRL: bit 4 MUTE_HP, bit 2 SELECT_ADC (1 = line in), bits 5/1 zero-cross. The Teensy
// sequence writes 0x0036 (headphones muted) and unmutes in volume(); here they start unmuted.
static uint16_t sAnaCtrl = 0x0026;
static bool sOutHp = true, sOutSpk = true;   // where tones and playback go ("out")

static bool reg(uint16_t r, uint16_t v) {
    const uint8_t b[4] = {(uint8_t) (r >> 8), (uint8_t) r, (uint8_t) (v >> 8), (uint8_t) v};
    return sCodec && i2c_master_transmit(sCodec, b, 4, 50) == ESP_OK;
}

static bool regRead(uint16_t r, uint16_t* v) {
    const uint8_t a[2] = {(uint8_t) (r >> 8), (uint8_t) r};
    uint8_t d[2] = {};
    if (!sCodec || i2c_master_transmit_receive(sCodec, a, 2, d, 2, 50) != ESP_OK) return false;
    *v = (uint16_t) (d[0] << 8 | d[1]);
    return true;
}

// Counters for the monitor's health (micAudioCounts): DMA buffers the output played without
// new data, input buffers lost because nobody read them, monitor writes that did not fit.
static std::atomic<int> sTxStarved{0}, sRxLost{0}, sTxShort{0};
static bool IRAM_ATTR onTxStarved(i2s_chan_handle_t, i2s_event_data_t*, void*) { sTxStarved++; return false; }
static bool IRAM_ATTR onRxOverflow(i2s_chan_handle_t, i2s_event_data_t*, void*) { sRxLost++; return false; }

static bool i2sInit() {
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;   // silence (not the last buffer) when nothing is written
    cc.dma_frame_num = 240;
    cc.dma_desc_num = 4;     // 4 x 5 ms: what the mic monitor adds as latency (micMonitorWrite)
    if (i2s_new_channel(&cc, &sTx, &sRx) != ESP_OK) return false;
    i2s_std_config_t sc = {};
    sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRate);
    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    sc.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;   // BCLK = 64 Fs, as the codec's SCLKFREQ 0
    sc.slot_cfg.ws_width = 32;   // LRCLK at 50 % (the preset's is the data width, 16: the codec then
                                 // read the right channel 16 clocks late, and that ear was silent)
    sc.gpio_cfg.mclk = kMclk;
    sc.gpio_cfg.bclk = kBclk;
    sc.gpio_cfg.ws = kLrclk;
    sc.gpio_cfg.dout = kDout;
    sc.gpio_cfg.din = kDin;
    if (i2s_channel_init_std_mode(sTx, &sc) != ESP_OK) return false;
    if (i2s_channel_init_std_mode(sRx, &sc) != ESP_OK) return false;
    i2s_event_callbacks_t tx = {}, rx = {};
    tx.on_send_q_ovf = onTxStarved;
    rx.on_recv_q_ovf = onRxOverflow;
    i2s_channel_register_event_callback(sTx, &tx, nullptr);
    i2s_channel_register_event_callback(sRx, &rx, nullptr);
    return i2s_channel_enable(sTx) == ESP_OK && i2s_channel_enable(sRx) == ESP_OK;
}

static void setHeadphoneVolume(float v) {   // 0..1 -> -51.5 dB .. +12 dB in 0.5 dB steps
    const int n = 0x7F - (int) std::lround(std::fmax(0, std::fmin(1, v)) * 0x7F);
    reg(CHIP_ANA_HP_CTRL, (uint16_t) (n << 8 | n));
}

void audioInit() {
    speakerInit();   // the board's own ES8311 and speaker
    if (!i2sInit()) { ESP_LOGW(TAG, "I2S init failed"); return; }
    vTaskDelay(pdMS_TO_TICKS(20));   // MCLK up before the codec is asked anything
    sCodec = i2cDevice(kAddr);
    if (!sCodec || !regRead(CHIP_ID, &sChipId)) {
        ESP_LOGI(TAG, "no SGTL5000 at 0x0A (Teensy Audio Shield not connected)");
        sCodec = nullptr;
        return;
    }
    reg(CHIP_ANA_POWER, 0x4060);      // VDDD from the shield's 1.8 V regulator
    reg(CHIP_LINREG_CTRL, 0x006C);    // VDDA and VDDIO over 3.1 V
    reg(CHIP_REF_CTRL, 0x01F2);       // VAG 1.575 V, normal ramp, +12.5 % bias
    reg(CHIP_LINE_OUT_CTRL, 0x0F22);  // LO_VAGCNTRL 1.65 V, 0.54 mA
    reg(CHIP_SHORT_CTRL, 0x4446);     // up to 125 mA
    reg(CHIP_ANA_CTRL, 0x0137);       // zero-cross detectors, everything muted
    reg(CHIP_ANA_POWER, 0x40FF);      // line out, headphones, ADC, DAC
    reg(CHIP_DIG_POWER, 0x0073);      // I2S in/out, DAP, DAC, ADC
    vTaskDelay(pdMS_TO_TICKS(400));
    reg(CHIP_LINE_OUT_VOL, 0x1D1D);
    reg(CHIP_CLK_CTRL, 0x0008);       // SYS_FS 48 kHz, MCLK = 256 Fs
    reg(CHIP_I2S_CTRL, 0x0030);       // slave, SCLK 64 Fs, 16-bit, I2S
    reg(CHIP_SSS_CTRL, 0x0010);       // ADC -> I2S out, I2S in -> DAC
    reg(CHIP_ADCDAC_CTRL, 0x0000);    // DAC unmuted
    reg(CHIP_DAC_VOL, 0x3C3C);        // 0 dB
    setHeadphoneVolume(0.5f);
    audioInput(true);                 // the mic
    ESP_LOGI(TAG, "SGTL5000 rev 0x%02X up: 48 kHz, headphones and mic", sChipId & 0xFF);
}

bool audioReady() { return sCodec != nullptr; }

void audioInput(bool mic) {
    if (!sCodec) return;
    if (mic) {
        reg(CHIP_MIC_CTRL, 0x0173);       // bias 3.0 V through 2 kΩ, +40 dB
        reg(CHIP_ANA_ADC_CTRL, 0x0088);   // ADC +12 dB
        sAnaCtrl &= ~(1 << 2);
    } else {
        reg(CHIP_ANA_ADC_CTRL, 0x0055);   // line in, +7.5 dB
        sAnaCtrl |= (1 << 2);
    }
    reg(CHIP_ANA_CTRL, sAnaCtrl);
}

// ── board/mic.h ──

bool micReady() { return sCodec != nullptr; }

static std::atomic<bool> sWatchRight{false};
static std::atomic<int> sRightNonzero{0}, sRightMax{0};
static int sRightLog[64][3], sRightSample;
void micRightLog() {
    for (int i = 0; i < std::min(64, sRightNonzero.load()); i++)
        printf("right %d: frame %d R 0x%04X L 0x%04X\n", i, sRightLog[i][0], sRightLog[i][1] & 0xFFFF, sRightLog[i][2] & 0xFFFF);
}
void micWatchRight(bool on, int* nonzero, int* maxAbs) {
    if (on) { sRightNonzero = 0; sRightMax = 0; sRightSample = 0; }
    sWatchRight = on;
    *nonzero = sRightNonzero;
    *maxAbs = sRightMax;
}

// While the console records (record: rec, loop, sweep), apps get silence: two readers of one
// I2S channel would each get every other block, and the recording would have gaps
static std::atomic<bool> sRecHold{false};

int micRead(int16_t* out, int frames, int timeoutMs) {
    if (!sCodec) return 0;
    if (sRecHold) {
        vTaskDelay(pdMS_TO_TICKS(std::max(1, frames * 1000 / kRate)));
        memset(out, 0, (size_t) frames * 2);
        return frames;
    }
    static int16_t buf[2 * 240];
    int n = 0;
    while (n < frames) {
        const int want = std::min(240, frames - n);
        size_t got = 0;
        if (i2s_channel_read(sRx, buf, (size_t) want * 4, &got, timeoutMs) != ESP_OK && got == 0) break;
        for (size_t i = 0; i + 1 < got / 2; i += 2) {
            if (sWatchRight) {   // a test (micWatchRight): the right slot, which apps don't read
                const int r = std::abs((int) buf[i + 1]);
                if (r) {
                    if (sRightNonzero < 64) { sRightLog[sRightNonzero][0] = sRightSample; sRightLog[sRightNonzero][1] = buf[i + 1]; sRightLog[sRightNonzero][2] = buf[i]; }
                    sRightNonzero++;
                }
                sRightSample++;
                if (r > sRightMax) sRightMax = r;
            }
            out[n++] = buf[i];
        }
    }
    return n;
}

static std::atomic<int> sMonitor{0};   // the mic monitor's percent (micMonitor)
static int sMicGainCode = 3;   // CHIP_MIC_CTRL's gain bits: +40 dB, as audioInput(true) sets it

void micGain(int db) {
    sMicGainCode = db >= 40 ? 3 : db >= 30 ? 2 : db >= 20 ? 1 : 0;
    reg(CHIP_MIC_CTRL, (uint16_t) (0x0170 | sMicGainCode));
}

void micInput(bool line) {
    if (!sCodec) return;
    if (line) {
        reg(CHIP_ANA_ADC_CTRL, 0x0000);   // line in at 0 dB (0x55 was +7.5: a preamp's line out needs none)
        sAnaCtrl |= (1 << 2);
    } else {
        reg(CHIP_MIC_CTRL, (uint16_t) (0x0170 | sMicGainCode));
        reg(CHIP_ANA_ADC_CTRL, 0x0088);   // ADC +12 dB, as audioInput(true)
        sAnaCtrl &= ~(1 << 2);
    }
    // the headphones' mute bit (micMonitor) stays as it is
    reg(CHIP_ANA_CTRL, sAnaCtrl | (sMonitor.load() == 0 ? 0x10 : 0));
}

// ── The mic monitor: what the app captured, back out of the headphones ──


void micMonitor(int percent) {
    if (!sCodec) return;
    percent = std::clamp(percent, 0, 100);
    if (percent == sMonitor.exchange(percent)) return;
    if (percent == 0) {
        reg(CHIP_ANA_CTRL, sAnaCtrl | 0x10);   // headphones muted: no hiss while off
        return;
    }
    // 1..100 % -> -40 .. 0 dB (CHIP_ANA_HP_CTRL: 0x00 is +12 dB, 0.5 dB a step)
    const float db = -40.0f + 40.0f * (float) percent / 100.0f;
    const int n = std::clamp((int) std::lround(2 * (12 - db)), 0, 0x7F);
    reg(CHIP_ANA_HP_CTRL, (uint16_t) (n << 8 | n));
    reg(CHIP_ANA_CTRL, sAnaCtrl);
}

void micAudioCounts(int* txStarved, int* rxLost, int* txShort) {
    *txStarved = sTxStarved.load();
    *rxLost = sRxLost.load();
    *txShort = sTxShort.load();
}

void micDigitalLoop(bool on) {   // the codec's I2S out carries its I2S in (not the ADC): a test
    reg(CHIP_SSS_CTRL, on ? 0x0011 : 0x0010);
}

void micMonitorWrite(const int16_t* mono, int frames) {
    if (!sCodec || sMonitor.load() == 0) return;
    static int16_t buf[2 * 240];
    while (frames > 0) {
        const int n = std::min(frames, 240);
        for (int i = 0; i < n; i++) buf[2 * i] = buf[2 * i + 1] = mono[i];
        size_t written = 0;
        // Waits for a free DMA buffer: input and output share the clock, so a buffer frees up as
        // each block arrives, and the queue stays full (4 x 5 ms of latency). Not waiting dropped
        // the blocks that came a moment early, and the codec played silence: a constant ticking.
        i2s_channel_write(sTx, buf, (size_t) n * 4, &written, 10);
        if (written < (size_t) n * 4) sTxShort++;
        mono += n;
        frames -= n;
    }
}

// ── Tones: a task plays the latest request; a newer one replaces it ──

// Tones and playback on the headphones too (out hp|both), unless the monitor plays there: the
// capture task then writes a block every 5 ms and holds the channel while it waits for room, at
// a higher priority, so a second writer never got the channel and waited forever, and the
// speaker after it never played. Never waits longer than a block.
static void headphonesWrite(const int16_t* stereo, int frames) {
    if (!sCodec || !sOutHp || sMonitor.load() != 0) return;
    size_t written = 0;
    i2s_channel_write(sTx, stereo, (size_t) frames * 4, &written, 10);
}

struct Tone { int hz, ms; };
static QueueHandle_t sTones;

static void toneTask(void*) {
    static int16_t buf[2 * 240];
    Tone t{};
    while (true) {
        xQueueReceive(sTones, &t, portMAX_DELAY);
        double phase = 0;
        int left = (int) ((int64_t) kRate * t.ms / 1000);
        const int fade = kRate / 200;   // 5 ms in and out: no clicks
        const int total = left;
        while (left > 0) {
            Tone next;
            if (xQueuePeek(sTones, &next, 0) == pdTRUE) break;   // replaced
            const int n = std::min(left, 240);
            for (int i = 0; i < n; i++) {
                const int k = total - left + i;
                const float env = std::fmin(1.0f, std::fmin((float) k / fade, (float) (total - k) / fade));
                const int16_t s = (int16_t) (std::sin(phase) * 9000 * env);
                phase += 2 * M_PI * t.hz / kRate;
                if (phase > 2 * M_PI) phase -= 2 * M_PI;
                buf[2 * i] = buf[2 * i + 1] = s;
            }
            headphonesWrite(buf, n);
            if (sOutSpk) speakerWrite(buf, n);
            left -= n;
        }
    }
}

bool beep(int hz, int ms) {
    if ((!sCodec && !speakerReady()) || hz <= 0 || ms <= 0) return false;
    if (!sTones) {
        sTones = xQueueCreate(1, sizeof(Tone));
        if (!sTones || xTaskCreatePinnedToCore(toneTask, "tone", 3072, nullptr, 5, nullptr, 0) != pdPASS)
            return false;
    }
    const Tone t{std::min(hz, kRate / 2 - 1), std::min(ms, 10000)};
    xQueueOverwrite(sTones, &t);
    return true;
}

// The input's level (peak and RMS, 0..1 of full scale) over `ms` milliseconds.
bool audioLevel(int ms, float* peak, float* rms) {
    if (!sCodec) return false;
    static int16_t buf[2 * 240];
    int n = kRate * ms / 1000;
    double sum = 0;
    int count = 0, pk = 0;
    i2s_channel_read(sRx, buf, sizeof buf, nullptr, 100);   // drop what queued up
    while (n > 0) {
        size_t got = 0;
        if (i2s_channel_read(sRx, buf, sizeof buf, &got, 100) != ESP_OK) return false;
        for (size_t i = 0; i < got / 2; i += 2) {   // left channel
            const int s = buf[i];
            pk = std::max(pk, std::abs(s));
            sum += (double) s * s;
            count++;
        }
        n -= (int) (got / 4);
    }
    *peak = pk / 32768.0f;
    *rms = count ? (float) std::sqrt(sum / count) / 32768.0f : 0;
    return true;
}

// ── Recording: mono (the left slot) into PSRAM, for checking the mic ──

static int16_t* sRec;
static int sRecLen;   // frames

static bool record(float seconds) {
    if (!sCodec) return false;
    const int want = (int) (seconds * kRate);
    if (!sRec || sRecLen < want) {
        heap_caps_free(sRec);
        sRec = (int16_t*) heap_caps_malloc((size_t) want * 2, MALLOC_CAP_SPIRAM);
        if (!sRec) { sRecLen = 0; return false; }
    }
    static int16_t buf[2 * 240];
    size_t got = 0;
    sRecHold = true;
    vTaskDelay(pdMS_TO_TICKS(20));   // the app's read in progress ends
    for (int i = 0; i < 8; i++) i2s_channel_read(sRx, buf, sizeof buf, &got, 50);   // stale DMA
    int n = 0;
    while (n < want) {
        if (i2s_channel_read(sRx, buf, sizeof buf, &got, 200) != ESP_OK) { sRecHold = false; return false; }
        for (size_t i = 0; i + 1 < got / 2 && n < want; i += 2) sRec[n++] = buf[i];
    }
    sRecHold = false;
    sRecLen = want;
    return true;
}

// "sweep": an exponential sine sweep (Farina) on the speaker alone, while record() takes it in.
// Starts after 0.2 s of the recording; the host deconvolves it (the room's impulse response).
static constexpr float kSweepF1 = 50, kSweepF2 = 20000, kSweepLead = 0.2f;
static float sSweepSecs = 2;
static int16_t* sSweep;   // mono, computed before it plays: per sample in real time (sin and exp
static int sSweepLen;     // of doubles, in software on this chip) it fell behind, with gaps

static bool sweepPrepare() {
    const int total = (int) (sSweepSecs * kRate), fade = kRate / 100;   // 10 ms fades
    if (!sSweep || sSweepLen < total) {
        heap_caps_free(sSweep);
        sSweep = (int16_t*) heap_caps_malloc((size_t) total * 2, MALLOC_CAP_SPIRAM);
        if (!sSweep) { sSweepLen = 0; return false; }
    }
    sSweepLen = total;
    const double L = sSweepSecs / std::log(kSweepF2 / kSweepF1);
    for (int k = 0; k < total; k++) {
        const double t = (double) k / kRate;
        const float env = std::fmin(1.0f, std::fmin((float) k / fade, (float) (total - k) / fade));
        sSweep[k] = (int16_t) (std::sin(std::fmod(2 * M_PI * kSweepF1 * L * (std::exp(t / L) - 1), 2 * M_PI)) * 9000 * env);
    }
    return true;
}

static void sweepTask(void*) {
    vTaskDelay(pdMS_TO_TICKS((int) (kSweepLead * 1000)));
    static int16_t buf[2 * 240];
    for (int k0 = 0; k0 < sSweepLen; k0 += 240) {
        const int n = std::min(240, sSweepLen - k0);
        for (int i = 0; i < n; i++) buf[2 * i] = buf[2 * i + 1] = sSweep[k0 + i];
        speakerWrite(buf, n);
    }
    vTaskDelete(nullptr);
}

// Power of one frequency in x (Goertzel), as an amplitude 0..1 of full scale.
static float toneAmplitude(const int16_t* x, int n, float hz) {
    const double k = 2 * cos(2 * M_PI * hz / kRate);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) {
        const double s0 = x[i] / 32768.0 + k * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double p = s1 * s1 + s2 * s2 - k * s1 * s2;
    return (float) (2 * std::sqrt(std::fmax(0, p)) / n);
}

static float db(float a) { return 20 * std::log10(std::fmax(a, 1e-6f)); }

static void analyse(const int16_t* x, int n) {
    double sum = 0, sum2 = 0;
    int peak = 0, clipped = 0;
    for (int i = 0; i < n; i++) {
        sum += x[i];
        peak = std::max(peak, std::abs((int) x[i]));
        if (x[i] >= 32700 || x[i] <= -32700) clipped++;
    }
    const double dc = sum / n;
    for (int i = 0; i < n; i++) sum2 += (x[i] - dc) * (x[i] - dc);
    // 50 ms windows: the quietest is the noise floor, the loudest the signal
    const int win = kRate / 20;
    float quiet = 1, loud = 0;
    for (int w0 = 0; w0 + win <= n; w0 += win) {
        double e = 0;
        for (int i = w0; i < w0 + win; i++) e += (x[i] - dc) * (x[i] - dc);
        const float r = (float) std::sqrt(e / win) / 32768.0f;
        quiet = std::min(quiet, r);
        loud = std::max(loud, r);
    }
    const float rms = (float) std::sqrt(sum2 / n) / 32768.0f;
    printf("rec: %.2f s, DC %+.4f, RMS %.1f dBFS, peak %.1f dBFS, clipped %d samples\n",
           (float) n / kRate, dc / 32768.0, db(rms), db(peak / 32768.0f), clipped);
    printf("rec: noise floor %.1f dBFS (quietest 50 ms), loudest 50 ms %.1f dBFS, range %.1f dB\n",
           db(quiet), db(loud), db(loud) - db(quiet));
}

static void dumpWav() {
    printf("\nWAV BEGIN %d %d\n", kRate, sRecLen);
    static unsigned char b64[1100];
    const unsigned char* p = (const unsigned char*) sRec;
    const size_t bytes = (size_t) sRecLen * 2;
    for (size_t off = 0; off < bytes; off += 768) {
        size_t olen = 0;
        mbedtls_base64_encode(b64, sizeof b64, &olen, p + off, std::min((size_t) 768, bytes - off));
        b64[olen] = 0;
        printf("%s\n", b64);
        if ((off / 768 & 31) == 31) fflush(stdout);
    }
    printf("WAV END\n");
    fflush(stdout);
}

static void playRecording() {
    static int16_t buf[2 * 240];
    for (int i = 0; i < sRecLen; i += 240) {
        const int n = std::min(240, sRecLen - i);
        for (int j = 0; j < n; j++) buf[2 * j] = buf[2 * j + 1] = sRec[i + j];
        headphonesWrite(buf, n);
        if (sOutSpk) speakerWrite(buf, n);
    }
}

bool audioConsole(const std::string& line) {
    if (line == "spkclk off" || line == "spkclk on") {   // a test: the speaker's I2S1 clocks
        speakerClock(line == "spkclk on");
        printf("%s\n", line.c_str());
        return true;
    }
    if (!line.compare(0, 4, "reg ")) {   // "reg ADDR [VALUE]" (hex): read or write a codec register
        unsigned a = 0, v = 0;
        const int n = sscanf(line.c_str() + 4, "%x %x", &a, &v);
        if (n == 2) reg((uint16_t) a, (uint16_t) v);
        uint16_t now = 0;
        if (n >= 1 && regRead((uint16_t) a, &now)) printf("reg 0x%04X = 0x%04X\n", a, now);
        else printf("reg: usage reg ADDR [VALUE] (hex)\n");
        return true;
    }
    if (!line.compare(0, 3, "rec")) {   // "rec [seconds]": record the mic, then measure it
        float sec = 5;
        sscanf(line.c_str() + 3, "%f", &sec);
        sec = std::fmax(0.2f, std::fmin(10, sec));
        printf("rec: recording %.1f s now\n", sec);
        fflush(stdout);
        if (!record(sec)) { printf("rec: failed (no codec or no memory)\n"); return true; }
        printf("rec: done\n");
        analyse(sRec, sRecLen);
        return true;
    }
    if (line == "wav") {   // the last recording as base64 16-bit mono (tools/rec.py)
        if (!sRecLen) printf("wav: nothing recorded\n");
        else dumpWav();
        return true;
    }
    if (line == "play") {   // the last recording, on the speaker (and the shield's headphones)
        if (!sRecLen) { printf("play: nothing recorded\n"); return true; }
        printf("play: %.1f s\n", (float) sRecLen / kRate);
        fflush(stdout);
        playRecording();
        printf("play: done\n");
        return true;
    }
    if (!line.compare(0, 4, "loop")) {   // "loop [hz]": the speaker plays a tone, the mic hears it
        int hz = 1000;
        sscanf(line.c_str() + 4, "%d", &hz);
        if (!sCodec || !speakerReady()) { printf("loop: needs the shield and the speaker\n"); return true; }
        audioInput(true);
        record(0.6f);   // silence first
        float quietAt = toneAmplitude(sRec, sRecLen, (float) hz);
        float other = toneAmplitude(sRec, sRecLen, hz * 1.37f);
        printf("loop: silence  %5d Hz %6.1f dBFS (control %d Hz %6.1f dBFS)\n", hz, db(quietAt),
               (int) (hz * 1.37f), db(other));
        beep(hz, 1200);
        vTaskDelay(pdMS_TO_TICKS(200));
        record(0.6f);
        const float on = toneAmplitude(sRec, sRecLen, (float) hz);
        other = toneAmplitude(sRec, sRecLen, hz * 1.37f);
        printf("loop: tone on  %5d Hz %6.1f dBFS (control %d Hz %6.1f dBFS)\n", hz, db(on),
               (int) (hz * 1.37f), db(other));
        printf("loop: the tone is %.1f dB above silence: %s\n", db(on) - db(quietAt),
               db(on) - db(quietAt) > 20 ? "the mic hears the speaker" : "weak or not heard");
        vTaskDelay(pdMS_TO_TICKS(600));
        return true;
    }
    if (!line.compare(0, 5, "sweep")) {   // "sweep [seconds]": the room's response (tools: sweep.py)
        float sec = 2;
        sscanf(line.c_str() + 5, "%f", &sec);
        sSweepSecs = std::fmax(0.5f, std::fmin(5, sec));
        if (!sCodec || !speakerReady()) { printf("sweep: needs the shield and the speaker\n"); return true; }
        if (!sweepPrepare()) { printf("sweep: no memory\n"); return true; }
        if (xTaskCreatePinnedToCore(sweepTask, "sweep", 3072, nullptr, 5, nullptr, 0) != pdPASS) {
            printf("sweep: no task\n");
            return true;
        }
        if (!record(kSweepLead + sSweepSecs + 1.0f)) { printf("sweep: recording failed\n"); return true; }
        printf("sweep: %.1f s, %g..%g Hz, starts at %.1f s; recorded %.2f s\n", sSweepSecs, kSweepF1,
               kSweepF2, kSweepLead, (float) sRecLen / kRate);
        analyse(sRec, sRecLen);
        return true;
    }
    if (!line.compare(0, 3, "out")) {   // "out hp|spk|both": where tones and playback go
        const std::string arg = line.size() > 4 ? line.substr(4) : "both";
        sOutHp = arg != "spk";
        sOutSpk = arg != "hp";
        printf("out: %s%s\n", sOutHp ? "headphones " : "", sOutSpk ? "speaker" : "");
        return true;
    }
    if (!line.compare(0, 7, "micgain")) {   // "micgain 0|20|30|40" dB (the mic preamp)
        int g = 40;
        sscanf(line.c_str() + 7, "%d", &g);
        const int code = g >= 40 ? 3 : g >= 30 ? 2 : g >= 20 ? 1 : 0;
        micGain(g);
        printf("micgain %d dB\n", code == 0 ? 0 : 10 + 10 * code);
        return true;
    }
    if (line == "micbias off" || line == "micbias on") {
        uint16_t v = 0;
        regRead(CHIP_MIC_CTRL, &v);
        reg(CHIP_MIC_CTRL, line == "micbias on" ? (uint16_t) (v | 0x0100) : (uint16_t) (v & ~0x0300));
        printf("%s\n", line.c_str());
        return true;
    }
    if (line == "spk") {
        printf("speaker: %s\n", speakerReady() ? "ES8311 up" : "not up");
        return true;
    }
    if (line == "audio") {
        uint16_t pw = 0;
        if (!sCodec) { printf("audio: no SGTL5000 at 0x0A\n"); return true; }
        regRead(CHIP_ANA_POWER, &pw);
        printf("audio: SGTL5000 id 0x%04X, ANA_POWER 0x%04X, 48 kHz\n", sChipId, pw);
        return true;
    }
    if (!line.compare(0, 4, "tone")) {   // "tone [hz] [ms]"
        int hz = 440, ms = 1000;
        sscanf(line.c_str() + 4, "%d %d", &hz, &ms);
        printf("tone %d Hz %d ms: %s\n", hz, ms, beep(hz, ms) ? "playing" : "no codec");
        return true;
    }
    if (!line.compare(0, 6, "volume")) {   // "volume [hp|spk] 0..100" (both without a target)
        int v = 50;
        const char* rest = line.c_str() + 6;
        const bool hp = strstr(rest, "hp") != nullptr, spk = strstr(rest, "spk") != nullptr;
        while (*rest && !isdigit((unsigned char) *rest)) rest++;
        sscanf(rest, "%d", &v);
        if (hp || !spk) setHeadphoneVolume(v / 100.0f);
        if (spk || !hp) speakerVolume(v / 100.0f);
        // 0 really mutes the headphones (the lowest step is still -51.5 dB)
        if (hp || !spk) reg(CHIP_ANA_CTRL, v == 0 ? (sAnaCtrl | 0x10) : sAnaCtrl);
        printf("volume %s%d\n", hp ? "hp " : spk ? "spk " : "", v);
        return true;
    }
    if (line == "mic" || line == "line") {
        audioInput(line == "mic");
        printf("input: %s\n", line.c_str());
        for (int i = 0; i < 20; i++) {
            float pk = 0, rms = 0;
            if (!audioLevel(250, &pk, &rms)) { printf("level: no codec\n"); break; }
            const int bars = (int) std::lround(std::fmin(1.0f, rms * 8) * 40);
            printf("level peak %.3f rms %.4f |%.*s\n", pk, rms, bars, "########################################");
        }
        return true;
    }
    return false;
}

}  // namespace board
