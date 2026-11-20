// rc2png: render a RemoteCompose document with the ThorVG backend (the same code the Tab5
// runs) to a PNG, for comparison against the Skia reference renderer (rc2image).
//
//   rc2png in.rc out.png [--fit W H] [--native] [--theme light|dark] [--bg AARRGGBB]
//                        [--density D] [--frames N] [--anim SEC] [--time MS] [--bench N]
//                        [--sys ID=VALUE]... [--named NAME=VALUE]... [--bitmap NAME=W,H,FILE]... [--click X,Y]...
//                        [--drag X0,Y0,X1,Y1]... [--print PATTERN]... [--live NAME=VALUE]... [--live-ms MS] [--live-dump PREFIX]
//                        [--damage N] [--vary NAME=LO:HI[:COUNT]]... [--step-ms MS]
//
// --damage N draws N more frames, the clock (and --time) advancing --step-ms (33) each, every
// --vary variable moving between LO and HI (a list of COUNT values when COUNT is given), and
// prints how much of the frame changed from the one before: the box around the changed
// pixels, the 16 x 16 tiles they touch, and the pixels themselves. The least a renderer that
// redraws only what changed would have to redraw (and send).
//
// Default is "player" mode, the way a device shows a document: the document is laid out
// into the W x H surface (window size vars 5/6 set), light theme. --native instead paints
// the document at its own size, scaled to fit (rc2image --fit semantics).
#include "rccore/CoreDocument.h"
#include "rccore/RemoteContext.h"
#include "rccore/WireBuffer.h"
#include "rccore/operations/DrawOperations.h"   // NamedVariable
#include "kobentovg/ThorvgPaintContext.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <chrono>
#include <functional>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

static std::vector<uint8_t> readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

static void be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}

static void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> c;
    be32(c, (uint32_t) data.size());
    c.insert(c.end(), type, type + 4);
    c.insert(c.end(), data.begin(), data.end());
    be32(c, (uint32_t) crc32(0, c.data() + 4, (uInt) (c.size() - 4)));
    fwrite(c.data(), 1, c.size(), f);
}

// Premultiplied ARGB8888 -> RGBA PNG.
static bool writePng(const char* path, const uint32_t* px, int w, int h) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t) (w * 4 + 1) * h);
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        for (int x = 0; x < w; x++) {
            uint32_t p = px[(size_t) y * w + x];
            uint32_t a = p >> 24, r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            if (a && a < 255) { r = r * 255 / a; g = g * 255 / a; b = b * 255 / a; }
            raw.push_back(r); raw.push_back(g); raw.push_back(b); raw.push_back(a);
        }
    }
    uLongf zlen = compressBound((uLong) raw.size());
    std::vector<uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), (uLong) raw.size(), 6) != Z_OK) return false;
    z.resize(zlen);
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    be32(ihdr, w); be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(f, "IHDR", ihdr);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    fclose(f);
    return true;
}

static void loadFont(kobentovg::ThorvgPaintContext::Face face, const char* file,
                     std::vector<std::vector<uint8_t>>& keep) {
    keep.push_back(readFile((std::string(FONT_DIR) + "/" + file).c_str()));
    if (keep.back().empty() ||
        !kobentovg::ThorvgPaintContext::registerFont(face, keep.back().data(), keep.back().size()))
        std::cerr << "warning: font " << file << " not loaded\n";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: rc2png in.rc out.png [--fit W H] [--native] [--theme light|dark]"
                     " [--bg AARRGGBB] [--density D] [--frames N] [--anim SEC] [--time MS]"
                     " [--bench N] [--bench-var NAME] [--sys ID=VALUE]... [--named NAME=VALUE]... [--bitmap NAME=W,H,FILE]... [--click X,Y]... [--drag X0,Y0,X1,Y1]... [--print PATTERN]...\n";
        return 1;
    }
    int fitW = 0, fitH = 0, frames = 1, bench = 0;
    std::string benchVar;   // --bench-var: a named float changed every bench frame (forces the layout pass)
    float anim = -1;
    int64_t timeMs = -1;
    bool native = false;
    int theme = -3;                  // Theme.LIGHT, a phone's default
    uint32_t bg = 0x00000000;        // transparent, like the TS reference
    float density = 1;
    std::vector<std::pair<int, float>> sys;                   // system variables, by id
    // --click X,Y and --drag X0,Y0,X1,Y1 (dp), in the order given: a click is a drag of none.
    struct Gesture { bool click; std::array<float, 4> at; };
    std::vector<Gesture> gestures;
    std::vector<std::string> prints;                          // --print NAME or PREFIX*
    std::vector<std::pair<std::string, std::string>> live;    // --live NAME=VALUE
    int liveMs = 16;                                          // --live-ms: a frame's time, live
    std::string liveDump;                                     // --live-dump PREFIX: every live frame
    std::vector<std::pair<std::string, std::string>> named;   // NAMED_VARIABLE overrides
    struct HostBitmap { std::string name; int w, h; std::string file; };
    std::vector<HostBitmap> bitmaps;   // --bitmap NAME=W,H,FILE: a host-drawn bitmap's pixels
    int damage = 0, stepMs = 33;
    struct Vary { std::string name; float lo, hi; int count; };
    std::vector<Vary> varies;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--damage") && i + 1 < argc) { damage = atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "--step-ms") && i + 1 < argc) { stepMs = std::max(1, atoi(argv[++i])); continue; }
        if (!strcmp(argv[i], "--vary") && i + 1 < argc) {
            std::string kv = argv[++i];
            const size_t eq = kv.find('=');
            Vary v{kv.substr(0, eq), 0, 1, 0};
            if (eq != std::string::npos) sscanf(kv.c_str() + eq + 1, "%f:%f:%d", &v.lo, &v.hi, &v.count);
            varies.push_back(v);
            continue;
        }
        if (!strcmp(argv[i], "--fit") && i + 2 < argc) { fitW = atoi(argv[++i]); fitH = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::max(1, atoi(argv[++i]));
        else if (!strcmp(argv[i], "--anim") && i + 1 < argc) anim = (float) atof(argv[++i]);
        else if (!strcmp(argv[i], "--time") && i + 1 < argc) timeMs = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bench-var") && i + 1 < argc) benchVar = argv[++i];
        else if (!strcmp(argv[i], "--native")) native = true;
        else if (!strcmp(argv[i], "--click") && i + 1 < argc) {
            Gesture g{true, {}};
            sscanf(argv[++i], "%f,%f", &g.at[0], &g.at[1]);
            gestures.push_back(g);
        }
        else if (!strcmp(argv[i], "--drag") && i + 1 < argc) {
            Gesture g{false, {}};
            sscanf(argv[++i], "%f,%f,%f,%f", &g.at[0], &g.at[1], &g.at[2], &g.at[3]);
            gestures.push_back(g);
        }
        else if (!strcmp(argv[i], "--print") && i + 1 < argc) prints.push_back(argv[++i]);
        else if (!strcmp(argv[i], "--live-ms") && i + 1 < argc) liveMs = std::max(1, atoi(argv[++i]));
        else if (!strcmp(argv[i], "--live-dump") && i + 1 < argc) liveDump = argv[++i];
        else if (!strcmp(argv[i], "--live") && i + 1 < argc) {
            std::string kv = argv[++i];
            size_t eq = kv.find('=');
            if (eq != std::string::npos) live.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        }
        else if (!strcmp(argv[i], "--theme") && i + 1 < argc) theme = strcmp(argv[++i], "dark") ? -3 : -2;
        else if (!strcmp(argv[i], "--bg") && i + 1 < argc) bg = (uint32_t) strtoul(argv[++i], nullptr, 16);
        else if (!strcmp(argv[i], "--density") && i + 1 < argc) density = (float) atof(argv[++i]);
        else if (!strcmp(argv[i], "--bitmap") && i + 1 < argc) {
            // NAME=W,H,FILE: FILE holds W x H premultiplied ARGB8888, little-endian uint32 each
            std::string kv = argv[++i];
            size_t eq = kv.find('='), c1 = kv.find(',', eq), c2 = kv.find(',', c1 + 1);
            if (eq == std::string::npos || c1 == std::string::npos || c2 == std::string::npos) continue;
            bitmaps.push_back({kv.substr(0, eq), atoi(kv.c_str() + eq + 1), atoi(kv.c_str() + c1 + 1), kv.substr(c2 + 1)});
        }
        else if ((!strcmp(argv[i], "--sys") || !strcmp(argv[i], "--named")) && i + 1 < argc) {
            bool isSys = argv[i][2] == 's';
            std::string kv = argv[++i];
            size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            if (isSys) sys.emplace_back(atoi(kv.c_str()), (float) atof(kv.c_str() + eq + 1));
            else named.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        }
    }

    tvg::Initializer::init(0);
    std::vector<std::vector<uint8_t>> fonts;
    using Face = kobentovg::ThorvgPaintContext::Face;
    loadFont(Face::Sans, "Roboto-Regular.ttf", fonts);
    loadFont(Face::SansBold, "Roboto-Bold.ttf", fonts);
    loadFont(Face::Mono, "RobotoMono-Regular.ttf", fonts);

    std::vector<uint8_t> data = readFile(argv[1]);
    if (data.empty()) { std::cerr << "cannot read " << argv[1] << "\n"; return 1; }
    rccore::WireBuffer buf(data.data(), data.size());
    rccore::CoreDocument doc;
    if (!doc.initFromBuffer(buf)) { std::cerr << "parse failed\n"; return 1; }
    if (timeMs >= 0) doc.setFixedTimeMs(timeMs);

    int dw = doc.getWidth() > 0 ? doc.getWidth() : 600;
    int dh = doc.getHeight() > 0 ? doc.getHeight() : 600;
    int sw = fitW > 0 ? fitW : dw, sh = fitH > 0 ? fitH : dh;
    float scale = 1, ox = 0, oy = 0;
    if (!native) {
        // Player mode: the document lays out into the surface, in dp.
        scale = density;
        dw = (int) (sw / density);
        dh = (int) (sh / density);
        doc.setWidth(dw);
        doc.setHeight(dh);
    } else if (fitW > 0) {
        scale = std::min((float) sw / dw, (float) sh / dh);
        ox = (sw - dw * scale) * 0.5f;
        oy = (sh - dh * scale) * 0.5f;
    }

    std::vector<uint32_t> px((size_t) sw * sh);
    rccore::RemoteContext ctx;
    kobentovg::ThorvgPaintContext pc(ctx, px.data(), sw, sh);
    ctx.setPaintContext(&pc);
    ctx.setDocument(&doc);
    ctx.mWidth = (float) dw;
    ctx.mHeight = (float) dh;
    doc.registerListeners(ctx);
    ctx.loadFloat(5, (float) dw);    // window width / height system variables
    ctx.loadFloat(6, (float) dh);
    if (anim >= 0) ctx.overrideFloat(rccore::RemoteContext::ID_ANIMATION_TIME, anim);
    for (auto& [id, v] : sys) ctx.loadFloat(id, v);
    doc.applyDataOperations(ctx, theme);
    // As the Tab5 player does (rcstage Player::setNamed): types 0 string, 1 float, 2 color,
    // 4 integer.
    // The named variables, wherever they are (a canvas may declare one), as rcstage finds them.
    std::vector<const rccore::NamedVariable*> vars;
    std::function<void(const std::vector<std::unique_ptr<rccore::Operation>>&)> collect =
        [&](const std::vector<std::unique_ptr<rccore::Operation>>& ops) {
            for (auto& op : ops) {
                if (auto* nv = dynamic_cast<const rccore::NamedVariable*>(op.get())) vars.push_back(nv);
                if (!op->mChildren.empty()) collect(op->mChildren);
            }
        };
    collect(doc.getOperations());
    auto setNamed = [&](const std::string& n, const std::string& v) {
        for (auto* nv : vars) {
            if (nv->varName != n) continue;
            if (nv->varType == 0) ctx.overrideText(nv->varId, v);
            else if (nv->varType == 1) ctx.overrideFloat(nv->varId, (float) atof(v.c_str()));
            else if (nv->varType == 2) {
                const char* h = v.c_str() + (v[0] == '#');
                uint32_t c = (uint32_t) strtoul(h, nullptr, 16);
                if (strlen(h) <= 6) c |= 0xFF000000u;
                ctx.overrideColor(nv->varId, (int) c);
            } else if (nv->varType == 4) ctx.overrideInteger(nv->varId, atoi(v.c_str()));
            else if (nv->varType == 6) {   // float array: numbers separated by commas or spaces
                std::vector<float> f;
                const char* p = v.c_str();
                char* end = nullptr;
                while (*p) {
                    const float x = std::strtof(p, &end);
                    if (end == p) { p++; continue; }
                    f.push_back(x);
                    p = end;
                }
                ctx.overrideFloatList(nv->varId, f);
            }
        }
    };
    for (auto& [n, v] : named) setNamed(n, v);
    for (auto& b : bitmaps) {   // as rcstage::setNamedBitmap
        std::ifstream f(b.file, std::ios::binary);
        std::vector<uint32_t> pxs((size_t) b.w * b.h);
        if (!f.read(reinterpret_cast<char*>(pxs.data()), (std::streamsize) (pxs.size() * 4))) {
            std::fprintf(stderr, "--bitmap %s: can't read %d x %d pixels from %s\n", b.name.c_str(), b.w, b.h, b.file.c_str());
            continue;
        }
        bool found = false;
        for (auto* nv : vars)
            if (nv->varName == b.name && nv->varType == 3) found = pc.setImagePixels(nv->varId, b.w, b.h, std::move(pxs));
        if (!found) std::fprintf(stderr, "--bitmap %s: the document has no such image\n", b.name.c_str());
    }

    const tvg::Matrix base{scale, 0, ox, 0, scale, oy, 0, 0, 1};
    auto frame = [&] {
        pc.beginFrame(base, bg);
        doc.paint(ctx, theme);
        pc.endFrame();
    };
    for (int i = 0; i < frames; i++) frame();
    // Gestures (dp), after a frame has laid the document out. A click prints what it runs. A
    // drag is a finger down at X0,Y0, moved to X1,Y1 in ten steps and lifted at rest; then the
    // animation clock runs 5 s on, so whatever eases after the release (a touch expression
    // settling on a notch) has settled.
    ctx.hostAction = [](int id, const std::string& md) { std::printf("  hostAction(%d, \"%s\")\n", id, md.c_str()); };
    ctx.hostNamedAction = [](const std::string& n, int, const std::string& v) { std::printf("  namedAction %s = %s\n", n.c_str(), v.c_str()); };
    for (auto& g : gestures) {
        auto& d = g.at;
        if (g.click) {
            std::printf("click %.0f,%.0f:\n", d[0], d[1]);
            doc.touchDown(ctx, d[0], d[1]);
            doc.touchUp(ctx, d[0], d[1], 0, 0);
            doc.onClick(ctx, d[0], d[1]);
            frame();
            continue;
        }
        float t = ctx.getAnimationTime();
        std::printf("drag %.0f,%.0f -> %.0f,%.0f\n", d[0], d[1], d[2], d[3]);
        doc.touchDown(ctx, d[0], d[1]);
        for (int s = 1; s <= 10; s++) {
            ctx.overrideFloat(rccore::RemoteContext::ID_ANIMATION_TIME, t += 0.016f);
            doc.touchDrag(ctx, d[0] + (d[2] - d[0]) * s / 10, d[1] + (d[3] - d[1]) * s / 10);
            frame();
        }
        doc.touchUp(ctx, d[2], d[3], 0, 0);
        frame();
        ctx.overrideFloat(rccore::RemoteContext::ID_ANIMATION_TIME, t + 5);
        frame();
        frame();
    }

    // Live: each NAME=VALUE set on the real clock, then frames drawn only while the engine asks
    // for them (needsRepaint, getRepaintDelay 0), as a device player schedules them.
    for (auto& [n, v] : live) {
        setNamed(n, v);
        int drawn = 0;
        do {
            std::this_thread::sleep_for(std::chrono::milliseconds(liveMs));
            frame();
            drawn++;
            if (!liveDump.empty()) {   // PREFIX-NNN.png, numbered across all the live changes
                static int dumped = 0;
                char path[512];
                snprintf(path, sizeof path, "%s-%03d.png", liveDump.c_str(), dumped++);
                writePng(path, px.data(), sw, sh);
            }
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (!pc.doesNeedsRepaint() && ctx.getRepaintDelay(now) != 0) break;
        } while (drawn < 300);
        std::printf("live %s=%s: %d frames\n", n.c_str(), v.c_str(), drawn);
    }

    // The named variables asked for, as they end up: NAME, or PREFIX* for a class.
    for (auto* nv : vars) {
        for (auto& pat : prints) {
            const bool hit = !pat.empty() && pat.back() == '*' ? nv->varName.compare(0, pat.size() - 1, pat, 0, pat.size() - 1) == 0
                                                               : nv->varName == pat;
            if (!hit) continue;
            if (nv->varType == 0) std::printf("  %s = \"%s\"\n", nv->varName.c_str(), ctx.getText(nv->varId).c_str());
            else std::printf("  %s = %g\n", nv->varName.c_str(), ctx.getFloat(nv->varId));
            break;
        }
    }

    if (damage > 0) {
        // Each variable moves smoothly with some jitter, as a level or a spectrum does.
        uint32_t seed = 1;
        auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
        auto wave = [&](int f, int k) {
            const float u = 0.5f + 0.35f * std::sin(f * 0.21f + k * 0.7f) + 0.15f * (rnd() - 0.5f);
            return std::clamp(u, 0.0f, 1.0f);
        };
        constexpr int kTile = 16;
        const int tw = (sw + kTile - 1) / kTile, th = (sh + kTile - 1) / kTile;
        std::vector<uint32_t> prev = px;
        // Two buffers in turn, as a device double-buffers (ThorvgPaintContext::retarget).
        std::vector<uint32_t> alt = px;
        const bool crc = getenv("RC_FRAME_CRC") != nullptr;   // each frame's CRC, to diff two runs
        int kept = 0, made = 0;
        double sumDrawn = 0;   // what the renderer rastered (ThorvgPaintContext::drawn), % of the frame
        // A panel sent only the drawn regions, as a board's presentAsyncRegions: where it ends up
        // unlike the frame, a partial send leaves stale pixels.
        std::vector<uint32_t> panel = px;
        long stale = 0;
        int staleFrames = 0;
        float t = ctx.getAnimationTime();
        double sumBox = 0, sumTile = 0, sumPx = 0, maxBox = 0, maxTile = 0;
        int still = 0;
        const double area = (double) sw * sh;
        for (int f = 0; f < damage; f++) {
            ctx.overrideFloat(rccore::RemoteContext::ID_ANIMATION_TIME, t += stepMs / 1000.0f);
            if (timeMs >= 0) doc.setFixedTimeMs(timeMs + (int64_t) (f + 1) * stepMs);
            for (size_t k = 0; k < varies.size(); k++) {
                auto& v = varies[k];
                std::string val;
                if (v.count <= 0) {
                    val = std::to_string(v.lo + (v.hi - v.lo) * wave(f, (int) k));
                } else {
                    for (int j = 0; j < v.count; j++) {
                        if (j) val += ',';
                        val += std::to_string(v.lo + (v.hi - v.lo) * wave(f, (int) k * 31 + j));
                    }
                }
                setNamed(v.name, val);
            }
            static const bool single = getenv("RC_SINGLE_BUFFER") != nullptr;   // debug
            std::vector<uint32_t>& buf = (f & 1) || single ? px : alt;
            pc.retarget(buf.data());
            frame();
            kept += pc.keptCount();
            made += pc.madeCount();
            {
                const auto& d = pc.drawn();
                double a = 0;
                for (size_t i = 0; i + 3 < d.size(); i += 4) a += (double) d[i + 2] * d[i + 3];
                sumDrawn += 100.0 * a / ((double) sw * sh);
                for (size_t i = 0; i + 3 < d.size(); i += 4)
                    for (int y = d[i + 1]; y < d[i + 1] + d[i + 3]; y++)
                        std::memcpy(&panel[(size_t) y * sw + d[i]], &buf[(size_t) y * sw + d[i]], (size_t) d[i + 2] * 4);
                long n = 0;
                int bx0 = sw, by0 = sh, bx1 = -1, by1 = -1;
                for (int y = 0; y < sh; y++)
                    for (int x = 0; x < sw; x++)
                        if (panel[(size_t) y * sw + x] != buf[(size_t) y * sw + x]) {
                            n++;
                            bx0 = std::min(bx0, x), bx1 = std::max(bx1, x), by0 = std::min(by0, y), by1 = std::max(by1, y);
                        }
                if (n) {
                    if (getenv("RC_DAMAGE_VERBOSE") || staleFrames < 3)
                        std::printf("  frame %3d: the panel would be stale in %ld px (%d,%d-%d,%d)\n", f, n, bx0, by0, bx1, by1);
                    stale += n;
                    staleFrames++;
                    panel = buf;   // as if resent: count each frame's own
                }
                if (getenv("RC_DAMAGE_VERBOSE")) {
                    std::printf("  frame %3d: kept %d made %d, drawn", f, pc.keptCount(), pc.madeCount());
                    for (size_t i = 0; i + 3 < d.size() && i < 4 * 12; i += 4) std::printf(" %d,%d %dx%d", d[i], d[i + 1], d[i + 2], d[i + 3]);
                    std::printf("%s\n", d.size() > 48 ? " ..." : "");
                }
            }
            if (crc) std::printf("  crc %3d %08lx\n", f, crc32(0, reinterpret_cast<const Bytef*>(buf.data()), (uInt) (buf.size() * 4)));
            int x0 = sw, y0 = sh, x1 = -1, y1 = -1;
            long changed = 0;
            std::vector<uint8_t> tiles((size_t) tw * th, 0);
            for (int y = 0; y < sh; y++) {
                const uint32_t* a = &buf[(size_t) y * sw];
                const uint32_t* b = &prev[(size_t) y * sw];
                for (int x = 0; x < sw; x++) {
                    if (a[x] == b[x]) continue;
                    changed++;
                    x0 = std::min(x0, x); x1 = std::max(x1, x);
                    y0 = std::min(y0, y); y1 = std::max(y1, y);
                    tiles[(size_t) (y / kTile) * tw + x / kTile] = 1;
                }
            }
            long nt = 0;
            for (auto b : tiles) nt += b;
            const double box = x1 < 0 ? 0 : 100.0 * (x1 - x0 + 1) * (y1 - y0 + 1) / area;
            const double tile = 100.0 * nt / ((double) tw * th);
            const double pix = 100.0 * changed / area;
            if (x1 < 0) still++;
            sumBox += box; sumTile += tile; sumPx += pix;
            maxBox = std::max(maxBox, box); maxTile = std::max(maxTile, tile);
            if (getenv("RC_DAMAGE_VERBOSE"))
                std::printf("  frame %3d: box %5.1f%% (%d,%d %dx%d)  tiles %5.1f%%  pixels %5.1f%%\n", f, box, x0, y0,
                            x1 < 0 ? 0 : x1 - x0 + 1, x1 < 0 ? 0 : y1 - y0 + 1, tile, pix);
            prev = buf;
        }
        pc.retarget(px.data());
        px = prev;   // the last frame, for the PNG
        std::printf("damage over %d frames: box %.1f%% (max %.1f%%), %dx%d tiles %.1f%% (max %.1f%%), pixels %.1f%%, %d frames unchanged; paints kept %.0f%%, drawn %.1f%%, panel stale in %d frames\n",
                    damage, sumBox / damage, maxBox, kTile, kTile, sumTile / damage, maxTile, sumPx / damage, still,
                    kept + made ? 100.0 * kept / (kept + made) : 0.0, sumDrawn / damage, staleFrames);
    }

    if (bench > 0) {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < bench; i++) {
            if (!benchVar.empty()) setNamed(benchVar, std::to_string(-30 + (i % 7)));   // as a level does
            frame();
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("bench: %.2f ms/frame (%d paints)\n", ms / bench, pc.paintCount());
    }
    if (!writePng(argv[2], px.data(), sw, sh)) { std::cerr << "write failed\n"; return 1; }
    std::printf("%s -> %s (%dx%d, %d paints)\n", argv[1], argv[2], sw, sh, pc.paintCount());
    tvg::Initializer::term();
    return 0;
}
