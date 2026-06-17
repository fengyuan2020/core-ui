// gh_img_view.cpp — 通用瓦块画布 widget 实现。
//
// 渲染思路：
//   1. ComputeDestRect 算出"图像在屏幕上占多大"
//   2. preview_ 存在则先 StretchBlt 一张兜底（NEAREST）
//   3. 遍历可见瓦块（active level 网格），有 bitmap 就画上去
//   4. 缺的瓦块就让 preview 透出来 → 渐进式 LOD 体验
//
// auto-level：zoom 跨越某 level 阈值时自动切 active level，但**不**主动清旧
// 数据 —— 由调用方决定何时清（Begin / ClearLevel / Clear）。这样切级时不会
// 黑屏 (旧 level 瓦块还在 + preview 仍兜底)。

#include "gh_img_view.h"
#include "event.h"

#include <lunasvg.h>     /* L173 / core-ui Phase 4: 统一 SVG 引擎 (替 D2D
                          * ID2D1SvgDocument; 原生支持 CSS / filter / text /
                          * mask, 不再需要 svg_style_inliner 与 text→path 补丁) */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>
#include <Windows.h>

namespace ui {

namespace {

// CW rotation of (x, y) by angle (must be 0/90/180/270) around origin.
// Screen y-axis is down, so 90° CW maps (1,0) → (0,1).
inline void RotateCW(int angle, float x, float y, float& rx, float& ry) {
    switch (angle) {
        case 90:  rx = -y; ry =  x; break;
        case 180: rx = -x; ry = -y; break;
        case 270: rx =  y; ry = -x; break;
        default:  rx =  x; ry =  y; break;
    }
}

// Inverse of RotateCW (= CCW by same angle).
inline void RotateCCW(int angle, float x, float y, float& rx, float& ry) {
    switch (angle) {
        case 90:  rx =  y; ry = -x; break;
        case 180: rx = -x; ry = -y; break;
        case 270: rx = -y; ry =  x; break;
        default:  rx =  x; ry =  y; break;
    }
}

// Pick D2D interpolation based on draw scale (screen px per source px).
// HQ_CUBIC 4×4 邻域 + negative-lobe lobe 在 ≥2× 下采时采样窗不够 → 高对比
// 边缘会 ring 出色边 (灰银/黑 → 白 转换看到的蓝紫边即此). LINEAR 是
// 2×2 bilinear, 无负权, 永不 ring; 高频细节本来在大幅下采时也保不住, 视觉
// 损失可忽略. 阈值 0.5 跟 D2D 文档"HQ_CUBIC 推荐 0.5-2× 区间"对齐.
// (D2D 没有 HIGH_QUALITY_LINEAR 这种常量, LINEAR 即标准 bilinear.)
inline D2D1_INTERPOLATION_MODE PickInterp(float screen_per_source) {
    return screen_per_source < 0.5f
        ? D2D1_INTERPOLATION_MODE_LINEAR
        : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
}

inline int NormalizeAngle(int a) {
    // 任意 int → 最近 90° 的 0/90/180/270.
    int m = ((a % 360) + 360) % 360;
    // round to nearest 90, then mod 360.
    return ((m + 45) / 90 * 90) % 360;
}

} // namespace

GhImgViewWidget::GhImgViewWidget() {
    // bgColor 沿用基类默认 {0,0,0,0} = 透明，跟其它 core-ui widget 一致。
    // 宿主需要自定义底色，在 .uix 里写
    //     gh_img_view { background: <color> | <gradient>; }
    // CSS 的 background 不会自动从父容器继承（CSS 标准行为），但 widget 默认
    // 透明意味着父容器的 background（含 linear-gradient 棋盘格）会透出来 ——
    // 这是看图软件常见的浅灰/棋盘格画布外观需要的 baseline。
    cssTag  = "gh_img_view";
    focusable = true;
}

// L173 Phase 4: 后台 SVG 栅格化结果槽。widget 与后台渲染线程经 shared_ptr 共享,
// 线程只碰这个 slot + 自己持有的 Document/HWND (不碰 widget), 故 widget 析构 /
// 换图都不悬空; slot 在双方都释放后销毁。
struct SvgRenderSlot {
    std::mutex            mu;            // 保护 bgra / w / h / ready
    std::vector<uint8_t>  bgra;          // 渲好的紧凑 BGRA premul (w*h*4)
    uint32_t              w = 0, h = 0;
    bool                  ready = false;
    std::atomic<bool>     inFlight{false};    // 是否有后台线程在渲
    std::atomic<uint32_t> wantW{0}, wantH{0}; // 最新期望尺寸 (缩放变了 → 渲完再渲最新)
};

GhImgViewWidget::~GhImgViewWidget() = default;

// ===== 数据形状 =====

void GhImgViewWidget::Begin(const Info& info, Renderer& r) {
    tiles_.clear();
    /* L168: keepPreview 时不清 preview 兜底层 — preview 在 OnDraw 永远先画兜底,
     * tile 逐级盖上, 清晰度物理单调, 消除切金字塔时的闪烁。 */
    if (!info.keepPreview) {
        preview_.Reset();
        previewW_ = previewH_ = 0;
    }
    /* 切瓦块 source 前清 SVG 状态. svgSlot_ 置空 = 放弃任何在飞后台渲染 (其线程
     * 写到的是旧 slot, 自然丢弃)。 */
    svgDoc_.reset();
    svgRaster_.Reset();
    svgRasterW_ = svgRasterH_ = 0;
    svgW_ = svgH_ = 0;
    svgSlot_.reset();

    /* Cache DPI scale for PickAutoLevel — 算物理像素密度需要 zoom 乘上
     * dpi_scale, 不然 DPI>100% 时按 DIP 算阈值会偏向上采样 level → fit
     * 大图模糊. 用户实测原话 "默认糊, 放大缩小后清晰" 就是这条 — wheel
     * cycle 时 widget 切到高分辨率 mip, cached tile 留在 tiles_ 被 OnDraw
     * 覆盖渲染, 等效用了下采样 level. 修了 PickAutoLevel 后默认就走下
     * 采样 level, 不需要 wheel 操作. */
    if (auto* ctx = r.RT()) {
        float dx = 96.0f, dy = 96.0f;
        ctx->GetDpi(&dx, &dy);
        if (dx > 1e-3f) dpi_scale_ = dx / 96.0f;
    }

    info_ = info;
    if (info_.tileSize == 0) info_.tileSize = 256;
    if (info_.levels   == 0) info_.levels   = 1;
    activeLevel_ = autoLevel_ ? PickAutoLevel() : 0;

    // 默认 fit 到视口（如果已 layout）. 新图片默认 rotation=0 — 看图器
    // 翻图时不继承上一张的旋转, 跟 Win11 照片 / IrfanView 行为一致.
    rotation_ = 0;
    if (rect.right > rect.left && rect.bottom > rect.top) {
        Fit();
    } else {
        zoom_ = 1.0f; panX_ = 0; panY_ = 0;
    }
    NotifyViewport();
    InvalidateAllWindows();
}

void GhImgViewWidget::SetPreview(const void* bgra, uint32_t pw, uint32_t ph,
                                  uint32_t stride, Renderer& r) {
    if (!bgra || pw == 0 || ph == 0) return;
    if (stride == 0) stride = pw * 4;
    /* gh_img_view 的输入约定: caller 喂 straight (non-premultiplied) BGRA —
       绝大多数 PNG/JPEG 解码器 (ghde / WIC GUID_WICPixelFormat32bppBGRA /
       libpng / stb_image) 默认输出 straight. lib 内部 premul 转成 D2D
       PREMULTIPLIED bitmap 期望的格式 — 抗锯齿边缘 (alpha 中间值) 大倍率
       放大时不会出 chroma fringe. */
    auto bmp = r.CreateBitmapFromPixelsStraight(bgra, (int)pw, (int)ph, (int)stride);
    if (!bmp) return;
    preview_  = std::move(bmp);
    previewW_ = pw;
    previewH_ = ph;
    InvalidateAllWindows();
}

void GhImgViewWidget::SetTile(uint32_t level, uint32_t tx, uint32_t ty,
                               const void* bgra, uint32_t tw, uint32_t th,
                               uint32_t stride, Renderer& r) {
    if (!bgra || tw == 0 || th == 0) return;
    if (level >= info_.levels) return;
    if (stride == 0) stride = tw * 4;

    /* 同 SetPreview — caller 喂 straight BGRA, lib 内部 premul. */
    auto bmp = r.CreateBitmapFromPixelsStraight(bgra, (int)tw, (int)th, (int)stride);
    if (!bmp) return;

    // L48: 不再做 LRU evict — viewport trim 在 NotifyViewport 内做, 跟 viewport
    // 边界严格绑定. SetTile 只负责装新 tile, 不主动 evict.
    Tile t;
    t.bmp = std::move(bmp);
    t.w   = tw;
    t.h   = th;
    tiles_[TileKey{level, tx, ty}] = std::move(t);
    if (!tileBatch_) InvalidateAllWindows();   // L115: batch 内不刷, EndTileBatch 一次刷
}

void GhImgViewWidget::EndTileBatch() {
    tileBatch_ = false;
    InvalidateAllWindows();
}

void GhImgViewWidget::TrimToViewport_(uint32_t active_level,
                                       uint32_t visible_tx0, uint32_t visible_tx1,
                                       uint32_t visible_ty0, uint32_t visible_ty1) {
    // L48: 清掉 tiles_ 中 (1) 非 active_level 的全部 tile + (2) active_level
    // 内但不在 viewport [tx0,tx1) × [ty0,ty1) 范围的 tile. 每个被清的 tile
    // fire onTileEvicted, caller 同步自己端 pushed_tiles_ erase.
    //
    // 内存稳态 ≈ viewport 内 tile 数 × 256KB, 不随 zoom 历史累积. zoom out /
    // pan 远 → trim → caller pushed 同步清 → 下次 viewport callback 重新 enqueue
    // worker decode (单 tile 0.38ms × 4 worker 并发 ~10ms 不可感知).
    for (auto it = tiles_.begin(); it != tiles_.end(); ) {
        const TileKey& k = it->first;
        const bool in_viewport = (k.level == active_level &&
                                    k.tx >= visible_tx0 && k.tx < visible_tx1 &&
                                    k.ty >= visible_ty0 && k.ty < visible_ty1);
        // L115: 保留上一个 active level 的 tile — OnDraw 多级 fallback 用它覆盖新级
        // 未到达的 tile, 切级清晰→更清晰无波浪 (旧级 tile 数约新级 1/4, 内存可忽略)。
        const bool keep_prev = (k.level == prevActiveLevel_);
        if (in_viewport || keep_prev) { ++it; continue; }
        if (onTileEvicted) {
            onTileEvicted(k.level, k.tx, k.ty);
        }
        it = tiles_.erase(it);
    }
}

void GhImgViewWidget::ClearLevel(uint32_t level) {
    for (auto it = tiles_.begin(); it != tiles_.end(); ) {
        if (it->first.level == level) {
            it = tiles_.erase(it);
        } else {
            ++it;
        }
    }
    InvalidateAllWindows();
}

void GhImgViewWidget::Clear() {
    tiles_.clear();
    preview_.Reset();
    previewW_ = previewH_ = 0;
    svgDoc_.reset();
    svgRaster_.Reset();
    svgRasterW_ = svgRasterH_ = 0;
    svgW_ = svgH_ = 0;
    svgSlot_.reset();
    info_ = Info{};
    activeLevel_ = 0;
    zoom_ = 1.0f; panX_ = 0; panY_ = 0;
    rotation_ = 0;
    InvalidateAllWindows();
}

// ---- SVG 矢量源 (Build 70+ L20) ----

bool GhImgViewWidget::SetSvgFromFile(const std::wstring& path, Renderer& r) {
    (void)r;   /* LunaSVG 纯 CPU 栅格化, 加载阶段不需要 D2D 渲染器 */

    /* L173 / core-ui Phase 4: 改用 LunaSVG。它原生解析 <style> CSS 选择器 / 渐变 /
     * clip / mask / <text> / filter, 不再需要 svg_style_inliner 预处理或
     * <text>→<path> 转换 (那些是 D2D ID2D1SvgDocument 不支持时的补丁)。
     * loadFromFile 走 UTF-8 路径, 这里把宽字符路径转 UTF-8。 */
    /* 用宽字符路径读文件再 loadFromData — Windows 的 fopen/loadFromFile 把窄路径
     * 按系统 ANSI 码页解释, 传 UTF-8 路径会打不开带中文的文件。MSVC 的 ifstream
     * 支持 wchar_t* 路径, 绕开码页问题 (跟 ghde gh_svg_decoder 同思路)。 */
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return false;
    std::string xml((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    if (xml.empty()) return false;

    auto doc = lunasvg::Document::loadFromData(xml);
    if (!doc) return false;

    /* natural size = LunaSVG intrinsic (viewBox 优先, 否则 width/height 属性)。 */
    uint32_t natW = static_cast<uint32_t>(doc->width()  + 0.5f);
    uint32_t natH = static_cast<uint32_t>(doc->height() + 0.5f);
    if (natW == 0) natW = 1024;
    if (natH == 0) natH = 1024;

    /* 进入 SVG 模式 — 清瓦块 state, 把 natural size 写进 info_ 让 Fit / zoom
     * 几何全部复用瓦块路径的代码. levels = 1 (SVG 不分级). */
    tiles_.clear();
    preview_.Reset();
    previewW_ = previewH_ = 0;
    svgRaster_.Reset();              /* 新文件 — 作废旧栅格缓存 */
    svgRasterW_ = svgRasterH_ = 0;
    svgSlot_ = std::make_shared<SvgRenderSlot>();   /* 新文件 → 新结果槽 */
    svgDoc_  = std::move(doc);       /* unique_ptr → shared_ptr */
    svgW_    = natW;
    svgH_    = natH;
    info_ = Info{};
    info_.fullWidth   = natW;
    info_.fullHeight  = natH;
    info_.tileSize    = 256;
    info_.levels      = 1;
    info_.pixelFormat = 0;
    activeLevel_ = 0;
    rotation_ = 0;
    if (rect.right > rect.left && rect.bottom > rect.top) {
        Fit();
    } else {
        zoom_ = 1.0f; panX_ = 0; panY_ = 0;
    }
    NotifyViewport();
    InvalidateAllWindows();
    return true;
}

int GhImgViewWidget::RenderSvgToBgra(uint32_t target_w, uint32_t target_h,
                                       uint8_t* out_bgra,
                                       uint32_t* out_w, uint32_t* out_h,
                                       Renderer& r) {
    if (!svgDoc_ || svgW_ == 0 || svgH_ == 0) return -1;
    if (!out_bgra || target_w == 0 || target_h == 0) return -2;
    (void)r;   /* LunaSVG 纯 CPU 栅格化, 不需要 D2D 渲染器 */

    /* fit 保 aspect, 长边贴到 target 边 */
    double src_aspect = (double)svgW_ / (double)svgH_;
    double dst_aspect = (double)target_w / (double)target_h;
    uint32_t out_w_v, out_h_v;
    if (src_aspect > dst_aspect) {
        out_w_v = target_w;
        out_h_v = (uint32_t)((double)target_w / src_aspect + 0.5);
    } else {
        out_h_v = target_h;
        out_w_v = (uint32_t)((double)target_h * src_aspect + 0.5);
    }
    if (out_w_v == 0) out_w_v = 1;
    if (out_h_v == 0) out_h_v = 1;

    /* LunaSVG 栅格化 (CPU)。premultiplied ARGB32 (BGRA 字节序) = caller 期望的
     * BGRA premul, 逐行拷出 (stride 可能 > w*4)。 */
    lunasvg::Bitmap bmp = svgDoc_->renderToBitmap((int)out_w_v, (int)out_h_v,
                                                  0x00000000u);
    if (bmp.isNull() || !bmp.data()) return -5;
    const uint8_t* src    = bmp.data();
    const int      stride = bmp.stride();
    const uint32_t bw     = (uint32_t)bmp.width();
    const uint32_t bh     = (uint32_t)bmp.height();
    const uint32_t cw     = (bw < out_w_v) ? bw : out_w_v;
    const uint32_t ch     = (bh < out_h_v) ? bh : out_h_v;
    memset(out_bgra, 0, (size_t)out_w_v * out_h_v * 4);
    for (uint32_t y = 0; y < ch; ++y) {
        memcpy(out_bgra + (size_t)y * out_w_v * 4,
               src + (size_t)y * stride,
               (size_t)cw * 4);
    }

    if (out_w) *out_w = out_w_v;
    if (out_h) *out_h = out_h_v;
    return 0;
}

// L173 / core-ui Phase 4: SVG 按当前显示分辨率重栅到 svgRaster_。
// 关键: 缓存分辨率必须贴近显示分辨率 — 放大过界 (cache < 显示, 会上采糊) 或缩小
// 过多 (cache > 显示×kReuseMax, 大倍数 downscale 直接缩会锯齿, D2D 无 mipmap 预
// 滤波) 都要重栅。重栅渲到 显示×kSuper (轻度超采样抗锯齿 + 给邻近缩放档复用余量)。
void GhImgViewWidget::EnsureSvgRaster(const D2D1_RECT_F& dest, Renderer& r) {
    if (!svgDoc_ || svgW_ == 0 || svgH_ == 0) return;
    const float drawW = dest.right - dest.left;
    const float drawH = dest.bottom - dest.top;
    if (drawW <= 0.f || drawH <= 0.f) return;

    /* 当前显示尺寸 (设备像素), 封顶兜内存 (保 aspect)。 */
    const uint32_t kCap = 4096;
    uint32_t needW = (uint32_t)std::ceil(drawW);
    uint32_t needH = (uint32_t)std::ceil(drawH);
    if (needW > kCap || needH > kCap) {
        const float s = (float)kCap / (float)(needW > needH ? needW : needH);
        needW = (uint32_t)(needW * s);
        needH = (uint32_t)(needH * s);
    }
    if (needW == 0) needW = 1;
    if (needH == 0) needH = 1;

    /* 复用窗口: 缓存 ∈ [显示, 显示×1.6]。下限保证不上采 (不糊), 上限把 downscale
     * 倍数限制在 1.6× 内 (D2D 高质三次插值在此范围内无锯齿; 再大直接缩就走样)。
     * 落窗内 → 复用; 否则 (放大过界 / 缩小过多) → 重栅到当前显示档。 */
    if (svgRaster_
        && svgRasterW_ >= needW && svgRasterW_ <= needW + needW * 6 / 10
        && svgRasterH_ >= needH && svgRasterH_ <= needH + needH * 6 / 10) {
        return;
    }

    /* 渲到 显示×1.3 (轻超采样: 渲得比显示大、缩回去 = supersample AA, 圆角更平滑;
     * 同时给邻近缩放档留复用余量, 减少重栅频率)。封顶 kCap。 */
    uint64_t renW = (uint64_t)needW * 13 / 10;
    uint64_t renH = (uint64_t)needH * 13 / 10;
    if (renW > kCap || renH > kCap) {
        const double s = (double)kCap / (double)(renW > renH ? renW : renH);
        renW = (uint64_t)(renW * s);
        renH = (uint64_t)(renH * s);
    }
    if (renW == 0) renW = 1;
    if (renH == 0) renH = 1;

    lunasvg::Bitmap bmp = svgDoc_->renderToBitmap((int)renW, (int)renH,
                                                  0x00000000u);
    if (bmp.isNull() || !bmp.data()) return;
    const uint32_t bw = (uint32_t)bmp.width();
    const uint32_t bh = (uint32_t)bmp.height();
    if (bw == 0 || bh == 0) return;

    /* LunaSVG premultiplied ARGB32 (BGRA 字节序) = D2D premultiplied BGRA 位图,
     * 直接拿数据建 D2D 位图 (CreateBitmap 拷贝数据, bmp 之后可析构)。 */
    auto* rt = r.RT();
    if (!rt) return;
    const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(rt->CreateBitmap(D2D1::SizeU(bw, bh), bmp.data(),
                                (UINT32)bmp.stride(), &props, &bitmap))) {
        return;
    }
    svgRaster_  = std::move(bitmap);
    svgRasterW_ = bw;
    svgRasterH_ = bh;
}

// 起 (或更新) 一个后台栅格化请求。非阻塞: 后台线程渲 LunaSVG → BGRA 存进 slot
// → InvalidateRect 唤醒 UI 线程 (下次 OnDraw 由 ConsumeSvgRasterResult_ 换上)。
// 线程只碰 slot + 自己持有的 doc/hwnd (shared_ptr 保活), 不碰 this → 析构/换图安全。
// 渲染期间又改缩放 (want 变) → 渲完再渲最新档 (合并到最后一次), inFlight 保证单线程。
void GhImgViewWidget::RequestSvgRasterAsync_(uint32_t renW, uint32_t renH,
                                             unsigned long uiThreadId) {
    auto slot = svgSlot_;
    auto doc  = svgDoc_;
    if (!slot || !doc || renW == 0 || renH == 0) return;
    slot->wantW.store(renW);
    slot->wantH.store(renH);
    bool expected = false;
    if (!slot->inFlight.compare_exchange_strong(expected, true))
        return;   /* 已有后台线程在渲, 它渲完会看 want 再渲最新 */

    std::thread([slot, doc, uiThreadId]() {
        for (;;) {
            const uint32_t w = slot->wantW.load();
            const uint32_t h = slot->wantH.load();
            lunasvg::Bitmap bmp = doc->renderToBitmap((int)w, (int)h, 0x00000000u);
            if (!bmp.isNull() && bmp.data()) {
                const uint32_t bw = static_cast<uint32_t>(bmp.width());
                const uint32_t bh = static_cast<uint32_t>(bmp.height());
                const int stride  = bmp.stride();
                const uint8_t* src = bmp.data();
                std::vector<uint8_t> buf(static_cast<size_t>(bw) * bh * 4);
                for (uint32_t y = 0; y < bh; ++y)
                    memcpy(buf.data() + static_cast<size_t>(y) * bw * 4,
                           src + static_cast<size_t>(y) * stride,
                           static_cast<size_t>(bw) * 4);
                {
                    std::lock_guard<std::mutex> lk(slot->mu);
                    slot->bgra.swap(buf);
                    slot->w = bw; slot->h = bh; slot->ready = true;
                }
                /* 跨线程唤醒 UI 重绘 (InvalidateRect 线程安全; 同
                 * InvalidateAllWindows 的按线程枚举窗口模式)。 */
                EnumThreadWindows(uiThreadId, [](HWND hwnd, LPARAM) -> BOOL {
                    wchar_t cls[64];
                    GetClassNameW(hwnd, cls, 64);
                    if (wcscmp(cls, L"UiCore_Window") == 0)
                        InvalidateRect(hwnd, nullptr, FALSE);
                    return TRUE;
                }, 0);
            }
            /* 期间又改了缩放 → 渲最新; 否则收工。 */
            if (slot->wantW.load() == w && slot->wantH.load() == h) {
                slot->inFlight.store(false);
                break;
            }
        }
    }).detach();
}

// UI 线程: 后台结果就绪则把 BGRA 建成 D2D 位图换上 svgRaster_ (D2D 单线程, 必须
// 在 UI 线程建)。
void GhImgViewWidget::ConsumeSvgRasterResult_(Renderer& r) {
    if (!svgSlot_) return;
    std::vector<uint8_t> buf;
    uint32_t bw = 0, bh = 0;
    {
        std::lock_guard<std::mutex> lk(svgSlot_->mu);
        if (!svgSlot_->ready) return;
        buf.swap(svgSlot_->bgra);
        bw = svgSlot_->w; bh = svgSlot_->h;
        svgSlot_->ready = false;
    }
    if (bw == 0 || bh == 0 || buf.size() < static_cast<size_t>(bw) * bh * 4) return;
    auto* rt = r.RT();
    if (!rt) return;
    const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(rt->CreateBitmap(D2D1::SizeU(bw, bh), buf.data(), bw * 4, &props, &bitmap)))
        return;
    svgRaster_  = std::move(bitmap);
    svgRasterW_ = bw;
    svgRasterH_ = bh;
}

void GhImgViewWidget::SetActiveLevel(uint32_t level) {
    if (level >= info_.levels) return;
    SwitchLevel(level);
    autoLevel_ = false;
    NotifyViewport();
    InvalidateAllWindows();
}

// ===== 视口 =====

void GhImgViewWidget::SetZoom(float z) {
    z = std::clamp(z, minZoom_, maxZoom_);
    if (zoom_ == z) return;
    zoom_ = z;
    if (autoLevel_) SwitchLevel(PickAutoLevel());
    ConstrainPan();
    NotifyViewport();
    InvalidateAllWindows();
}

/* L47 follow-up: 之前 SetPan 是 .h inline 单纯赋值 panX_/Y_, 没 fire
 * NotifyViewport — 跟 SetZoom 不对称. caller 调 ui_gh_img_view_set_pan
 * (典型场景 minimap click 切换显示区域) 后 viewport callback 不 fire,
 * caller 不知道要 push_visible_tiles_ 给新可见区, OnDraw fallback 显
 * preview / 老 level → 用户看到模糊, 要点击画布触发 OnMouseMove 才
 * fire callback 才清晰. 修: 跟 SetZoom 同款 fire NotifyViewport +
 * InvalidateAllWindows. ConstrainPan 也跟 SetZoom 路径对齐.
 *
 * 早 return 优化: (x, y) 等于当前 pan 时跳过 — caller 端可能反复调
 * (例如 minimap drag 时连续 set_pan), 避免无效 callback. */
void GhImgViewWidget::SetPan(float x, float y) {
    if (panX_ == x && panY_ == y) return;
    panX_ = x;
    panY_ = y;
    ConstrainPan();
    NotifyViewport();
    InvalidateAllWindows();
}

void GhImgViewWidget::SetZoomAround(float z, float anchorX, float anchorY) {
    z = std::clamp(z, minZoom_, maxZoom_);
    if (zoom_ == z) return;

    // 1) 当前 state 下 anchor 屏幕点对应的图像坐标 (顶级, rotation-aware)
    float ix = 0, iy = 0;
    ScreenToImage(anchorX, anchorY, ix, iy);

    // 2) 切新 zoom
    zoom_ = z;
    if (autoLevel_) SwitchLevel(PickAutoLevel());

    // 3) 反算 pan 使 (ix, iy) 仍落在 (anchorX, anchorY)
    //
    // Forward image (ix, iy) → screen (sx, sy):
    //   fx = (ix - fullW/2) * zoom_new
    //   fy = (iy - fullH/2) * zoom_new
    //   (rx, ry) = RotateCW(rotation_, fx, fy)
    //   sx = widget_cx + panX + rx
    //   sy = widget_cy + panY + ry
    //
    // 要求 sx = anchorX, sy = anchorY → panX/Y = anchor - widget_center - (rx/ry).
    float fullW = (float)info_.fullWidth;
    float fullH = (float)info_.fullHeight;
    float fx = (ix - fullW * 0.5f) * zoom_;
    float fy = (iy - fullH * 0.5f) * zoom_;
    float rx, ry;
    RotateCW(rotation_, fx, fy, rx, ry);
    float cx = (rect.left + rect.right) * 0.5f;
    float cy = (rect.top  + rect.bottom) * 0.5f;
    panX_ = anchorX - cx - rx;
    panY_ = anchorY - cy - ry;

    ConstrainPan();
    NotifyViewport();
    InvalidateAllWindows();
}

void GhImgViewWidget::Fit() {
    if (info_.fullWidth == 0 || info_.fullHeight == 0) return;
    float w = std::max(1.0f, rect.right - rect.left);
    float h = std::max(1.0f, rect.bottom - rect.top);
    // rotation-aware: 90/270 时图像视觉宽高互换, fit 用 effective 尺寸.
    float effW = (float)EffectiveImageWidth();
    float effH = (float)EffectiveImageHeight();
    float sx = w / effW;
    float sy = h / effH;
    zoom_ = std::min(sx, sy);
    zoom_ = std::clamp(zoom_, minZoom_, maxZoom_);
    panX_ = 0; panY_ = 0;
    if (autoLevel_) SwitchLevel(PickAutoLevel());
    NotifyViewport();
    InvalidateAllWindows();
}

void GhImgViewWidget::SetRotation(int angle) {
    int n = NormalizeAngle(angle);
    if (n == rotation_) return;
    rotation_ = n;
    // pan / zoom 都保留 (用户预期: 旋转不丢视图状态). pan 在屏幕空间, 跟
    // rotation 解耦, 不需要旋转 pan 向量 — 详见类注释.
    NotifyViewport();
    InvalidateAllWindows();
}

void GhImgViewWidget::Reset() {
    zoom_ = 1.0f;
    panX_ = 0; panY_ = 0;
    if (autoLevel_) SwitchLevel(PickAutoLevel());
    NotifyViewport();
    InvalidateAllWindows();
}

// ===== Widget 虚函数 =====

void GhImgViewWidget::OnDraw(Renderer& r) {
    // 画布底色 (旋转不影响, 永远填满 widget rect)
    r.FillRect(rect, bgColor);

    if (info_.fullWidth == 0 || info_.fullHeight == 0) return;

    // L48 followup: rect 变化 (典型 window resize 让 parent layout 重 size
     // widget) 自动 fire NotifyViewport, caller 不需要在 resize handler 里
     // 手动 invalidate viewport. visible tile 范围 = f(rect, zoom, pan), rect
     // 变 → visible 变 → 需要 push 新 viewport tile.
    if (rect.left   != lastNotifiedRect_.left   ||
        rect.top    != lastNotifiedRect_.top    ||
        rect.right  != lastNotifiedRect_.right  ||
        rect.bottom != lastNotifiedRect_.bottom) {
        NotifyViewport();   // 内部 set lastNotifiedRect_ = rect 防重 fire
    }

    // 视觉 AABB (rotation 应用后的可见框) 做早期剔除. logical dest 给 tile
    // 算位置用 (preview/tile dest 在未旋转坐标系里, D2D transform 完成旋转).
    D2D1_RECT_F visual = ComputeVisualDestRect();
    if (visual.right <= rect.left || visual.left >= rect.right ||
        visual.bottom <= rect.top || visual.top  >= rect.bottom) {
        return;  // 旋转后仍完全在视口外
    }

    D2D1_RECT_F dest = ComputeDestRect();   // logical (pre-rotation) dest

    r.PushClip(rect);

    /* L173 / core-ui Phase 4: SVG 模式 — LunaSVG 按当前显示分辨率栅格化到
     * svgRaster_ 缓存位图, 走和普通图一样的 DrawBitmap 路径 (统一)。放大过界
     * 才重栅 (任意缩放档清晰), 平移用缓存零成本。rotation 跟瓦块路径同款: 绕
     * dest 中心的 transform + logical dest 内 DrawBitmap。 */
    if (svgDoc_) {
        /* 后台栅格化: UI 线程只画缓存位图, 永不阻塞输入。
         *  1) 先消费后台已渲好的结果 (若有) → 换上 svgRaster_。
         *  2) 算当前显示需要的分辨率 + 复用窗口 [显示, 显示×1.6]。落窗内 → 直接画。
         *  3) 出窗: 没缓存 (首帧) 同步渲一张 (load 一次性); 否则丢后台渲 (非阻塞),
         *     渲好前继续画旧缓存 (略软), 渲完 InvalidateRect → 下帧换上。 */
        ConsumeSvgRasterResult_(r);

        const float drawW = dest.right - dest.left;
        const float drawH = dest.bottom - dest.top;
        if (drawW > 0.f && drawH > 0.f) {
            const uint32_t kCap = 4096;
            uint32_t needW = (uint32_t)std::ceil(drawW);
            uint32_t needH = (uint32_t)std::ceil(drawH);
            if (needW > kCap || needH > kCap) {
                const float s = (float)kCap / (float)(needW > needH ? needW : needH);
                needW = (uint32_t)(needW * s);
                needH = (uint32_t)(needH * s);
            }
            if (needW == 0) needW = 1;
            if (needH == 0) needH = 1;
            const bool inWindow = svgRaster_
                && svgRasterW_ >= needW && svgRasterW_ <= needW + needW * 6 / 10
                && svgRasterH_ >= needH && svgRasterH_ <= needH + needH * 6 / 10;
            if (!inWindow) {
                if (!svgRaster_) {
                    EnsureSvgRaster(dest, r);   /* 首帧同步 (load 一次性, 非缩放热路径) */
                } else {
                    uint64_t renW = (uint64_t)needW * 13 / 10;
                    uint64_t renH = (uint64_t)needH * 13 / 10;
                    if (renW > kCap || renH > kCap) {
                        const double s = (double)kCap / (double)(renW > renH ? renW : renH);
                        renW = (uint64_t)(renW * s);
                        renH = (uint64_t)(renH * s);
                    }
                    if (renW == 0) renW = 1;
                    if (renH == 0) renH = 1;
                    RequestSvgRasterAsync_((uint32_t)renW, (uint32_t)renH,
                                           GetCurrentThreadId());
                }
            }
        }
        if (svgRaster_) {
            D2D1_MATRIX_3X2_F oldXf;
            bool rotated = (rotation_ != 0);
            if (rotated) {
                auto* rt = r.RT();
                rt->GetTransform(&oldXf);
                float dcx = (dest.left + dest.right ) * 0.5f;
                float dcy = (dest.top  + dest.bottom) * 0.5f;
                auto xf = D2D1::Matrix3x2F::Rotation((float)rotation_,
                                                     D2D1::Point2F(dcx, dcy)) * oldXf;
                rt->SetTransform(xf);
            }
            auto ps = svgRaster_->GetPixelSize();
            float pscale = ps.width > 0
                ? (dest.right - dest.left) / (float)ps.width : 1.0f;
            auto interp = (!antialias_ && pscale >= 1.0f)
                ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                : PickInterp(pscale);
            r.DrawBitmapHQ(svgRaster_.Get(), dest, 1.0f, interp);
            if (rotated) r.RT()->SetTransform(oldXf);
        }
        r.PopClip();
        return;
    }

    // rotation != 0: 套一层绕 dest 中心的旋转 transform. 内部 DrawBitmap 仍
    // 用 logical dest, D2D 会把 bitmap 围着中心旋转, 视觉 AABB 自然变成
    // effW×effH×zoom. rotation == 0 时跳过 GetTransform/SetTransform 开销.
    D2D1_MATRIX_3X2_F oldXf;
    bool rotated = (rotation_ != 0);
    if (rotated) {
        auto* rt = r.RT();
        rt->GetTransform(&oldXf);
        float dcx = (dest.left + dest.right ) * 0.5f;
        float dcy = (dest.top  + dest.bottom) * 0.5f;
        auto xf = D2D1::Matrix3x2F::Rotation((float)rotation_,
                                              D2D1::Point2F(dcx, dcy)) * oldXf;
        rt->SetTransform(xf);
    }

    // 1) preview 兜底（最粗，永远先画）
    // Interpolation 走 PickInterp: 大幅下采 (zoom < 0.5) 退 HQ_LINEAR 避免
    // CUBIC negative-lobe 在高对比边缘振铃出色边. 适度缩放 / 上采保持
    // HQ_CUBIC 的锐度优势.
    if (preview_) {
        auto ps = preview_->GetPixelSize();
        float pscale = ps.width > 0
            ? (dest.right - dest.left) / static_cast<float>(ps.width)
            : 1.0f;
        // antialias_=false 且上采 (pscale >= 1) → NEAREST 像素清晰; 否则平滑.
        auto pinterp = (!antialias_ && pscale >= 1.0f)
            ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
            : PickInterp(pscale);
        r.DrawBitmapHQ(preview_.Get(), dest, 1.0f, pinterp);
    }

    // 2) 多级金字塔：从最粗到最细，每级把已加载的可见瓦块画上去。
    //    后画的覆盖先画的 → active level（最细）压在最上面，旧级在下兜底。
    //    切级瞬间新级瓦块还没全到时，已有的旧级仍盖在 preview 之上 —— 无缝过渡。
    if (info_.levels > 0) {
        for (int lvl = (int)info_.levels - 1; lvl >= 0; --lvl) {
            DrawLevel(r, (uint32_t)lvl, dest);
        }
    }

    if (rotated) {
        r.RT()->SetTransform(oldXf);
    }

    r.PopClip();
}

void GhImgViewWidget::DrawLevel(Renderer& r, uint32_t level, const D2D1_RECT_F& dest) {
    uint32_t lw = LevelWidth(level);
    uint32_t lh = LevelHeight(level);
    if (lw == 0 || lh == 0) return;

    uint32_t ts = info_.tileSize;
    uint32_t txMax = (lw + ts - 1) / ts;
    uint32_t tyMax = (lh + ts - 1) / ts;

    float scale = LevelToScreenScale(level) * zoom_;
    if (scale <= 1e-6f) return;

    // 可见瓦块范围 — rotation-aware: widget rect 4 角通过反旋转回 logical
    // 空间, 取 AABB, 再换算到 level 像素. rotation=0 时退化为旧路径
    // (RotateCCW 此时是 identity), 无功能差.
    float destLeft = dest.left;
    float destTop  = dest.top;
    float dcx = (dest.left + dest.right ) * 0.5f;
    float dcy = (dest.top  + dest.bottom) * 0.5f;
    auto cornerToLevel = [&](float sx, float sy, float& lx, float& ly) {
        float dx = sx - dcx;
        float dy = sy - dcy;
        float rdx, rdy;
        RotateCCW(rotation_, dx, dy, rdx, rdy);
        lx = (dcx + rdx - destLeft) / scale;
        ly = (dcy + rdy - destTop ) / scale;
    };
    float lx[4], ly[4];
    cornerToLevel(rect.left,  rect.top,    lx[0], ly[0]);
    cornerToLevel(rect.right, rect.top,    lx[1], ly[1]);
    cornerToLevel(rect.right, rect.bottom, lx[2], ly[2]);
    cornerToLevel(rect.left,  rect.bottom, lx[3], ly[3]);
    float vlx0 = std::min({lx[0], lx[1], lx[2], lx[3]});
    float vlx1 = std::max({lx[0], lx[1], lx[2], lx[3]});
    float vly0 = std::min({ly[0], ly[1], ly[2], ly[3]});
    float vly1 = std::max({ly[0], ly[1], ly[2], ly[3]});

    int tx0 = std::max(0, (int)std::floor(vlx0 / ts));
    int ty0 = std::max(0, (int)std::floor(vly0 / ts));
    int tx1 = std::min((int)txMax, (int)std::floor((vlx1 - 1) / ts) + 1);
    int ty1 = std::min((int)tyMax, (int)std::floor((vly1 - 1) / ts) + 1);

    // Interpolation: 大幅下采 (scale < 0.5) 退 HQ_LINEAR 避免 cubic 振铃
    // 出色边. 适度缩放 / 上采保持 HQ_CUBIC. scale 局部变量已是 screen-per-
    // source-px, 见上面 LevelToScreenScale * zoom_.
    // antialias_=false 且上采 (scale >= 1) → NEAREST 像素清晰; 否则平滑.
    auto interp = (!antialias_ && scale >= 1.0f)
        ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
        : PickInterp(scale);
    // snap 屏幕坐标到整数【设备像素】(dest rect 是 DIP, ctx 按 dpi_scale_ 缩到设备).
    const float dpr = dpi_scale_ > 1e-3f ? dpi_scale_ : 1.0f;
    auto snapDev = [dpr](float v) { return std::round(v * dpr) / dpr; };

    // L105: 把本级可见瓦块先 1:1 精确拼到一张【连续】离屏位图(无内部瓦块边界),
    // 再整体 device-snap + 缩放一次上屏。非整数缩放下逐块直绘会出两类半透明缝:
    //   ① 分数设备像素部分覆盖(相邻块共享边界总覆盖<100%, 透背景);
    //   ② 每块独立插值在瓦块边缘钳位 → 插值梯度不连续。
    // 拼成连续位图后, 缩放时 CUBIC/LINEAR 跨越原瓦块边界连续采样 → 两类缝全消。
    ID2D1DeviceContext5* ctx5 = r.RT5();
    const uint32_t bx0 = (uint32_t)tx0 * ts;
    const uint32_t by0 = (uint32_t)ty0 * ts;
    const uint32_t bw  = std::min(lw - bx0, (uint32_t)(tx1 - tx0) * ts);
    const uint32_t bh  = std::min(lh - by0, (uint32_t)(ty1 - ty0) * ts);
    constexpr uint32_t kMaxComposite = 4096;   // bbox 超此则退逐块直绘(安全阀)
    bool composited = false;

    if (ctx5 && bw > 0 && bh > 0 && bw <= kMaxComposite && bh <= kMaxComposite) {
        if (!composite_ || compositeW_ < bw || compositeH_ < bh) {
            uint32_t nw = std::max(bw, compositeW_);
            uint32_t nh = std::max(bh, compositeH_);
            D2D1_BITMAP_PROPERTIES1 bp = {};
            bp.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                D2D1_ALPHA_MODE_PREMULTIPLIED);
            bp.dpiX = 96.0f; bp.dpiY = 96.0f;
            bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
            Microsoft::WRL::ComPtr<ID2D1Bitmap1> nb;
            if (SUCCEEDED(ctx5->CreateBitmap(D2D1::SizeU(nw, nh), nullptr, 0, &bp, &nb))) {
                composite_ = nb; compositeW_ = nw; compositeH_ = nh;
            } else {
                composite_.Reset(); compositeW_ = compositeH_ = 0;
            }
        }
        if (composite_) {
            // 切离屏 target, identity transform + 96 DPI → 1:1 精确拼瓦块.
            Microsoft::WRL::ComPtr<ID2D1Image> oldTarget;
            ctx5->GetTarget(&oldTarget);
            D2D1_MATRIX_3X2_F oldXf2; ctx5->GetTransform(&oldXf2);
            float odx = 96.0f, ody = 96.0f; ctx5->GetDpi(&odx, &ody);

            ctx5->SetTarget(composite_.Get());
            ctx5->SetDpi(96.0f, 96.0f);
            ctx5->SetTransform(D2D1::Matrix3x2F::Identity());
            ctx5->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, (float)bw, (float)bh),
                                       D2D1_ANTIALIAS_MODE_ALIASED);
            ctx5->Clear(D2D1::ColorF(0, 0, 0, 0));   // 缺的瓦块留透明 → 缩放后透出 preview
            for (int ty = ty0; ty < ty1; ++ty) {
                for (int tx = tx0; tx < tx1; ++tx) {
                    auto it = tiles_.find(TileKey{level, (uint32_t)tx, (uint32_t)ty});
                    if (it == tiles_.end()) continue;
                    float lx = (float)(tx * (int)ts - (int)bx0);
                    float ly = (float)(ty * (int)ts - (int)by0);
                    D2D1_RECT_F dr = {lx, ly, lx + (float)it->second.w, ly + (float)it->second.h};
                    ctx5->DrawBitmap(it->second.bmp.Get(), dr, 1.0f,
                                      D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
                }
            }
            ctx5->PopAxisAlignedClip();
            ctx5->SetTarget(oldTarget.Get());
            ctx5->SetDpi(odx, ody);
            ctx5->SetTransform(oldXf2);

            // 整体上屏: 源 rect=[0,0,bw,bh], dest device-snap 外边界 + CUBIC/LINEAR 缩放.
            float X0 = snapDev(destLeft + (float)bx0        * scale);
            float Y0 = snapDev(destTop  + (float)by0        * scale);
            float X1 = snapDev(destLeft + (float)(bx0 + bw) * scale);
            float Y1 = snapDev(destTop  + (float)(by0 + bh) * scale);
            D2D1_RECT_F dst = {X0, Y0, X1, Y1};
            D2D1_RECT_F src = {0.0f, 0.0f, (float)bw, (float)bh};
            ctx5->DrawBitmap(composite_.Get(), dst, 1.0f, interp, &src);
            composited = true;
        }
    }

    if (!composited) {   // fallback: 逐块直绘(snapDev 消部分覆盖缝)
        for (int ty = ty0; ty < ty1; ++ty) {
            for (int tx = tx0; tx < tx1; ++tx) {
                auto it = tiles_.find(TileKey{level, (uint32_t)tx, (uint32_t)ty});
                if (it == tiles_.end()) continue;
                float x0 = snapDev(destLeft + (float)(tx * (int)ts)                     * scale);
                float y0 = snapDev(destTop  + (float)(ty * (int)ts)                     * scale);
                float x1 = snapDev(destLeft + (float)(tx * (int)ts + (int)it->second.w) * scale);
                float y1 = snapDev(destTop  + (float)(ty * (int)ts + (int)it->second.h) * scale);
                D2D1_RECT_F tileDest = {x0, y0, x1, y1};
                r.DrawBitmapHQ(it->second.bmp.Get(), tileDest, 1.0f, interp);
            }
        }
    }

}

bool GhImgViewWidget::OnMouseDown(const MouseEvent& e) {
    if (!Contains(e.x, e.y)) return false;
    dragging_   = true;
    dragStartX_ = e.x;
    dragStartY_ = e.y;
    dragPanX_   = panX_;
    dragPanY_   = panY_;
    return true;
}

bool GhImgViewWidget::OnMouseMove(const MouseEvent& e) {
    if (!dragging_) return false;
    // drag 期间 fire 基类 onMouseMoveHook 让宿主观察拖动 (实现"图片拖出"等
    // 自定义手势). 窗口的 pressed 分支直接调本方法、不 fire hook (只有 hover
    // 路径 fire), 故在此补 fire, 否则宿主在 drag 期间收不到 move.
    // 宿主若在 hook 内发起 DoDragDrop, 其夺鼠标 capture → WM_CAPTURECHANGED →
    // CancelMouseCapture → 本 widget OnMouseUp → dragging_=false; DoDragDrop
    // 返回后下面复检 dragging_ 为假 → 不再 pan (拖出后图不被 pan 走).
    if (onMouseMoveHook) onMouseMoveHook(e);
    if (!dragging_) return true;
    // 拖动平移按轴锁 (锁定/只读视图): 锁住的轴拖动不动。放在 hook 之后 → 宿主仍能
    // 观察拖动手势 (如"拖出图片"), 只是被锁轴不平移; 命令式 SetPan 不受影响。
    // 长图锁水平 (lockX,!lockY) → 左右固定居中、上下仍可拖动阅读。
    if (panLockX_ && panLockY_) return true;   // 全锁: 无平移、无重绘 (同原 early-return)
    if (!panLockX_) panX_ = dragPanX_ + (e.x - dragStartX_);
    if (!panLockY_) panY_ = dragPanY_ + (e.y - dragStartY_);
    ConstrainPan();
    /* L47 follow-up: drag 期间完全不 fire NotifyViewport, drag 结束 OnMouseUp
     * 才 fire 一次精解. 之前实测节流 100ms 也不够 — drag 跨越 tile 数决定
     * 总 enqueue 量, 不取决于 callback fire 频率. 1 秒 drag 实测 234 tile
     * decode + set_tile (UI 线程 D2D CreateBitmap ~1-2ms/tile = ~470ms 阻塞) →
     * 用户感知卡.
     *
     * drag 期间 OnDraw 仍跟随 pan 渲染 cache 中已有的 tile (DrawLevel pyramid
     * fallback), 视觉是 "preview 模糊跟着 pan, drag 结束才精解新区域" —
     * 跟浏览器 / Photoshop drag 大图体验一致, 不卡 UI. tile cache 在 drag
     * 期间也不被打扰 (无新 SetTile, 无 evict), 旧 viewport 内 tile 保留.
     *
     * 仍 InvalidateAllWindows 让 D2D 重绘新 pan 后的 viewport (用现有 tile +
     * preview). */
    InvalidateAllWindows();
    return true;
}

bool GhImgViewWidget::OnMouseUp(const MouseEvent& /*e*/) {
    if (!dragging_) return false;
    dragging_ = false;
    /* L47 follow-up: drag 结束强 fire NotifyViewport — drag 期间 OnMouseMove
     * 完全不 fire callback (避免 tile enqueue 风暴, 跨多 tile 边界拖动会让
     * UI 线程 set_tile 卡几百 ms). drag 释放后这条 fire 让 caller 拿到最终
     * viewport, push_visible_tiles_ 精解新可见区 tile, 画面立即清晰. */
    NotifyViewport();
    return true;
}

bool GhImgViewWidget::OnMouseWheel(const MouseEvent& e) {
    if (!Contains(e.x, e.y)) return false;
    // wheelZoomEnabled_=false: 不内部缩放, 让宿主 (ui_widget_on_mouse_wheel
    // hook 已在分发开头无条件 fire) 自行决定滚轮行为 (如切图). 返 false = 未消费.
    if (!wheelZoomEnabled_) return false;
    float factor = (e.delta > 0) ? 1.25f : 1.0f / 1.25f;
    SetZoomAround(zoom_ * factor, e.x, e.y);
    return true;
}

D2D1_SIZE_F GhImgViewWidget::SizeHint() const {
    return {200.0f, 200.0f};
}

// ===== 几何辅助 =====

uint32_t GhImgViewWidget::LevelWidth(uint32_t level) const {
    uint32_t w = info_.fullWidth;
    for (uint32_t i = 0; i < level; ++i) w = std::max(1u, w / 2);
    return w;
}

uint32_t GhImgViewWidget::LevelHeight(uint32_t level) const {
    uint32_t h = info_.fullHeight;
    for (uint32_t i = 0; i < level; ++i) h = std::max(1u, h / 2);
    return h;
}

float GhImgViewWidget::LevelToScreenScale(uint32_t level) const {
    // 顶级宽 / level 宽 —— level 像素到顶级像素的放大倍数
    if (info_.fullWidth == 0) return 1.0f;
    uint32_t lw = LevelWidth(level);
    if (lw == 0) return 1.0f;
    return (float)info_.fullWidth / (float)lw;
}

D2D1_RECT_F GhImgViewWidget::ComputeDestRect() const {
    float fullW = (float)info_.fullWidth;
    float fullH = (float)info_.fullHeight;
    float w = fullW * zoom_;
    float h = fullH * zoom_;
    float cx = (rect.left + rect.right) * 0.5f + panX_;
    float cy = (rect.top  + rect.bottom) * 0.5f + panY_;
    return D2D1_RECT_F{cx - w * 0.5f, cy - h * 0.5f,
                       cx + w * 0.5f, cy + h * 0.5f};
}

D2D1_RECT_F GhImgViewWidget::ComputeVisualDestRect() const {
    float effW = (float)EffectiveImageWidth();
    float effH = (float)EffectiveImageHeight();
    float w = effW * zoom_;
    float h = effH * zoom_;
    float cx = (rect.left + rect.right) * 0.5f + panX_;
    float cy = (rect.top  + rect.bottom) * 0.5f + panY_;
    return D2D1_RECT_F{cx - w * 0.5f, cy - h * 0.5f,
                       cx + w * 0.5f, cy + h * 0.5f};
}

uint32_t GhImgViewWidget::EffectiveImageWidth() const {
    return (rotation_ == 90 || rotation_ == 270) ? info_.fullHeight : info_.fullWidth;
}

uint32_t GhImgViewWidget::EffectiveImageHeight() const {
    return (rotation_ == 90 || rotation_ == 270) ? info_.fullWidth : info_.fullHeight;
}

void GhImgViewWidget::ScreenToImage(float sx, float sy, float& ix, float& iy) const {
    // 反向变换: screen → (减 widget center + pan) → CCW 反旋转 → 除 zoom →
    // 加图像中心. 顶级图像坐标系 (fullW × fullH).
    float fullW = (float)info_.fullWidth;
    float fullH = (float)info_.fullHeight;
    float cx = (rect.left + rect.right) * 0.5f;
    float cy = (rect.top  + rect.bottom) * 0.5f;
    float fx = sx - cx - panX_;
    float fy = sy - cy - panY_;
    float rx, ry;
    RotateCCW(rotation_, fx, fy, rx, ry);
    float z = (zoom_ > 1e-6f) ? zoom_ : 1e-6f;
    ix = rx / z + fullW * 0.5f;
    iy = ry / z + fullH * 0.5f;
}

void GhImgViewWidget::ImageToScreen(float ix, float iy, float& sx, float& sy) const {
    // 正向: image (顶级) → 减图像中心 → 乘 zoom → CW 旋转 → 加 widget center + pan.
    float fullW = (float)info_.fullWidth;
    float fullH = (float)info_.fullHeight;
    float fx = (ix - fullW * 0.5f) * zoom_;
    float fy = (iy - fullH * 0.5f) * zoom_;
    float rx, ry;
    RotateCW(rotation_, fx, fy, rx, ry);
    float cx = (rect.left + rect.right) * 0.5f;
    float cy = (rect.top  + rect.bottom) * 0.5f;
    sx = cx + panX_ + rx;
    sy = cy + panY_ + ry;
}

uint32_t GhImgViewWidget::PickAutoLevel() const {
    // 算 "屏幕物理像素 / level 像素" 比例 (= LevelToScreenScale * zoom *
    // dpi_scale), 选 ≤ 1.0 中最大的 level — 物理上"下采样最少且不上采样".
    // D2D HQ_CUBIC 下采样质量远好于上采样 (上采样无新信息只是插值, 下采样
    // 自带 mipmap LOD + 低通滤波), 所以宁可下采样几倍, 也别上采样 1.x.
    //
    // 实例 (世界地图 9934px fit zoom=0.108, DPI 150% dpi_scale=1.5):
    //   L0 scale_phys = 1.0  * 0.108 * 1.5 = 0.162   (下采样 6.2x)
    //   L1            = 2.0  * ...        = 0.324   (下采样 3.1x)
    //   L2            = 4.0  * ...        = 0.648   (下采样 1.5x) ⭐ 选这个
    //   L3            = 8.0  * ...        = 1.296   (上采样 1.3x = 旧算法选)
    //   L4            = 16.0 * ...        = 2.592   (上采样 2.6x)
    //
    // 算法: lvl=0 升序遍历, scale_phys 单调增. 一旦 > 1.0 退出, 返上一个
    // (= 满足 ≤ 1.0 的最大 lvl). 极小 zoom (全部 ≤ 1) 自然返 levels-1
    // (最高 lvl = 最小 mip, 兜底). zoom 大 (L0 已 > 1) 返 0 (初始, 最高
    // 分辨率 mip, 必然上采样但 caller 主动放大).
    //
    // 历史: build 52 修过算法方向 (旧是反的). build 100 阈值 1.0→0.5 缓解
    // 但没感知 DPI, 150% scaling 下按 DIP 算 0.5 = 物理 0.75 仍偏向上采样.
    // 本版直接按 物理px 算 + 改"≤ 1.0 最大 lvl"逻辑, 根治.
    if (info_.levels == 0) return 0;
    uint32_t result = 0;
    for (uint32_t lvl = 0; lvl < info_.levels; ++lvl) {
        float screen_per_level_phys = LevelToScreenScale(lvl) * zoom_ * dpi_scale_;
        if (screen_per_level_phys > 1.0f) break;
        result = lvl;
    }
    /* 兜底: zoom_*dpi 极大让 L0 已超阈值, for 循环第一次就 break, result=0
     * (初始). 这是要的 — L0 最高分辨率 mip, caller 正在主动放大查看细节. */
    return result;
}

void GhImgViewWidget::SwitchLevel(uint32_t level) {
    if (level >= info_.levels) level = info_.levels - 1;
    // L115: 切级时记下旧级, TrimToViewport_ 据此保留旧级 tile 供 OnDraw fallback 覆盖。
    if (level != activeLevel_) prevActiveLevel_ = activeLevel_;
    activeLevel_ = level;
}

void GhImgViewWidget::ConstrainPan() {
    // v1：完全自由 pan，不约束（小图能拖到画布外，方便对照参考）
    // 后续若要"小图居中、大图限边界"再加。
}

void GhImgViewWidget::NotifyViewport() {
    if (!onViewportChanged) return;
    if (info_.fullWidth == 0 || info_.fullHeight == 0) return;

    Viewport vp{};
    vp.activeLevel = activeLevel_;
    vp.zoom = zoom_;
    vp.panX = panX_;
    vp.panY = panY_;

    uint32_t lw = LevelWidth(activeLevel_);
    uint32_t lh = LevelHeight(activeLevel_);
    uint32_t ts = info_.tileSize;
    uint32_t txMax = (lw + ts - 1) / ts;
    uint32_t tyMax = (lh + ts - 1) / ts;

    D2D1_RECT_F dest = ComputeDestRect();
    float scale = LevelToScreenScale(activeLevel_) * zoom_;
    if (scale <= 1e-6f) {
        vp.visibleTx0 = vp.visibleTy0 = 0;
        vp.visibleTx1 = txMax;
        vp.visibleTy1 = tyMax;
    } else {
        // rotation-aware: 4 角反旋转回 logical, 取 level-px AABB.
        float dcx = (dest.left + dest.right ) * 0.5f;
        float dcy = (dest.top  + dest.bottom) * 0.5f;
        auto corner = [&](float sx, float sy, float& lx, float& ly) {
            float dx = sx - dcx, dy = sy - dcy;
            float rdx, rdy;
            RotateCCW(rotation_, dx, dy, rdx, rdy);
            lx = (dcx + rdx - dest.left) / scale;
            ly = (dcy + rdy - dest.top ) / scale;
        };
        float lx[4], ly[4];
        corner(rect.left,  rect.top,    lx[0], ly[0]);
        corner(rect.right, rect.top,    lx[1], ly[1]);
        corner(rect.right, rect.bottom, lx[2], ly[2]);
        corner(rect.left,  rect.bottom, lx[3], ly[3]);
        float lx0 = std::min({lx[0], lx[1], lx[2], lx[3]});
        float lx1 = std::max({lx[0], lx[1], lx[2], lx[3]});
        float ly0 = std::min({ly[0], ly[1], ly[2], ly[3]});
        float ly1 = std::max({ly[0], ly[1], ly[2], ly[3]});
        int tx0 = std::max(0, (int)std::floor(lx0 / ts));
        int ty0 = std::max(0, (int)std::floor(ly0 / ts));
        int tx1 = std::min((int)txMax, (int)std::floor((lx1 - 1) / ts) + 1);
        int ty1 = std::min((int)tyMax, (int)std::floor((ly1 - 1) / ts) + 1);
        vp.visibleTx0 = (uint32_t)tx0;
        vp.visibleTy0 = (uint32_t)ty0;
        vp.visibleTx1 = (uint32_t)tx1;
        vp.visibleTy1 = (uint32_t)ty1;
    }

    // L48: 主动 trim viewport 外的 tile + 非 active level 的全部 tile.
    // 每个被清的 tile fire onTileEvicted → caller 同步 pushed_tiles_ erase.
    // 跟用户记忆的"按区加载内存小"行为对齐, 内存稳态 = viewport tile × 256KB.
    TrimToViewport_(vp.activeLevel, vp.visibleTx0, vp.visibleTx1,
                                     vp.visibleTy0, vp.visibleTy1);

    // L48 followup: 记 rect, OnDraw 比较防重 fire (resize-detect 用).
    lastNotifiedRect_ = rect;

    onViewportChanged(vp);
}

void GhImgViewWidget::InvalidateAllWindows() {
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM) -> BOOL {
        wchar_t cls[64];
        GetClassNameW(hwnd, cls, 64);
        if (wcscmp(cls, L"UiCore_Window") == 0)
            InvalidateRect(hwnd, nullptr, FALSE);
        return TRUE;
    }, 0);
}

} // namespace ui
