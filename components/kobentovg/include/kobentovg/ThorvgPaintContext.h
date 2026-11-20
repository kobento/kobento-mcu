// RemoteCompose PaintContext backed by ThorVG's software rasterizer.
//
// The Skia backend (players/cpp/lib/rcskia) is the semantic reference; this mirrors it
// with a renderer small enough for an ESP32-P4. ThorVG is retained-mode, so a frame is
// built as a scene graph and rasterized once at endFrame():
//
//   beginFrame()  -> drop last frame's paints
//   doc.paint()   -> every draw call appends a tvg::Paint carrying the full CTM
//   endFrame()    -> canvas draw + sync into the caller's ARGB8888 buffer
//
// Clips and layers map onto nested tvg::Scene containers that are pushed by
// clip*/saveLayer* and popped by matrixRestore/restoreLayer, so save/restore keeps
// Skia's semantics (a restore drops the clip and the layer together with the matrix).
#pragma once
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif
#include <cstdint>
#include <cstdlib>
#include <memory>

#include "rccore/PaintContext.h"
#include "rccore/PaintBundle.h"
#include "rccore/d3/SoftwarePaint3DContext.h"
#include "rccore/d3/Rasterizer.h"
#include "rccore/operations/Mesh2D.h"

#include <thorvg.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace kobentovg {

class ThorvgPaintContext : public rccore::PaintContext {
public:
    // Bitmap type / encoding constants (BitmapData.java)
    static constexpr int TYPE_PNG_8888 = 0;
    static constexpr int TYPE_PNG = 1;
    static constexpr int TYPE_RAW8 = 2;
    static constexpr int TYPE_RAW8888 = 3;
    static constexpr int TYPE_PNG_ALPHA_8 = 4;
    static constexpr int ENCODING_INLINE = 0;
    static constexpr int ENCODING_EMPTY = 3;

    // Font families this backend resolves typefaces to. Register the data once, before
    // any document paints, with registerFont(). Without fonts, text is skipped.
    enum class Face { Sans, SansBold, Mono };
    static bool registerFont(Face face, const uint8_t* data, size_t size);

    // `pixels` is a premultiplied ARGB8888 buffer of w*h pixels, owned by the caller.
    ThorvgPaintContext(rccore::RemoteContext& context, uint32_t* pixels, int w, int h);
    ~ThorvgPaintContext() override;

    // Frame lifecycle (call around CoreDocument::paint).
    // `base` maps document space to the buffer (fit scale + offset); `clearArgb` fills
    // the buffer first.
    void beginFrame(const tvg::Matrix& base, uint32_t clearArgb);
    // Render the next frames into a different buffer of the same size (double buffering).
    void retarget(uint32_t* pixels);
    void endFrame();
    // Between the document's paint and endFrame(): a paint drawn on top of everything, in
    // target pixels (no document transform or clip). The canvas takes ownership. For host UI
    // such as status banners.
    void addOverlay(tvg::Paint* p);

    // Number of tvg paints the last frame produced; a cheap complexity gauge.
    int paintCount() const { return mPaintCount; }

    // Kept paints: a paint the frame draws as the frame before did (same shape, fill, stroke,
    // transform, clip; for text and pictures, the same content) stays in the canvas from that
    // frame, and ThorVG doesn't prepare it again (outlines, strokes: most of a frame's raster
    // time on the ESP32-S3). On by default (host: RC_NO_RETAIN=1 turns it off); off, every
    // frame is built anew, as before. kept/made: the last frame's paints reused and built.
    void setRetain(bool on);
    bool retaining() const { return mRetain; }
    // With kept paints, a frame rasters only where they changed (ThorVG's partial rendering), and
    // retarget() brings the other buffer up to date first (copying what the last frame changed).
    // drawn(): the regions the last frame rastered, x, y, w, h (the whole frame when it drew all).
    // redrawAll(): the buffer's pixels were changed behind the context's back: draw all next.
    const std::vector<int32_t>& drawn() const { return mDrawn; }
    void redrawAll();
    int keptCount() const { return mKept; }
    int madeCount() const { return mMade; }

    // Profiling, between the document's paint and endFrame() (rcstage::profilePaints): the
    // paints at the canvas's top level, what one is (type, bounds, fill), and a raster of that
    // one alone, the others removed (the frame is spent then: begin another).
    // Top-level paint i, or child j of it when it is a scene (j -1: the paint itself).
    int topCount() const;
    int childCount(int i) const;   // 0 unless a scene
    std::string describe(int i, int j = -1) const;
    void rasterOnly(int i, int j = -1);

#if !defined(ESP_PLATFORM) || CONFIG_KOBENTOVG_3D
    rccore::Paint3D* asPaint3D() override { return &m3D; }
#else
    rccore::Paint3D* asPaint3D() override { return nullptr; }   // 3D off (CONFIG_KOBENTOVG_3D)
#endif

    // ── Drawing
    void drawRect(float l, float t, float r, float b) override;
    void drawCircle(float cx, float cy, float radius) override;
    void drawLine(float x1, float y1, float x2, float y2) override;
    void drawOval(float l, float t, float r, float b) override;
    void drawArc(float l, float t, float r, float b, float start, float sweep) override;
    void drawRoundRect(float l, float t, float r, float b, float rx, float ry) override;
    void drawSector(float l, float t, float r, float b, float start, float sweep) override;
    void drawPath(int pathId, float start, float end) override;
    void drawTweenPath(int path1Id, int path2Id, float tween, float start, float end) override;
    void tweenPath(int outId, int pathId1, int pathId2, float tween) override;
    void drawBitmap(int imageId, float l, float t, float r, float b) override;
    void drawBitmapInt(int imageId, int srcL, int srcT, int srcR, int srcB,
                       int dstL, int dstT, int dstR, int dstB, int cdId) override;
    void drawTextRun(int textId, int start, int end, int contextStart, int contextEnd,
                     float x, float y, bool rtl) override;
    void drawTextAnchored(int textId, float x, float y, float panX, float panY,
                          int flags) override;
    void drawTextOnPath(int textId, int pathId, float hOffset, float vOffset) override;

    // ── 2D vertex meshes
    void setMesh(int meshId, int layout, int uCount, int vCount,
                 const std::vector<float>& verts, const std::vector<float>& uv,
                 const std::vector<int32_t>& colors,
                 const std::vector<int32_t>& indices) override;
    void drawMesh(int meshId, int blend, int imageId) override;
    void matrixFromMesh(int meshId, float u, float v, int flags) override;

    // ── Paint
    void applyPaint(const rccore::PaintBundle& bundle) override;
    void savePaint() override;
    void restorePaint() override;

    // ── Transform / layers / clip
    void matrixSave() override;
    void matrixRestore() override;
    void matrixScale(float sx, float sy, float cx, float cy) override;
    void matrixTranslate(float dx, float dy) override;
    void matrixRotate(float degrees, float cx, float cy) override;
    void matrixSkew(float sx, float sy) override;
    void saveLayerAlpha(float alpha, float l, float t, float r, float b) override;
    void saveLayerWithBlur(float sx, float sy, float l, float t, float r, float b) override;
    void restoreLayer() override;
    void clipRect(float l, float t, float r, float b) override;
    void clipPath(int pathId, int regionOp) override;
    void roundedClipRect(float w, float h, float topStart, float topEnd,
                         float bottomStart, float bottomEnd) override;

    // ── Data
    void loadText(int id, const std::string& text) override;
    std::string getText(int id) override;
    void loadBitmap(int imageId, int widthAndType, int heightAndEncoding,
                    const std::vector<uint8_t>& data) override;
    // A bitmap the host draws into (a document's empty, exported bitmap): these pixels from now
    // on, w x h premultiplied ARGB8888, any size (the document scales it where it draws it).
    // Taken, not copied. Between frames only: ThorVG reads the pixels while it renders.
    bool setImagePixels(int imageId, int w, int h, std::vector<uint32_t>&& px);
    void loadPathData(int instanceId, int winding, const std::vector<float>& path) override;
    void appendPathData(int instanceId, const std::vector<float>& path) override;
    void beginDrawToBitmap(int bitmapId, int mode, int color) override;
    void endDrawToBitmap() override;

    // ── Text measurement
    float measureTextWidth(const std::string& text, float fontSize) override;
    float measureTextHeight(const std::string& text, float fontSize) override;
    float measureTextAscent(const std::string& text, float fontSize) override;
    void getTextBounds(int textId, int start, int end, int flags, float bounds[4]) override;

    void reset() override;

private:
    struct Gradient {
        int type = -1;                    // PaintBundle::*_GRADIENT, -1 = none
        std::vector<tvg::Fill::ColorStop> stops;
        float p[4] = {0, 0, 0, 0};        // linear: x1 y1 x2 y2 / radial: cx cy r
        tvg::FillSpread spread = tvg::FillSpread::Pad;
        bool hasMatrix = false;
        tvg::Matrix matrix{};
    };
    struct PaintState {
        uint32_t color = 0xFF000000;      // ARGB, not premultiplied
        int style = rccore::PaintBundle::STYLE_FILL;
        float strokeWidth = 0, miter = 4;
        tvg::StrokeCap cap = tvg::StrokeCap::Butt;
        tvg::StrokeJoin join = tvg::StrokeJoin::Miter;
        std::vector<float> dash;
        float dashPhase = 0;
        tvg::BlendMethod blend = tvg::BlendMethod::Normal;
        Gradient gradient;
        float textSize = 14;
        int weight = 400;
        bool italic = false;
        int fontType = 0;
        // A blend colour filter, as Skia's SkColorFilters::Blend: what the paint draws (a
        // colour, gradient stops, bitmap pixels) is the destination, colorFilter the source.
        bool hasColorFilter = false;
        uint32_t colorFilter = 0;
        int colorFilterMode = rccore::PaintBundle::BLEND_MODE_SRC_IN;
        // Bitmap sampling: FILTER_BITMAP / IMAGE_FILTER_QUALITY == 0 selects nearest-neighbour;
        // otherwise ThorVG's default bilinear. Matches Android Paint.setFilterBitmap.
        bool nearestFilter = false;
    };
    struct SaveEntry {
        tvg::Matrix matrix;
        size_t containerDepth;
    };
    struct PathData {
        std::vector<float> floats;
        int winding = 0;
    };
    struct Image {
        int w = 0, h = 0;
        std::vector<uint32_t> px;         // premultiplied ARGB8888
        uint32_t version = 0;             // changes whenever px does
    };
    // Bitmaps drawn through a colour filter: one filtered copy per image and filter, rebuilt
    // when the image changes. They outlive the frame (ThorVG reads pixels at render time).
    struct Filtered {
        const Image* src;
        uint32_t version, color;
        int mode;
        Image img;
    };
    std::vector<Filtered> mFiltered;
    uint32_t mImageVersion = 0;
    Image& filtered(Image& img);
    uint32_t paintColor() const;          // the paint's colour, through its colour filter

    // Scene-graph plumbing
    // add to current container, CTM applied. `content`: what the paint shows that its getters
    // don't (a text's string and font, a picture's pixels); 0 for a text or picture: never kept.
    void push(tvg::Paint* p, uint64_t content = 0);
    size_t mPushes = 0;                               // paints and containers pushed this frame
    void pushContainer(tvg::Scene* scene, uint64_t content = 0);   // nest: later paints go inside
    tvg::Shape* styled(tvg::Shape* s, bool forceStroke = false);
    tvg::Fill* makeFill() const;
    void drawShape(tvg::Shape* s, bool forceStroke = false, uint64_t content = 0);

    // Geometry helpers
    bool buildPath(tvg::Shape* s, int pathId);
    void buildPathFromFloats(tvg::Shape* s, const std::vector<float>& data);
    static void appendArc(tvg::Shape* s, float l, float t, float r, float b,
                          float startDeg, float sweepDeg, bool moveFirst);

    // Text helpers
    const char* fontName() const;
    tvg::Text* measurer(const char* font, float size);
    float advanceOf(const std::string& utf8, float size);
    void textMetrics(float size, float& ascent, float& descent);
    void drawText(const std::string& utf8, float x, float baselineY);

    // Bitmaps
    Image* imageFor(int id);
    void drawImage(Image& img, float dl, float dt, float dw, float dh);

    // 2D vertex meshes. The engine expands the geometry (AddMesh2D); this keeps the
    // user-space result so drawMesh can rasterize it and matrixFromMesh can sample it.
    struct Mesh2DEntry {
        int layout = 0, uCount = 0, vCount = 0;
        std::vector<float> verts;         // user-space x,y interleaved
        std::vector<float> uv;            // normalised 0..1, (0,0) top-left; empty = none
        std::vector<int32_t> colors;      // per-vertex ARGB; empty = use the paint colour
        std::vector<int32_t> indices;     // triangle list
    };
    std::unordered_map<int, Mesh2DEntry> mMeshes2D;
    // A rasterized mesh is pushed as a Picture that reads these pixels at render time, so each
    // buffer must outlive the frame; they are device-space ARGB8888S, freed at the next
    // beginFrame.
    std::vector<std::vector<uint32_t>> mMeshBuffers;
    void drawGlyphTransformed(const std::string& glyph, const tvg::Matrix& boxToWorld);

    uint32_t* mPixels;
    int mW, mH;
    std::unique_ptr<tvg::SwCanvas> mCanvas;
    std::vector<tvg::Scene*> mContainers;   // stack; empty = root canvas
    tvg::Matrix mMatrix;
    std::vector<SaveEntry> mSaves;
    PaintState mPaint;
    std::vector<PaintState> mPaintStack;
    int mPaintCount = 0;
    bool mClearOpaque = false;
    tvg::Paint* mClear = nullptr;   // this frame's clear rect (the canvas owns it)

    // The kept paints (setRetain): the canvas's tree as nodes, each with its key (keyOf). A
    // frame matches what it draws against the last frame's nodes of the same container (a
    // Level), in order with a little lookahead: equal, the old paint stays where it is and the
    // new one goes; else the new one goes in before the next old one. Old ones passed over, or
    // left at the end, are removed.
    struct Node {
        tvg::Paint* p;
        uint64_t key;              // 0: never matches
        std::vector<Node> kids;    // a scene's
    };
    struct Level {
        tvg::Scene* scene;         // nullptr: the canvas
        std::vector<Node> old;     // last frame's
        size_t cursor = 0;         // the next old one to match
        std::vector<Node> cur;     // this frame's
    };
    std::vector<Node> mRoot;       // the last frame's top level
    std::vector<Level> mLevels;    // during a frame: the canvas, then each open container
    bool mRetain = true;
    int mKept = 0, mMade = 0;
    std::vector<int32_t> mDrawn;   // the last frame's drawn regions (drawn())
    uint8_t mClearAlpha = 255;     // the clear rect's own alpha (hidden while the document covers it)
    void removeNode(tvg::Scene* parent, tvg::Paint* p);   // its area marked to redraw, then removed
    uint64_t keyOf(tvg::Paint* p, uint64_t content) const;
    tvg::Paint* place(tvg::Paint* p, uint64_t key, bool container);   // the paint now in the tree
    void closeLevel();
    void closeLevelsTo(size_t depth);   // open containers: `depth`
    void dropRetained();                // forget the tree, empty the canvas
    bool documentCoversFrame() const;

    std::unordered_map<int, std::string> mTexts;
    std::unordered_map<int, PathData> mPaths;
    std::unordered_map<int, Image> mImages;
    std::unordered_map<int, size_t> mImageSourceHash;
    std::unordered_map<uint64_t, float> mWidthCache;
    // Text drawn once to a bitmap and blitted after (drawText): a line's glyph outlines cost
    // 1-4 ms to build and raster on the ESP32-S3, its picture a fraction of that. Keyed by the
    // text, font, size on screen, colour and style; kept while used, up to kTextCacheBytes.
    struct TextImage {
        std::vector<uint32_t> px;   // premultiplied ARGB, w x h
        int w = 0, h = 0, pad = 0;  // pad: the margin round the text box (overhangs, italics)
        uint32_t used = 0;          // the last frame (mFrameNo) it was drawn in
    };
    std::unordered_map<uint64_t, TextImage> mTextCache;
    std::unordered_map<uint64_t, uint32_t> mTextSeen;   // the frame a text was last drawn uncached
    std::unique_ptr<tvg::SwCanvas> mTextCanvas;          // draws a TextImage, retargeted for each
    size_t mTextCacheBytes = 0;
    uint32_t mFrameNo = 0;
    bool drawCachedText(const std::string& utf8, const char* font, float x, float boxTop, float size,
                        float ascent, float descent, uint32_t color);
    tvg::Text* mMeasure = nullptr;
    const char* mMeasureFont = nullptr;
    float mMeasureSize = -1;

    // DrawToBitmap: an offscreen canvas rendered immediately at endDrawToBitmap.
    struct Offscreen;
    std::unique_ptr<Offscreen> mOffscreen;

    // 3D: rccore's software rasterizer renders into its own ARGB buffer, which is blitted
    // into the scene as a picture after each mesh, exactly as the Skia backend does.
    class Tvg3D : public rccore::Paint3D {
    public:
        explicit Tvg3D(ThorvgPaintContext& o) : mOwner(o) {}
        void defineMesh3D(int id, const std::vector<int32_t>& indices,
                          const std::vector<float>& verts, const std::vector<float>& normals,
                          const std::vector<float>& uv) override;
        void setCamera3D(int projection, const std::vector<float>& projParams,
                         const std::vector<float>& viewParams) override;
        void matrix3Op(int sub, const std::vector<float>& args) override;
        void drawMesh3D(int meshId, int mode) override;
        void clearDepth3D() override;
        void setLights3D(const std::vector<int>& types, const std::vector<int32_t>& colors,
                         const std::vector<float>& params) override;
        void setTexture3D(int bitmapId) override;
        void setMaterial3D(float specStrength, float shininess) override;
        void setDepthBias3D(float constant, float slope) override;
    private:
        rccore::d3::SoftwarePaint3DContext* ensure();
        void blit();
        ThorvgPaintContext& mOwner;
        rccore::d3::SoftwarePaint3DContext mCtx;
        bool mSized = false;
        struct FreeDeleter { void operator()(uint32_t* p) const { std::free(p); } };
        // Snapshots of the 3D color buffer, kept alive until the frame renders. Consecutive
        // meshes with nothing 2D pushed in between share one (see blit()).
        std::vector<std::unique_ptr<uint32_t[], FreeDeleter>> mBlits;
        size_t mLastBlitPush = SIZE_MAX;   // mOwner.mPushes when the last snapshot was pushed
        tvg::Matrix mLastBlitMatrix{};
    };
    Tvg3D m3D{*this};
};

}  // namespace kobentovg
