// Kobento RC player — a minimal sample app.
//
// kobento-mcu is a library for *displaying* RemoteCompose documents. This app is the smallest
// thing that exercises it on an ESP32 board: it embeds the documents in documents/ (and
// documents/fit/, documents/3d/), shows the first one, and lets you step through the rest with
// the board's button or the serial console. No networking — document delivery (kobento-net)
// and integrations (kobento-mcuApps) live in their own repositories.
//
// Documents drive the player with hostAction(0, "name[=value]") (docs/RC_ACTION_PATTERNS.md):
//   system.backlight=<0..1>|cycle   doc.show=<name>   doc.next   doc.back
// A document that declares the named variables Control.next / Control.back receives the board's
// buttons as press counts (instead of them stepping through the documents).
//
// On a board with a knob (board/knob.h), a document that declares Control.knob receives the
// knob's position in detents (kept per document, clamped to Control.knob_min/_max when it
// declares them; it starts at Control.knob_start), Control.press counts short presses (or, in
// a menu that exports the text Control.items, "name name ...", a press opens item
// Control.open, or by default the knob position modulo the item count) and Control.knob_down is 1 while the knob
// is held. Other documents: turning steps through the documents, a press is Control.next if
// they declare it, else a tap at the centre of the screen (the switch is the display pressed). A
// long press (0.8 s) goes home to knob_menu (from it: to the next document). A document that exports the colour
// Control.led (or Control.led0..4, clockwise from the top) lights the LED ring; otherwise the
// ring shows which document is up.
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "rcstage/stage.h"
#include "rcstage/device_state.h"
#include "board/board.h"
#if __has_include("board/knob.h")
#include "board/knob.h"
#define RCPLAYER_KNOB 1
#endif
#include <map>
#include <vector>

#include "console.h"
#include "embedded_docs.h"

static const char* TAG = "rcplayer";

static constexpr int kDocs = sizeof(kEmbeddedDocs) / sizeof(kEmbeddedDocs[0]);
static int sCurrent = 0, sPrevious = 0;

// A document may come in variants for particular screens, shown wherever <name> is asked for:
// "<name>_round" on a round screen, "<name>_light" (a white palette) on e-ink.
struct Variant { const char* suffix; bool mine; };
static constexpr Variant kVariants[] = {{"_round", board::kRound}, {"_light", board::kEink}};

static int findExact(const std::string& name) {
    for (int i = 0; i < kDocs; i++)
        if (name == kEmbeddedDocs[i].name) return i;
    return -1;
}
static int findDoc(const std::string& name) {
    for (const Variant& v : kVariants) {
        if (!v.mine) continue;
        int i = findExact(name + v.suffix);
        if (i >= 0) return i;
    }
    return findExact(name);
}
// The step cycle shows each document once, as this board opens it: it skips the variants meant
// for other screens, and the plain document when this board has a variant.
static bool otherScreen(int i) {
    std::string n = kEmbeddedDocs[i].name;
    for (const Variant& v : kVariants) {
        const size_t len = strlen(v.suffix);
        if (n.size() > len && n.compare(n.size() - len, len, v.suffix) == 0) {
            n.resize(n.size() - len);
            break;
        }
    }
    return findDoc(n) != i;
}

// Player task: open embedded document i, skipping any that fail. `step` (+1 or -1) is the
// direction to skip in when i is a variant for another screen; 0 opens i as asked.
static void open(int i, int step = 0) {
    const int from = sCurrent;
    for (int tries = 0; tries < kDocs; tries++, i += step ? step : 1) {
        const int at = ((i % kDocs) + kDocs) % kDocs;
        if (step && otherScreen(at)) continue;
        sCurrent = at;
        const auto& d = kEmbeddedDocs[sCurrent];
        if (rcstage::load(d.start, d.end - d.start, d.name, d.fit)) {
            if (sCurrent != from) sPrevious = from;
            return;
        }
    }
}

// ── Buttons and host actions (player task) ───────────────────────────────────────────────

static int sNextPresses = 0, sBackPresses = 0;
static int sBacklight = 100;

// The board's buttons go to the document when it declares Control.next / .back, else they step
// through the documents.
static void button(int step) {
    if (!rcstage::declaresNamed("Control.next") && !rcstage::declaresNamed("Control.back")) {
        open(sCurrent + step, step);
        return;
    }
    if (step > 0) rcstage::setNamed("Control.next", std::to_string(++sNextPresses));
    else rcstage::setNamed("Control.back", std::to_string(++sBackPresses));
}

static void runHostAction(const std::string& name, const std::string& value) {
    if (name == "system.backlight") {
        // A fraction 0..1, or "cycle" through 100 -> 60 -> 30 %.
        int pct = value == "cycle" ? (sBacklight > 60 ? 60 : sBacklight > 30 ? 30 : 100)
                                   : (int) (std::strtof(value.c_str(), nullptr) * 100);
        sBacklight = std::max(5, std::min(100, pct));
        board::setBrightness(sBacklight);
        rcstage::setNamed("system.backlight", std::to_string(sBacklight / 100.0f));
    } else if (name == "doc.show") {
        int i = findDoc(value);
        if (i >= 0) open(i);
        else ESP_LOGW(TAG, "no document '%s'", value.c_str());
    } else if (name == "doc.next") {
        open(sCurrent + 1, 1);
    } else if (name == "doc.back") {
        open(sPrevious);
    } else {
        ESP_LOGW(TAG, "unknown host action '%s'", name.c_str());
    }
}


#ifdef RCPLAYER_KNOB
// ── The knob and the LED ring (player task) ──────────────────────────────────────────────

static std::map<std::string, int> sKnobByDoc;   // each document's own knob position
static std::string sKnobDoc;
static int sKnobLast = INT_MIN;
static int sPresses = 0;
static bool sSwWas = false;

static bool namedNumber(const char* name, float* out) {
    std::string v;
    if (!rcstage::namedValueNow(name, v) || v.empty()) return false;
    *out = std::strtof(v.c_str(), nullptr);
    return true;
}

static int clampKnob(int v) {
    float lo, hi;
    if (namedNumber("Control.knob_min", &lo)) v = std::max(v, (int) std::lround(lo));
    if (namedNumber("Control.knob_max", &hi)) v = std::min(v, (int) std::lround(hi));
    return v;
}

// "#AARRGGBB" -> 0xRRGGBB with the alpha applied.
static uint32_t ledColour(const std::string& v) {
    if (v.size() < 2 || v[0] != '#') return 0;
    uint32_t c = (uint32_t) std::strtoul(v.c_str() + 1, nullptr, 16);
    const uint32_t a = v.size() > 7 ? (c >> 24) : 255;
    auto ch = [&](int shift) { return (((c >> shift) & 0xFF) * a / 255) << shift; };
    return ch(16) | ch(8) | ch(0);
}

static void ledsTick() {
    static int64_t next = 0;
    const int64_t now = esp_timer_get_time();
    if (now < next) return;
    next = now + 20000;   // 50 Hz is plenty for the eye
    uint32_t rgb[board::kLedCount] = {};
    std::string v;
    if (rcstage::namedValueNow("Control.led0", v)) {
        for (int i = 0; i < board::kLedCount; i++) {
            const std::string n = "Control.led" + std::to_string(i);
            if (rcstage::namedValueNow(n, v)) rgb[i] = ledColour(v);
        }
    } else if (rcstage::namedValueNow("Control.led", v)) {
        for (auto& c : rgb) c = ledColour(v);
    } else {
        // Which document is up: one dim LED, walking round as the documents change.
        rgb[sCurrent % board::kLedCount] = 0x081830;
    }
    board::setLeds(rgb, board::kLedCount);
}

static void knobTick() {
    const bool wants = rcstage::declaresNamed("Control.knob");
    const std::string doc = rcstage::currentName();
    if (doc != sKnobDoc) {   // a new document: give it its own position back
        sKnobDoc = doc;
        if (wants) {
            // First time: from Control.knob_start when it declares one.
            float start = 0;
            if (!sKnobByDoc.count(doc) && namedNumber("Control.knob_start", &start))
                sKnobByDoc[doc] = (int) std::lround(start);
            rcstage::setNamed("Control.knob", std::to_string(clampKnob(sKnobByDoc[doc])));
        }
    }

    const int pos = board::knobPosition();
    if (sKnobLast == INT_MIN) sKnobLast = pos;
    const int delta = pos - sKnobLast;
    sKnobLast = pos;
    if (delta) {
        if (wants) {
            int& mine = sKnobByDoc[doc];
            mine = clampKnob(mine + delta);
            rcstage::setNamed("Control.knob", std::to_string(mine));
        } else {
            const int step = delta > 0 ? 1 : -1;
            open(sCurrent + step, step);
        }
    }

    static int shortSeen = board::knobShortPresses(), longSeen = board::knobLongPresses();
    const int longs = board::knobLongPresses(), shorts = board::knobShortPresses();
    if (longs != longSeen) {   // long press: home (the menu), from it the next
        longSeen = longs;
        const int menu = findDoc("knob_menu");
        if (menu >= 0 && menu != sCurrent) open(menu);
        else open(sCurrent + 1, 1);
    }
    for (; shortSeen != shorts; shortSeen++) {
        std::string items;
        float pick = 0;
        if (rcstage::namedValueNow("Control.items", items) && !items.empty()) {
            // A menu: open item `pick` of the space-separated document names.
            std::vector<std::string> names;
            for (size_t at = 0; at < items.size();) {
                size_t end = items.find(' ', at);
                if (end == std::string::npos) end = items.size();
                if (end > at) names.push_back(items.substr(at, end - at));
                at = end + 1;
            }
            // Control.open when the document exports it, else the knob position (mod the items).
            const int n = (int) names.size();
            if (!namedNumber("Control.open", &pick)) pick = (float) sKnobByDoc[doc];
            const int i = n ? (((int) std::lround(pick) % n) + n) % n : -1;
            if (i >= 0) runHostAction("doc.show", names[i]);
            shortSeen = shorts - 1;   // a new document: the presses queued behind it are dropped
        } else if (rcstage::declaresNamed("Control.press")) {
            rcstage::setNamed("Control.press", std::to_string(++sPresses));
        } else if (rcstage::declaresNamed("Control.next")) {
            button(+1);
        } else {
            // The knob's switch is the whole display pressed in: a tap at its centre.
            rcstage::click();
        }
    }
    const bool sw = board::knobDown();
    if (sw != sSwWas) rcstage::setNamed("Control.knob_down", sw ? "1" : "0");
    sSwWas = sw;

    ledsTick();
}
#endif

static void tick() {
    console::Request req;
    while (console::poll(req)) {
        switch (req.cmd) {
            case console::Cmd::Snap:
                board::dumpFrame(board::kWidth > 400 ? 2 : 1);   // ~360 px wide
                break;
            case console::Cmd::Res: rcstage::setRenderDiv(req.arg); break;
            case console::Cmd::Next: open(sCurrent + 1, 1); break;
            case console::Cmd::Prev: open(sCurrent - 1, -1); break;
            case console::Cmd::Doc: open(req.arg); break;
            case console::Cmd::List:
                for (int i = 0; i < kDocs; i++)
                    printf("%c %d %s\n", i == sCurrent ? '*' : ' ', i, kEmbeddedDocs[i].name);
                break;
            case console::Cmd::Stats: rcstage::requestStats(); break;
            case console::Cmd::Bench: rcstage::bench(); break;
            case console::Cmd::Act: {
                std::string md = req.text;
                size_t eq = md.find('=');
                printf("act %s\n", md.c_str());
                runHostAction(md.substr(0, eq), eq == std::string::npos ? "" : md.substr(eq + 1));
                break;
            }
            default: break;
        }
    }
    if (int step = board::buttonPressed()) button(step);
#ifdef RCPLAYER_KNOB
    knobTick();
#endif
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Kobento RC player on %s", board::kName);
    console::start();
    if (!board::displayInit()) return;
    rcstage::Hooks hooks;
    hooks.tick = tick;
    hooks.longPress = [] { open(sCurrent + 1, 1); };
    hooks.hostAction = [](int id, const std::string& md) {
        if (id != 0) return;
        size_t eq = md.find('=');
        runHostAction(md.substr(0, eq), eq == std::string::npos ? "" : md.substr(eq + 1));
    };
    hooks.namedAction = [](const std::string& name, int, const std::string& value) {
        runHostAction(name, value);
    };
    // Start on the knob menu (knob boards), else the Kobento splash if it is built in, else the
    // first document.
    sCurrent = sPrevious = findDoc("knob_menu");
    if (sCurrent < 0) sCurrent = sPrevious = std::max(0, findDoc("kobento_splash"));
    const auto& d = kEmbeddedDocs[sCurrent];
    rcstage::start(hooks, d.start, d.end - d.start, d.name);
    rcstage::deviceStateStart();   // battery / memory / clock as system variables, for the docs
}
