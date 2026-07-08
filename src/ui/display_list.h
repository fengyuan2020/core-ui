#pragma once

#include "render_handles.h"

#include <d2d1.h>
#include <dwrite.h>
#include <cstdint>
#include <string>
#include <vector>

namespace ui {

enum class DrawCommandType : uint8_t {
    Clear,
    PushClip,
    PopClip,
    PushRoundedClip,
    PopRoundedClip,
    PushOpacity,
    PopOpacity,
    PushTransform,
    PopTransform,
    FillRect,
    DrawRect,
    FillRoundedRect,
    DrawRoundedRect,
    DrawBlurredRoundedRect,
    DrawLine,
    DrawText,
    DrawImage,
    FillImagePattern,
    FillGradient,
    DrawSvgIcon,
    DrawSvgDocument,
    DrawSvgTextRuns,
    DrawBackdropBlur,
};

enum class ImageSampling : uint8_t {
    Nearest,
    Linear,
    HighQualityCubic,
};

struct TextStyle {
    D2D1_COLOR_F color = D2D1::ColorF(0, 0, 0, 1);
    float font_size = 14.0f;
    std::wstring font_family;
    int alignment = 0;
    int paragraph_alignment = 0;
    int weight = 400;
    bool word_wrap = false;
};

struct GradientStopRef {
    float position = -1.0f;
    D2D1_COLOR_F color = D2D1::ColorF(0, 0, 0, 1);
};

struct GradientRef {
    bool radial = false;
    float angle_deg = 180.0f;
    float cx_pct = 50.0f;
    float cy_pct = 50.0f;
    float radius_pct = 50.0f;
    float tile_w = -1.0f;
    float tile_h = -1.0f;
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    std::vector<GradientStopRef> stops;
};

struct ImageRef {
    ResourceKey key;
    int width = 0;
    int height = 0;
    int stride = 0;
    PixelFormat format = PixelFormat::BgraPremul;
};

struct SvgPathLayerRef {
    std::vector<std::string> path_data;
    float opacity = 1.0f;
    float stroke_width = 0.0f;
    D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Identity();
};

struct SvgGradientStop {
    float offset = 0.0f;
    D2D1_COLOR_F color = D2D1::ColorF(0, 0, 0, 1);
};

struct SvgTextGradient {
    bool radial = false;
    bool userSpace = false;
    D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Identity();
    float x1 = 0, y1 = 0, x2 = 1, y2 = 0;
    float cx = 0.5f, cy = 0.5f, r = 0.5f;
    std::vector<SvgGradientStop> stops;
};

struct SvgTextRun {
    std::wstring text;
    D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Identity();
    float x = 0.0f;
    float y = 0.0f;
    float fontSize = 16.0f;
    std::wstring fontFamily;
    DWRITE_FONT_WEIGHT fontWeight = DWRITE_FONT_WEIGHT_NORMAL;
    D2D1_COLOR_F color = D2D1::ColorF(D2D1::ColorF::Black);
    float opacity = 1.0f;
    bool hasGradient = false;
    SvgTextGradient gradient;
    int anchor = 0;
    float maxWidth = 0.0f;
    bool block = false;
};

struct SvgTextRunListRef {
    std::vector<SvgTextRun> runs;
    D2D1_MATRIX_3X2_F base_transform = D2D1::Matrix3x2F::Identity();
};

struct SvgIconRef {
    float view_box_w = 24.0f;
    float view_box_h = 24.0f;
    std::vector<std::string> path_data;
    std::vector<SvgPathLayerRef> layers;
};

struct SvgDocumentRef {
    struct DropShadowLayer {
        std::string shadow_xml;
        std::string cover_xml;
        float dx = 0.0f;
        float dy = 0.0f;
        float std_deviation = 0.0f;
    };

    std::string xml;
    float viewport_w = 0.0f;
    float viewport_h = 0.0f;
    std::vector<DropShadowLayer> drop_shadow_layers;
};

struct DrawCommand {
    DrawCommandType type = DrawCommandType::FillRect;
    D2D1_RECT_F rect = {};
    D2D1_POINT_2F p0 = {};
    D2D1_POINT_2F p1 = {};
    D2D1_COLOR_F color = D2D1::ColorF(0, 0, 0, 1);
    float stroke_width = 1.0f;
    float radius_x = 0.0f;
    float radius_y = 0.0f;
    float blur_radius = 0.0f;
    float opacity = 1.0f;
    D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Identity();
    uint32_t text_index = UINT32_MAX;
    uint32_t image_ref_index = UINT32_MAX;
    uint32_t gradient_ref_index = UINT32_MAX;
    uint32_t svg_ref_index = UINT32_MAX;
    uint32_t svg_document_ref_index = UINT32_MAX;
    uint32_t svg_text_ref_index = UINT32_MAX;
    TextStyle text_style;
    ImageSampling sampling = ImageSampling::Linear;
};

class DisplayList {
public:
    DisplayList() = default;
    ~DisplayList();
    DisplayList(const DisplayList&) = delete;
    DisplayList& operator=(const DisplayList&) = delete;
    DisplayList(DisplayList&& other) noexcept;
    DisplayList& operator=(DisplayList&& other) noexcept;

    uint64_t window_id = 0;
    uint64_t generation = 0;
    int width_px = 0;
    int height_px = 0;
    float dpi_scale = 1.0f;

    std::vector<DrawCommand> commands;
    std::vector<std::wstring> text_pool;
    std::vector<ImageRef> image_refs;
    std::vector<GradientRef> gradient_refs;
    std::vector<SvgIconRef> svg_icon_refs;
    std::vector<SvgDocumentRef> svg_document_refs;
    std::vector<SvgTextRunListRef> svg_text_refs;

    bool Empty() const { return commands.empty(); }
    void Clear();
    void AddOwnedResource(ResourceKey key);

private:
    void ReleaseOwnedResources();

    std::vector<ResourceKey> owned_resources_;
};

class DisplayListRecorder {
public:
    DisplayListRecorder() = default;
    explicit DisplayListRecorder(DisplayList base);

    void Reset(DisplayList base = {});
    void Clear(D2D1_COLOR_F color);
    void PushClip(D2D1_RECT_F rect);
    void PopClip();
    void PushRoundedClip(D2D1_RECT_F rect, float rx, float ry);
    void PopRoundedClip();
    void PushOpacity(float opacity, D2D1_RECT_F bounds);
    void PopOpacity();
    void PushTransform(D2D1_MATRIX_3X2_F transform);
    void PopTransform();
    void FillRect(D2D1_RECT_F rect, D2D1_COLOR_F color);
    void DrawRect(D2D1_RECT_F rect, D2D1_COLOR_F color, float strokeWidth);
    void FillRoundedRect(D2D1_RECT_F rect, float rx, float ry, D2D1_COLOR_F color);
    void DrawRoundedRect(D2D1_RECT_F rect, float rx, float ry, D2D1_COLOR_F color, float strokeWidth);
    void DrawBlurredRoundedRect(D2D1_RECT_F rect, float rx, float ry,
                                float blurRadius, D2D1_COLOR_F color);
    void DrawLine(D2D1_POINT_2F p0, D2D1_POINT_2F p1, D2D1_COLOR_F color, float strokeWidth);
    void DrawText(std::wstring text, D2D1_RECT_F rect, TextStyle style);
    void DrawImage(ImageRef image, D2D1_RECT_F dst, ImageSampling sampling, float opacity = 1.0f);
    void FillImagePattern(ImageRef image, D2D1_RECT_F rect);
    void FillGradient(GradientRef gradient, D2D1_RECT_F rect, float radius);
    void DrawSvgIcon(SvgIconRef icon, D2D1_RECT_F dst, D2D1_COLOR_F color);
    void DrawSvgDocument(SvgDocumentRef doc, D2D1_MATRIX_3X2_F transform);
    void DrawSvgTextRuns(SvgTextRunListRef textRuns);
    void DrawBackdropBlur(D2D1_RECT_F rect, float radius, float blurRadius);
    void OwnResource(ResourceKey key);

    DisplayList Finish();

private:
    uint32_t AddText(std::wstring text);
    uint32_t AddImageRef(ImageRef image);
    uint32_t AddGradientRef(GradientRef gradient);
    uint32_t AddSvgIconRef(SvgIconRef icon);
    uint32_t AddSvgDocumentRef(SvgDocumentRef doc);
    uint32_t AddSvgTextRunRef(SvgTextRunListRef textRuns);
    DrawCommand& AddCommand(DrawCommandType type);

    DisplayList list_;
};

} // namespace ui
