#include "rendering/workspace_paint.hpp"
#include "platform_win/unicode_name.hpp"
#include "presentation/color_preview.hpp"
#include "presentation/card_content.hpp"
#include "presentation/card_labels.hpp"
#include "presentation/hanging_layout.hpp"
#include "presentation/swing.hpp"
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numbers>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace cliprope::rendering {
#ifdef CLIPROPE_DRAW_PROBES
namespace { thread_local DrawFault draw_fault{}; }
void inject_draw_fault(DrawFault fault) { draw_fault = fault; }
#endif
using Microsoft::WRL::ComPtr;
using application::CardKind;
using application::CardStatus;
using presentation::Hanging;
using presentation::ImageCardLayout;
using presentation::RopePlaque;
namespace {
thread_local std::uint64_t created_targets{};
thread_local const localization::LocalizationManager* current_labels{};
thread_local std::wstring chrome_face = L"Segoe UI";
const localization::LocalizationManager& strings() {
    return current_labels ? *current_labels : presentation::english_labels();
}
void check(HRESULT hr) {
    if (FAILED(hr))
        throw std::runtime_error("Workspace drawing resource unavailable");
}
using D2D1::ColorF;
using D2D1::Matrix3x2F;
using D2D1::Point2F;
using D2D1::RectF;

// Skin parameters. Colors are 0xRRGGBB.
namespace skin {
constexpr unsigned label = 0x5e5144, icon = 0x6c5a4c, muted = 0x8a7b6a, faint = 0xa89a88;
constexpr unsigned steel_edge = 0x4c5157, steel = 0x9ea4ab, steel_light = 0xe4e8ec, steel_dark = 0x5e646b;
constexpr unsigned wood_edge = 0x8a5a2b, wood_grain = 0x9c6a38, wood_light = 0xf7ddb0, wood_bottom = 0x8f5e2e;
constexpr unsigned engraving = 0xfff1d6, warm_paper = 0xfbf7ef, paper_edge = 0x5a4630;
constexpr unsigned drop = 0x2c7972, partial = 0xe0a030, damaged = 0xc8473b, link = 0x3a6ea5, warning = 0xb5533c;
constexpr unsigned hemp = 0xc9a26b;
} // namespace skin

struct Rgb {
    float r, g, b;
};
Rgb split(unsigned c) { return {((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f}; }
unsigned mix(unsigned a, unsigned b, float t) {
    const auto x = split(a), y = split(b);
    auto q = [&](float u, float v) { return unsigned(std::lround(std::clamp(u + (v - u) * t, 0.f, 1.f) * 255)); };
    return q(x.r, y.r) << 16 | q(x.g, y.g) << 8 | q(x.b, y.b);
}
std::uint32_t seed_of(std::string_view id) {
    std::uint32_t hash = 2166136261u;
    for (const auto c : id) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u;
    }
    return hash;
}
// Deterministic artwork noise. The same card always gets the same grain.
class TileDraw {
  public:
    explicit TileDraw(ID2D1RenderTarget* target) : target_(target) { target_->BeginDraw(); }
    TileDraw(const TileDraw&) = delete;
    ~TileDraw() { if (target_) target_->EndDraw(); }
    void finish() {
        const auto result = target_->EndDraw();
        target_ = nullptr;
        check(result);
    }
  private:
    ID2D1RenderTarget* target_;
};

struct Noise {
    std::uint32_t state;
    float next() {
        state = state * 1664525u + 1013904223u;
        return float(state >> 8) / 16777216.f;
    }
    float range(float a, float b) { return a + (b - a) * next(); }
};

constexpr float pi = std::numbers::pi_v<float>;
// Hook loop: centerline circle in card coordinates, see Hanging. Its inner
// edge rests on the top of the rope.
constexpr float wire = 2.8f, loop_radius = 7.4f, loop_x = -0.6f,
                loop_y = Hanging::pivot + loop_radius - wire / 2;
// The straight neck runs from here into the bar and carries a short twist.
constexpr float neck_top = 11.4f;
D2D1_POINT_2F loop(float degrees) {
    const float a = degrees * pi / 180;
    return Point2F(loop_x + loop_radius * std::cos(a), loop_y - loop_radius * std::sin(a));
}
// The hump meets the arms here, slightly above the rounded ends.
constexpr float shoulder = Hanging::bar_end - 0.8f, end_radius = 5;
// Upper outline of the wooden bar, matching the profile geometry.
float bar_top_at(float x, float width) {
    const float half = width / 2, ax = std::abs(x), hump = 0.21f * width, corner = half - end_radius;
    if (ax <= hump)
        return Hanging::bar_top + (shoulder - Hanging::bar_top) * (1 - std::cos(pi * ax / hump)) / 2;
    if (ax <= corner)
        return shoulder + 0.8f * (ax - hump) / std::max(1.f, corner - hump);
    const float d = std::min(end_radius, ax - corner);
    return Hanging::bar_end + end_radius - std::sqrt(end_radius * end_radius - d * d);
}
} // namespace

struct WorkspacePaint::Device {
    unsigned match_color = application::Appearance{}.match_rgb();
    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> write;
    ComPtr<IDWriteTextFormat> label, body, caption, name, detail, host, control, badge, note, url, plaque_name, plaque_total;
    ComPtr<ID2D1StrokeStyle> round, emerging;
    ComPtr<ID2D1PathGeometry> hook_back, hook_front, plaque_grain;
    std::map<CardKind, ComPtr<ID2D1Geometry>> icons;
    ComPtr<ID2D1Geometry> return_icon;
    struct Bar {
        float width{}, bottom{};
        ComPtr<ID2D1PathGeometry> profile, highlight;
        std::array<ComPtr<ID2D1PathGeometry>, 4> grain, streaks;
    };
    std::vector<Bar> bars;

    ComPtr<ID2D1DCRenderTarget> dc_target;
    ID2D1RenderTarget *target = nullptr;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1LinearGradientBrush> wood;
    ComPtr<ID2D1Layer> paper_layer;
    ComPtr<ID2D1BitmapBrush> fibers;
    float fibers_dpi = 0;
    struct RopeArt {
        float width{}, dpi{}, top{}, height{};
        domain::RopeMaterial material;
        ComPtr<ID2D1Bitmap> bitmap;
        std::uint64_t used{};
    };
    std::vector<RopeArt> ropes;
    std::uint64_t tick = 0;
    using TextKey = std::tuple<IDWriteTextFormat *, std::wstring, float, float, float, bool>;
    struct CachedText {
        ComPtr<IDWriteTextLayout> layout;
        std::uint64_t used{};
    };
    std::map<TextKey, CachedText> texts;
    struct BitmapPreview {
        std::weak_ptr<const domain::ImagePreview> pixels;
        ComPtr<ID2D1Bitmap> bitmap;
        std::uint64_t used{};
    };
    std::map<std::tuple<domain::Id, unsigned, unsigned>, BitmapPreview> preview_bitmaps;
    // During a swing, rotate the finished front of the card about the hook.
    // The hook behind the rope and all pointer feedback still draw separately.
    struct CardFace {
        application::CardSummary summary;
        std::weak_ptr<const domain::ImagePreview> thumbnail;
        std::size_t index;
        std::optional<int> capacity;
        ImageCardLayout geometry;
        UINT dpi;
        ComPtr<ID2D1Bitmap> bitmap;
        std::uint64_t used;
    };
    std::map<domain::Id, CardFace> faces;
    std::size_t face_bytes = 0;
    void refresh_ui_formats() {
        ComPtr<IDWriteFactory2> advanced_factory;
        ComPtr<IDWriteFontFallback> fallback;
        if (SUCCEEDED(write.As(&advanced_factory))) advanced_factory->GetSystemFontFallback(&fallback);
        auto font = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& destination,
                        DWRITE_TEXT_ALIGNMENT alignment, bool single) {
            ComPtr<IDWriteTextFormat> next;
            check(write->CreateTextFormat(chrome_face.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size, localization::locale_name(strings().effective_locale()).data(), &next));
            ComPtr<IDWriteTextFormat1> advanced;
            if (fallback && SUCCEEDED(next.As(&advanced))) check(advanced->SetFontFallback(fallback.Get()));
            check(next->SetTextAlignment(alignment));
            if (single) {
                ComPtr<IDWriteInlineObject> sign;
                check(write->CreateEllipsisTrimmingSign(next.Get(), &sign));
                const DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER};
                check(next->SetTrimming(&trim, sign.Get()));
                check(next->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
                check(next->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));
            }
            destination = std::move(next);
        };
        font(10, DWRITE_FONT_WEIGHT_NORMAL, caption, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        font(11, DWRITE_FONT_WEIGHT_NORMAL, detail, DWRITE_TEXT_ALIGNMENT_CENTER, true);
        font(12, DWRITE_FONT_WEIGHT_SEMI_BOLD, control, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        font(12, DWRITE_FONT_WEIGHT_NORMAL, note, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        font(12, DWRITE_FONT_WEIGHT_SEMI_BOLD, plaque_total, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    }

    static std::size_t bytes_of(ID2D1Bitmap *bitmap) {
        if (!bitmap)
            return 0;
        const auto size = bitmap->GetPixelSize();
        return static_cast<std::size_t>(size.width) * size.height * 4;
    }
    std::size_t preview_bytes() const {
        std::size_t bytes = 0;
        for (const auto &[id, item] : preview_bitmaps)
            bytes += bytes_of(item.bitmap.Get());
        return bytes;
    }
    std::size_t bitmap_bytes() const {
        auto bytes = preview_bytes() + face_bytes;
        for (const auto &art : ropes)
            bytes += bytes_of(art.bitmap.Get());
        if (fibers) {
            ComPtr<ID2D1Bitmap> tile;
            fibers->GetBitmap(&tile);
            bytes += bytes_of(tile.Get());
        }
        return bytes;
    }

    Device() {
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()));
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                  reinterpret_cast<IUnknown **>(write.GetAddressOf())));
        auto font = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat> &out) {
            check(write->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", &out));
        };
        auto single_line = [&](IDWriteTextFormat *format, DWRITE_TEXT_ALIGNMENT align) {
            ComPtr<IDWriteInlineObject> sign;
            check(write->CreateEllipsisTrimmingSign(format, &sign));
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            check(format->SetTrimming(&trimming, sign.Get()));
            check(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
            check(format->SetTextAlignment(align));
            check(format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));
        };
        font(12, DWRITE_FONT_WEIGHT_SEMI_BOLD, label);
        font(14, DWRITE_FONT_WEIGHT_NORMAL, body);
        font(10, DWRITE_FONT_WEIGHT_NORMAL, caption);
        font(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, name);
        font(11, DWRITE_FONT_WEIGHT_NORMAL, detail);
        font(15, DWRITE_FONT_WEIGHT_SEMI_BOLD, host);
        font(12, DWRITE_FONT_WEIGHT_SEMI_BOLD, control);
        font(8, DWRITE_FONT_WEIGHT_BOLD, badge);
        font(12, DWRITE_FONT_WEIGHT_NORMAL, note);
        font(11, DWRITE_FONT_WEIGHT_NORMAL, url);
        font(17, DWRITE_FONT_WEIGHT_SEMI_BOLD, plaque_name);
        font(12, DWRITE_FONT_WEIGHT_SEMI_BOLD, plaque_total);
        single_line(plaque_name.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        single_line(plaque_total.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        single_line(label.Get(), DWRITE_TEXT_ALIGNMENT_LEADING);
        single_line(caption.Get(), DWRITE_TEXT_ALIGNMENT_LEADING);
        single_line(name.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        single_line(detail.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        single_line(host.Get(), DWRITE_TEXT_ALIGNMENT_LEADING);
        single_line(control.Get(), DWRITE_TEXT_ALIGNMENT_LEADING);
        single_line(badge.Get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        check(note->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
        refresh_ui_formats();
        check(factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                     D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                         nullptr, 0, &round));
        // The front wire starts flush with the rope edge it emerges from.
        check(factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_ROUND,
                                                                     D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                         nullptr, 0, &emerging));
        build_hook();
        build_icons();
        Noise noise{0x9e3779b9u};
        plaque_grain = grain_lines(RopePlaque::width, RopePlaque::height - 3, noise, 9, 0.8f,
                                   [](float) { return 0.f; });
    }

    // Geometry helpers. These are device-independent and survive target loss.
    ComPtr<ID2D1PathGeometry> path(const std::function<void(ID2D1GeometrySink *)> &draw) {
        ComPtr<ID2D1PathGeometry> result;
        ComPtr<ID2D1GeometrySink> sink;
        check(factory->CreatePathGeometry(&result));
        check(result->Open(&sink));
        draw(sink.Get());
        check(sink->Close());
        return result;
    }
    static void polyline(ID2D1GeometrySink *sink, std::span<const D2D1_POINT_2F> points, bool filled = false) {
        if (points.size() < 2)
            return;
        sink->BeginFigure(points.front(), filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (std::size_t i = 1; i < points.size(); ++i)
            sink->AddLine(points[i]);
        sink->EndFigure(filled ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    }
    ComPtr<ID2D1Geometry> group(std::vector<ComPtr<ID2D1Geometry>> parts) {
        std::vector<ID2D1Geometry *> raw;
        for (auto &p : parts)
            raw.push_back(p.Get());
        ComPtr<ID2D1GeometryGroup> result;
        check(factory->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, raw.data(), static_cast<UINT32>(raw.size()),
                                           &result));
        return result;
    }
    ComPtr<ID2D1Geometry> ellipse(float x, float y, float r) {
        ComPtr<ID2D1EllipseGeometry> result;
        check(factory->CreateEllipseGeometry(D2D1::Ellipse(Point2F(x, y), r, r), &result));
        return result;
    }
    void build_hook() {
        // The tip passes behind the rope and shows below it. The wire comes
        // forward where the loop crosses the rope's upper edge on the left.
        const float crossing =
            180 - std::asin((loop_y - Hanging::pivot) / loop_radius) * 180 / pi;
        hook_back = path([&](ID2D1GeometrySink *s) {
            s->BeginFigure(loop(240), D2D1_FIGURE_BEGIN_HOLLOW);
            s->AddArc(D2D1::ArcSegment(loop(crossing), D2D1::SizeF(loop_radius, loop_radius), 0,
                                       D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            s->EndFigure(D2D1_FIGURE_END_OPEN);
        });
        hook_front = path([&](ID2D1GeometrySink *s) {
            const auto bend = loop(-45);
            s->BeginFigure(loop(crossing), D2D1_FIGURE_BEGIN_HOLLOW);
            s->AddArc(D2D1::ArcSegment(bend, D2D1::SizeF(loop_radius, loop_radius), 0,
                                       D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            s->AddBezier(D2D1::BezierSegment(Point2F(bend.x - 1.6f, bend.y + 1.6f), Point2F(0, bend.y + 1.8f),
                                             Point2F(0, neck_top)));
            s->AddLine(Point2F(0, Hanging::bar_top + 1));
            s->EndFigure(D2D1_FIGURE_END_OPEN);
        });
    }
    // Monochrome line icons drawn in a 13-DIP box.
    void build_icons() {
        icons[CardKind::image] = group(
            {[&] {
                 ComPtr<ID2D1RoundedRectangleGeometry> frame;
                 check(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(RectF(0.7f, 1.8f, 12.3f, 11.2f), 1.8f, 1.8f),
                                                               &frame));
                 return ComPtr<ID2D1Geometry>(frame);
             }(),
             path([&](ID2D1GeometrySink *s) {
                 const D2D1_POINT_2F hills[] = {{2.2f, 9.6f}, {5.2f, 6.2f}, {7.4f, 8.4f}, {9, 7}, {10.9f, 9.6f}};
                 polyline(s, hills);
             }),
             ellipse(9.1f, 4.4f, 1.15f)});
        icons[CardKind::text] = path([&](ID2D1GeometrySink *s) {
            const float rows[] = {2.4f, 5.1f, 7.8f, 10.5f}, ends[] = {12, 12, 12, 7.6f};
            for (int i = 0; i < 4; ++i) {
                const D2D1_POINT_2F stroke[] = {{1, rows[i]}, {ends[i], rows[i]}};
                polyline(s, stroke);
            }
        });
        const auto page = [&](ID2D1GeometrySink *s) {
            const D2D1_POINT_2F outline[] = {{2.6f, 0.8f}, {8.2f, 0.8f}, {10.6f, 3.2f},
                                             {10.6f, 12.2f}, {2.6f, 12.2f}, {2.6f, 0.8f}};
            polyline(s, outline);
            const D2D1_POINT_2F fold[] = {{8.2f, 0.8f}, {8.2f, 3.2f}, {10.6f, 3.2f}};
            polyline(s, fold);
        };
        icons[CardKind::files] = path(page);
        icons[CardKind::rich] = path([&](ID2D1GeometrySink *s) {
            page(s);
            for (float y : {6.2f, 8.4f, 10.4f}) {
                const D2D1_POINT_2F stroke[] = {{4.4f, y}, {y < 10 ? 8.8f : 7.2f, y}};
                polyline(s, stroke);
            }
        });
        icons[CardKind::folder] = path([&](ID2D1GeometrySink *s) {
            const D2D1_POINT_2F outline[] = {{0.8f, 2.6f},  {4.8f, 2.6f},  {6.1f, 4.1f}, {12.2f, 4.1f},
                                             {12.2f, 11}, {0.8f, 11}, {0.8f, 2.6f}};
            polyline(s, outline);
            const D2D1_POINT_2F lip[] = {{0.8f, 5.6f}, {12.2f, 5.6f}};
            polyline(s, lip);
        });
        std::vector<ComPtr<ID2D1Geometry>> links;
        for (float d : {-1.9f, 1.9f}) {
            ComPtr<ID2D1RoundedRectangleGeometry> link;
            check(factory->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(RectF(6.5f - 3.9f, 6.5f - 2.1f, 6.5f + 3.9f, 6.5f + 2.1f), 2.1f, 2.1f), &link));
            ComPtr<ID2D1TransformedGeometry> moved;
            const auto m = Matrix3x2F::Rotation(-45, Point2F(6.5f, 6.5f)) * Matrix3x2F::Translation(d, -d);
            check(factory->CreateTransformedGeometry(link.Get(), &m, &moved));
            links.push_back(moved);
        }
        icons[CardKind::link] = group(links);
        icons[CardKind::unavailable] = group({path([&](ID2D1GeometrySink *s) {
                                                  const D2D1_POINT_2F t[] = {
                                                      {6.5f, 1}, {12.3f, 11.6f}, {0.7f, 11.6f}, {6.5f, 1}};
                                                  polyline(s, t);
                                                  const D2D1_POINT_2F bar[] = {{6.5f, 4.8f}, {6.5f, 7.9f}};
                                                  polyline(s, bar);
                                              }),
                                              ellipse(6.5f, 9.9f, 0.35f)});
        return_icon = path([&](ID2D1GeometrySink *s) {
            const D2D1_POINT_2F rope[] = {{0.8f, 1.6f}, {12.2f, 1.6f}};
            polyline(s, rope);
            const D2D1_POINT_2F shaft[] = {{6.5f, 12}, {6.5f, 4.4f}};
            polyline(s, shaft);
            const D2D1_POINT_2F head[] = {{3.4f, 7.4f}, {6.5f, 4.3f}, {9.6f, 7.4f}};
            polyline(s, head);
        });
    }
    template<class TopEdge>
    ComPtr<ID2D1PathGeometry> grain_lines(float width, float base, Noise& noise, int count, float spread,
                                          TopEdge top_at) {
        const float h = width / 2;
        return path([&](ID2D1GeometrySink *s) {
            for (int i = 0; i < count; ++i) {
                const float t = (i + noise.range(0.25f, 0.75f)) / count;
                const float span = width * noise.range(0.35f, 0.95f);
                const float start = noise.range(-h + 3, std::max(-h + 3, h - 3 - span));
                const float f = noise.range(0.03f, 0.08f), phase = noise.range(0, 2 * pi);
                std::vector<D2D1_POINT_2F> points;
                for (float x = start; x <= std::min(h - 3, start + span); x += 3) {
                    const float top = top_at(x) + 2.2f, bottom = base - 1.8f;
                    const float y = top + (bottom - top) * t + spread * std::sin(x * f + phase);
                    points.push_back(Point2F(x, std::clamp(y, top, bottom)));
                }
                polyline(s, points);
            }
        });
    }
    Bar &bar(float width, float base) {
        for (auto &b : bars)
            if (b.width == width && b.bottom == base)
                return b;
        Bar b;
        b.width = width;
        b.bottom = base;
        const float h = width / 2, hump = 0.21f * width;
        b.profile = path([&](ID2D1GeometrySink *s) {
            s->BeginFigure(Point2F(-h + 2.5f, base), D2D1_FIGURE_BEGIN_FILLED);
            s->AddLine(Point2F(h - 2.5f, base));
            s->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Point2F(h, base),
                                                               Point2F(h, base - 2.5f)));
            s->AddLine(Point2F(h, Hanging::bar_end + end_radius));
            s->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Point2F(h, Hanging::bar_end),
                                                               Point2F(h - end_radius, Hanging::bar_end)));
            s->AddLine(Point2F(hump, shoulder));
            s->AddBezier(D2D1::BezierSegment(Point2F(0.62f * hump, shoulder - 0.3f),
                                             Point2F(0.43f * hump, Hanging::bar_top), Point2F(0, Hanging::bar_top)));
            s->AddBezier(D2D1::BezierSegment(Point2F(-0.43f * hump, Hanging::bar_top),
                                             Point2F(-0.62f * hump, shoulder - 0.3f), Point2F(-hump, shoulder)));
            s->AddLine(Point2F(-h + end_radius, Hanging::bar_end));
            s->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Point2F(-h, Hanging::bar_end),
                                                               Point2F(-h, Hanging::bar_end + end_radius)));
            s->AddLine(Point2F(-h, base - 2.5f));
            s->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Point2F(-h, base),
                                                               Point2F(-h + 2.5f, base)));
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
        });
        b.highlight = path([&](ID2D1GeometrySink *s) {
            std::vector<D2D1_POINT_2F> points;
            for (float x = -h + 4; x <= h - 4; x += 2)
                points.push_back(Point2F(x, bar_top_at(x, width) + 1.1f));
            polyline(s, points);
        });
        for (std::uint32_t variant = 0; variant < b.grain.size(); ++variant) {
            Noise noise{0x9e3779b9u * (variant + 1)};
            const auto top_at = [width](float x) { return bar_top_at(x, width); };
            b.grain[variant] = grain_lines(width, base, noise, 6, 0.45f, top_at);
            b.streaks[variant] = grain_lines(width, base, noise, 2, 0.3f, top_at);
        }
        if (bars.size() >= 64) bars.erase(bars.begin());
        bars.push_back(std::move(b));
        return bars.back();
    }

    void surface() {
        if (target)
            return;
        const auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1DCRenderTarget> new_target;
        ComPtr<ID2D1SolidColorBrush> new_brush;
        ComPtr<ID2D1LinearGradientBrush> new_wood;
        ComPtr<ID2D1Layer> new_paper_layer;
        check(factory->CreateDCRenderTarget(&properties, &new_target));
        check(new_target->CreateSolidColorBrush(ColorF(0), &new_brush));
        const D2D1_GRADIENT_STOP stops[] = {{0, ColorF(0xedcb93)}, {0.4f, ColorF(0xe2b67b)},
                                            {0.8f, ColorF(0xd3a163)}, {1, ColorF(0xbe8a4c)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        check(new_target->CreateGradientStopCollection(stops, 4, &collection));
        check(new_target->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(Point2F(0, Hanging::bar_top), Point2F(0, Hanging::bar_bottom)),
            collection.Get(), &new_wood));
        check(new_target->CreateLayer(nullptr, &new_paper_layer));
        dc_target = std::move(new_target);
        target = dc_target.Get();
        brush = std::move(new_brush);
        wood = std::move(new_wood);
        paper_layer = std::move(new_paper_layer);
        ++created_targets;
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    void discard_target() {
        faces.clear();
        face_bytes = 0;
        preview_bitmaps.clear();
        ropes.clear();
        fibers.Reset();
        fibers_dpi = 0;
        wood.Reset();
        paper_layer.Reset();
        brush.Reset();
        target = nullptr;
        dc_target.Reset();
    }
    struct Session {
        Device& device;
        bool drawing = true;
        explicit Session(Device& owner) : device(owner) {
            device.dc_target->BeginDraw();
            device.dc_target->SetTransform(Matrix3x2F::Identity());
        }
        Session(const Session&) = delete;
        ~Session() {
            if (drawing) {
                device.dc_target->EndDraw();
                device.discard_target();
            }
        }
        bool finish() {
            drawing = false;
            auto hr = device.dc_target->EndDraw();
#ifdef CLIPROPE_DRAW_PROBES
            if (draw_fault == DrawFault::finish_outer) hr = D2DERR_RECREATE_TARGET;
#endif
            if (FAILED(hr)) device.discard_target();
            if (hr == D2DERR_RECREATE_TARGET) return false;
            check(hr);
            return true;
        }
    };
    Session begin(HDC dc, const RECT &bounds, UINT dpi) {
        surface();
        check(dc_target->BindDC(dc, &bounds));
        target->SetDpi(float(dpi), float(dpi));
        return Session(*this);
    }

    // Drawing primitives in the current transform.
    void ink(unsigned rgb, float alpha = 1) { brush->SetColor(ColorF(rgb, alpha)); }
    void box(D2D1_RECT_F r, unsigned rgb, float alpha = 1, float radius = 0) {
        ink(rgb, alpha);
        if (radius > 0)
            target->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get());
        else
            target->FillRectangle(r, brush.Get());
    }
    void line(D2D1_POINT_2F a, D2D1_POINT_2F b, unsigned rgb, float weight, float alpha = 1) {
        ink(rgb, alpha);
        target->DrawLine(a, b, brush.Get(), weight, round.Get());
    }
    void stroke(ID2D1Geometry *g, unsigned rgb, float weight, float alpha = 1, ID2D1StrokeStyle *style = nullptr) {
        ink(rgb, alpha);
        target->DrawGeometry(g, brush.Get(), weight, style ? style : round.Get());
    }
    // A vertical ramp from transparent to `rgb`, built from thin bands.
    void fade(D2D1_RECT_F r, unsigned rgb, float from, float to) {
        constexpr int bands = 10;
        const float step = (r.bottom - r.top) / bands;
        for (int i = 0; i < bands; ++i)
            box(RectF(r.left, r.top + i * step, r.right, r.top + (i + 1) * step), rgb,
                from + (to - from) * (i + 0.5f) / bands);
    }
    IDWriteTextLayout *layout(IDWriteTextFormat *format, std::wstring_view s, float w, float h, float size = 0,
                              bool bold = false) {
        w = std::max(1.f, w);
        h = std::max(1.f, h);
        TextKey key{format, std::wstring(s), w, h, size, bold};
        auto it = texts.find(key);
        if (it == texts.end()) {
            if (texts.size() >= 256)
                texts.erase(std::min_element(texts.begin(), texts.end(), [](const auto &a, const auto &b) {
                    return a.second.used < b.second.used;
                }));
            ComPtr<IDWriteTextLayout> created;
            const auto length = static_cast<UINT32>(s.size());
            check(write->CreateTextLayout(s.data(), length, format, w, h, &created));
            if (size > 0) {
                if (length) {
                    check(created->SetFontSize(size, {0, length}));
                    check(created->SetFontWeight(bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                                 {0, length}));
                }
                // Uniform spacing keeps wrapped previews on an even rhythm.
                check(created->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 1.3f * size, 1.04f * size));
            }
            it = texts.emplace(std::move(key), CachedText{std::move(created), 0}).first;
        }
        it->second.used = ++tick;
        return it->second.layout.Get();
    }
    void text(std::wstring_view s, IDWriteTextFormat *format, D2D1_RECT_F r, unsigned rgb, float alpha = 1) {
        if (s.empty() || r.right <= r.left || r.bottom <= r.top)
            return;
        auto *l = layout(format, s, r.right - r.left, r.bottom - r.top);
        ink(rgb, alpha);
        target->DrawTextLayout(Point2F(r.left, r.top), l, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    ID2D1LinearGradientBrush* wood_finish(float top, float bottom) {
        wood->SetStartPoint(Point2F(0, top));
        wood->SetEndPoint(Point2F(0, bottom));
        return wood.Get();
    }
    void rope_plaque(const presentation::RopeCurve& curve, float top, const std::string& title, std::size_t count) {
        const RopePlaque placement(curve, top);
        constexpr float half = RopePlaque::width / 2, height = RopePlaque::height, radius = RopePlaque::radius;
        target->SetTransform(Matrix3x2F::Translation(placement.left + half, placement.top));
        box(RectF(-half, 0, half, height), skin::wood_bottom, 1, radius);
        const auto face = D2D1::RoundedRect(RectF(-half + 0.5f, 0.5f, half - 0.5f, height - 2.4f),
                                             radius - 0.5f, radius - 0.5f);
        target->FillRoundedRectangle(face, wood_finish(0, height - 2.4f));
        stroke(plaque_grain.Get(), skin::wood_grain, 0.7f, 0.22f);
        ink(skin::wood_edge);
        target->DrawRoundedRectangle(face, brush.Get(), 0.9f);
        line(Point2F(-half + radius, 1.5f), Point2F(half - radius, 1.5f), skin::wood_light, 1, 0.85f);
        for (const float x : {10 - half, half - 10}) {
            const auto head = D2D1::Ellipse(Point2F(x, 8.5f), 3.4f, 3.4f);
            ink(skin::wood_bottom, 0.6f);
            target->FillEllipse(D2D1::Ellipse(Point2F(x, 9.4f), 3.8f, 3.8f), brush.Get());
            ink(skin::steel);
            target->FillEllipse(head, brush.Get());
            ink(skin::steel_edge);
            target->DrawEllipse(head, brush.Get(), 0.8f);
            ink(skin::steel_light);
            target->FillEllipse(D2D1::Ellipse(Point2F(x - 0.6f, 7.2f), 1.9f, 1.0f), brush.Get());
        }
        auto label_text = win::unicode::wide(title);
        for (auto& c : label_text) if (c < L' ' || c == 0x7f) c = L' ';
        text(label_text, plaque_name.Get(), RectF(16 - half, 3, half - 16, 28), skin::paper_edge);
        text(strings().format(localization::TextKey::rope_total, {{"count", std::to_wstring(count)}}), plaque_total.Get(), RectF(4 - half, 26, half - 4, 43), skin::paper_edge);
        target->SetTransform(Matrix3x2F::Identity());
    }
    float width_of(std::wstring_view s, IDWriteTextFormat *format) {
        DWRITE_TEXT_METRICS m{};
        check(layout(format, s, 1000, 40)->GetMetrics(&m));
        return m.widthIncludingTrailingWhitespace;
    }
    void icon(CardKind kind, D2D1_RECT_F box_rect, unsigned rgb, float weight = 1.2f) {
        auto found = icons.find(kind);
        if (found == icons.end())
            return;
        glyph(found->second.Get(), box_rect, rgb, weight);
    }
    void glyph(ID2D1Geometry *g, D2D1_RECT_F box_rect, unsigned rgb, float weight) {
        D2D1_MATRIX_3X2_F previous{};
        target->GetTransform(&previous);
        const float scale = (box_rect.right - box_rect.left) / 13;
        target->SetTransform(Matrix3x2F::Scale(scale, scale) * Matrix3x2F::Translation(box_rect.left, box_rect.top) *
                             previous);
        stroke(g, rgb, weight / scale);
        target->SetTransform(previous);
    }

    // Artwork caches that depend on the render target and its DPI.
    void ensure_fibers(float dpi) {
        if (fibers && fibers_dpi == dpi)
            return;
        constexpr float tile_size = 96;
        ComPtr<ID2D1BitmapRenderTarget> tile;
#ifdef CLIPROPE_DRAW_PROBES
        if (draw_fault == DrawFault::create_tile) check(E_OUTOFMEMORY);
#endif
        check(target->CreateCompatibleRenderTarget(D2D1::SizeF(tile_size, tile_size), &tile));
        ComPtr<ID2D1SolidColorBrush> pen;
        check(tile->CreateSolidColorBrush(ColorF(0x6e5a40), &pen));
        TileDraw drawing(tile.Get());
        tile->Clear(ColorF(0, 0));
        Noise noise{0x5eedu};
        for (int i = 0; i < 34; ++i) {
            const float x = noise.range(0, tile_size), y = noise.range(0, tile_size);
            const float length = noise.range(1.6f, 5), angle = noise.range(0, 2 * pi), bend = noise.range(-0.8f, 0.8f);
            const float dx = std::cos(angle), dy = std::sin(angle);
            const float weight = noise.range(0.4f, 0.7f), alpha = noise.range(0.025f, 0.055f);
            auto fiber = path([&](ID2D1GeometrySink *s) {
                s->BeginFigure(Point2F(x, y), D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
                    Point2F(x + dx * length / 2 - dy * bend, y + dy * length / 2 + dx * bend),
                    Point2F(x + dx * length, y + dy * length)));
                s->EndFigure(D2D1_FIGURE_END_OPEN);
            });
            pen->SetOpacity(alpha);
            // Repeat across tile edges so the wrapped brush has no seams.
            for (float ox : {-tile_size, 0.f, tile_size})
                for (float oy : {-tile_size, 0.f, tile_size}) {
                    tile->SetTransform(Matrix3x2F::Translation(ox, oy));
                    tile->DrawGeometry(fiber.Get(), pen.Get(), weight, round.Get());
                }
        }
        tile->SetTransform(Matrix3x2F::Identity());
        for (int i = 0; i < 120; ++i) {
            pen->SetOpacity(noise.range(0.04f, 0.1f));
            const float r = noise.range(0.2f, 0.45f);
            tile->FillEllipse(D2D1::Ellipse(Point2F(noise.range(1, tile_size - 1), noise.range(1, tile_size - 1)), r, r),
                              pen.Get());
        }
        drawing.finish();
#ifdef CLIPROPE_DRAW_PROBES
        if (draw_fault == DrawFault::finish_tile) check(E_FAIL);
#endif
        ComPtr<ID2D1Bitmap> bitmap;
        check(tile->GetBitmap(&bitmap));
        ComPtr<ID2D1BitmapBrush> created;
        check(target->CreateBitmapBrush(bitmap.Get(),
                                        D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                                                                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                                        &created));
        fibers = std::move(created);
        fibers_dpi = dpi;
    }
    struct RopePalette {
        unsigned deep, dark, mid, light, high;
    };
    const RopeArt &rope_art(const presentation::RopeCurve &curve, const domain::RopeMaterial &material,
                            float dpi) {
        const float width = curve.width();
        for (auto &art : ropes)
            if (art.width == width && art.material == material && art.dpi == dpi) {
                art.used = ++tick;
                return art;
            }
        if (ropes.size() >= 6)
            ropes.erase(std::min_element(ropes.begin(), ropes.end(),
                                         [](const auto &a, const auto &b) { return a.used < b.used; }));
        const float scale = dpi / 96;
        // Align the bitmap to device pixels so it lands without resampling.
        const float top = std::floor((Hanging::rope_end - 10) * scale) / scale;
        const float bottom = std::ceil((curve.lowest() + 12) * scale) / scale;
        ComPtr<ID2D1BitmapRenderTarget> art;
        check(target->CreateCompatibleRenderTarget(D2D1::SizeF(width, bottom - top), &art));
        ComPtr<ID2D1SolidColorBrush> pen;
        check(art->CreateSolidColorBrush(ColorF(0), &pen));
        ComPtr<ID2D1Layer> clip;
        check(art->CreateLayer(nullptr, &clip));
        const auto& tones = material.tones;
        const RopePalette colors{tones[0],tones[1],tones[2],tones[3],tones[4]};
        const float t = Hanging::rope_thickness;
        auto centerline = path([&](ID2D1GeometrySink *s) {
            std::vector<D2D1_POINT_2F> points;
            for (float x = -8; x <= width + 8; x += 3)
                points.push_back(Point2F(x, curve.y(x)));
            polyline(s, points);
        });
        auto outline = path([&](ID2D1GeometrySink *s) {
            check(centerline->Widen(t, nullptr, nullptr, s));
        });
        Noise noise{material.seed};
        // Three laid strands show as slanted lobes between dark grooves.
        const float lay = 0.95f * t, slant = 0.33f * t;
        auto grooves = path([&](ID2D1GeometrySink *s) {
            for (float x = -lay; x <= width + lay; x += lay) {
                const float y = curve.y(x);
                s->BeginFigure(Point2F(x - slant, y + t / 2 + 0.8f), D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Point2F(x + 0.06f * t, y + 0.03f * t),
                                                                   Point2F(x + slant, y - t / 2 - 0.8f)));
                s->EndFigure(D2D1_FIGURE_END_OPEN);
            }
        });
        auto strands = [&](float along, float from, float to) {
            return path([&](ID2D1GeometrySink *s) {
                for (float x = -lay; x <= width + lay; x += lay) {
                    const float cx = x + lay / 2 + along, cy = curve.y(cx) - 0.1f * t;
                    const D2D1_POINT_2F points[] = {{cx - slant * from, cy + t * from},
                                                    {cx + slant * to, cy - t * to}};
                    polyline(s, points);
                }
            });
        };
        auto lobes = strands(0, 0.3f, 0.3f), glints = strands(0.08f * t, -0.05f, 0.24f);
        auto surface_fibers = path([&](ID2D1GeometrySink *s) {
            for (float x = 0; x < width; x += noise.range(5, 13)) {
                const float y = curve.y(x) + noise.range(-t / 2.5f, t / 2.5f);
                const float length = noise.range(2, 5), tilt = noise.range(-0.8f, 0.4f);
                const D2D1_POINT_2F points[] = {{x, y}, {x + length, y + tilt}};
                polyline(s, points);
            }
        });
        auto loose = path([&](ID2D1GeometrySink *s) {
            for (float x = noise.range(0, 10); x < width; x += noise.range(6, 16)) {
                const bool above = noise.next() < 0.55f;
                const float y = curve.y(x) + (above ? -1 : 1) * (t / 2 - 0.6f);
                const float reach = noise.range(1.4f, 3.8f) * (above ? -1 : 1), drift = noise.range(-2.8f, 2.8f);
                s->BeginFigure(Point2F(x, y), D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Point2F(x + drift * 0.3f, y + reach * 0.7f),
                                                                   Point2F(x + drift, y + reach)));
                s->EndFigure(D2D1_FIGURE_END_OPEN);
            }
        });
        TileDraw drawing(art.Get());
        art->Clear(ColorF(0, 0));
        const auto origin = Matrix3x2F::Translation(0, -top);
        auto draw = [&](ID2D1Geometry *g, unsigned rgb, float alpha, float weight, float dy = 0,
                        ID2D1StrokeStyle *style = nullptr) {
            art->SetTransform(Matrix3x2F::Translation(0, dy) * origin);
            pen->SetColor(ColorF(rgb, alpha));
            art->DrawGeometry(g, pen.Get(), weight, style);
        };
        draw(centerline.Get(), 0x000000, 0.07f, t + 3, 2.8f);
        draw(centerline.Get(), 0x000000, 0.09f, t, 2.4f);
        draw(centerline.Get(), colors.deep, 0.9f, t + 1.1f);
        draw(centerline.Get(), colors.mid, 1, t);
        art->SetTransform(origin);
        art->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), outline.Get()), clip.Get());
        draw(centerline.Get(), colors.dark, 0.55f, t * 0.42f, t * 0.3f);
        draw(centerline.Get(), colors.light, 0.45f, t * 0.36f, -t * 0.2f);
        draw(lobes.Get(), colors.light, 0.55f, t * 0.3f, 0, round.Get());
        // Each strand turns away into shadow just before the next groove.
        art->SetTransform(Matrix3x2F::Translation(-0.14f * t, 0) * origin);
        pen->SetColor(ColorF(colors.dark, 0.3f));
        art->DrawGeometry(grooves.Get(), pen.Get(), t * 0.24f, round.Get());
        draw(grooves.Get(), colors.deep, 0.78f, 1.2f, 0, round.Get());
        draw(glints.Get(), colors.high, 0.6f, t * 0.11f, 0, round.Get());
        draw(surface_fibers.Get(), colors.high, 0.28f, 0.45f, 0, round.Get());
        draw(centerline.Get(), colors.high, 0.3f, 0.8f, -t * 0.36f);
        art->SetTransform(origin);
        art->PopLayer();
        draw(loose.Get(), mix(colors.mid, colors.light, 0.4f), 0.6f, 0.5f, 0, round.Get());
        drawing.finish();
        RopeArt result;
        result.width = width;
        result.dpi = dpi;
        result.material = material;
        result.top = top;
        result.height = bottom - top;
        result.used = ++tick;
        check(art->GetBitmap(&result.bitmap));
        ropes.push_back(std::move(result));
        return ropes.back();
    }
    ID2D1Bitmap *preview(const application::CardSummary &card) {
        constexpr std::size_t bitmap_budget = 32u * 1024u * 1024u;
        if (!card.thumbnail || card.thumbnail->pbgra.size() > bitmap_budget || !card.thumbnail->width ||
            !card.thumbnail->height)
            return nullptr;
        const auto &pixels = *card.thumbnail;
        const auto key = std::make_tuple(card.entry.snapshot_id, pixels.width, pixels.height);
        auto found = preview_bitmaps.find(key);
        if (found == preview_bitmaps.end() || found->second.pixels.lock() != card.thumbnail) {
            if (found != preview_bitmaps.end())
                preview_bitmaps.erase(found);
            while (!preview_bitmaps.empty() &&
                   (preview_bitmaps.size() >= 64 || preview_bytes() + pixels.pbgra.size() > bitmap_budget))
                preview_bitmaps.erase(std::min_element(preview_bitmaps.begin(), preview_bitmaps.end(),
                    [](const auto& a, const auto& b) { return a.second.used < b.second.used; }));
            ComPtr<ID2D1Bitmap> image;
            check(target->CreateBitmap(
                D2D1::SizeU(pixels.width, pixels.height), pixels.pbgra.data(), pixels.width * 4,
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                &image));
            found = preview_bitmaps.emplace(key, BitmapPreview{card.thumbnail, std::move(image)})
                        .first;
        }
        found->second.used = ++tick;
        return found->second.bitmap.Get();
    }

    // Paper content templates. `area` is the content box inside the margins.
    void picture(ID2D1Bitmap *bitmap, const domain::ImagePreview &pixels, D2D1_RECT_F area) {
        const float w = area.right - area.left, h = area.bottom - area.top;
        const float scale = std::min(w / pixels.width, h / pixels.height);
        const float iw = pixels.width * scale, ih = pixels.height * scale;
        const auto r = RectF(area.left + (w - iw) / 2, area.top + (h - ih) / 2, area.left + (w + iw) / 2,
                             area.top + (h + ih) / 2);
        box(RectF(r.left + 0.6f, r.top + 1.4f, r.right + 0.6f, r.bottom + 1.4f), 0x000000, 0.08f);
        // Enlarged small images keep their pixels sharp instead of blurring.
        target->DrawBitmap(bitmap, r, 1,
                           scale >= 2 ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                      : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        ink(0x000000, 0.14f);
        target->DrawRectangle(RectF(r.left + 0.25f, r.top + 0.25f, r.right - 0.25f, r.bottom - 0.25f), brush.Get(),
                              0.5f);
    }
    std::vector<DWRITE_HIT_TEST_METRICS> hit_boxes(IDWriteTextLayout* layout, size_t start, size_t length) {
        UINT32 count{};
        auto hr = layout->HitTestTextRange(UINT32(start), UINT32(length), 0, 0, nullptr, 0, &count);
        if (!count || (FAILED(hr) && hr != E_NOT_SUFFICIENT_BUFFER)) return {};
        std::vector<DWRITE_HIT_TEST_METRICS> boxes(count);
        check(layout->HitTestTextRange(UINT32(start), UINT32(length), 0, 0, boxes.data(), count, &count));
        boxes.resize(count);
        return boxes;
    }
    void find_text(IDWriteTextFormat* format, std::wstring_view value, D2D1_RECT_F area, unsigned rgb, float size,
                   bool bold, std::wstring_view term, const application::SearchHit* hit = nullptr,
                   bool context = false) {
        if (area.right <= area.left || area.bottom <= area.top || term.empty()) return;
        const float w = area.right - area.left, h = area.bottom - area.top;
        auto make = [&](auto str) { return layout(format, str, w, h, size, bold); };
        IDWriteTextLayout* lines = value.empty() ? nullptr : make(value);
        size_t selected = std::wstring_view::npos;
        if (hit) {
            auto fits = [&](size_t start) {
                if (!lines || start > value.size() || hit->range.length > value.size() - start) return false;
                auto token = std::wstring(value.substr(start, hit->range.length));
                application::fold_find(token);
                if (token != term) return false;
                auto boxes = hit_boxes(lines, start, hit->range.length);
                return !boxes.empty() && std::all_of(boxes.begin(), boxes.end(), [&](auto& b) {
                    return !b.isTrimmed && b.top >= 0 && b.left >= -0.5f && b.top + b.height <= h + .5f &&
                           b.left + b.width <= w + .5f;
                });
            };
            selected = hit->range.start;
            if (context || !fits(selected)) {
                value = hit->context;
                selected = hit->range.start - hit->context_start;
                lines = make(value);
                // Reduce leading context until the selected glyphs fit, including
                // on a compact paper or a one-line filename caption.
                while (selected && !fits(selected)) {
                    size_t remove = (selected + 1) / 2;
                    if (remove < selected && value[remove] >= 0xdc00 && value[remove] <= 0xdfff) ++remove;
                    value.remove_prefix(remove);
                    selected -= remove;
                    lines = make(value);
                }
            }
        }
        if (!lines) return;
        target->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
        std::wstring folded(value);
        application::fold_find(folded);
        auto mark = [&](size_t p, bool current) {
            for (const auto& b : hit_boxes(lines, p, term.size())) {
                if (!b.isTrimmed)
                    box(RectF(area.left + b.left, area.top + b.top, area.left + b.left + b.width,
                              area.top + b.top + b.height),
                        current ? match_color : 0xf3de9a);
            }
        };
        for (size_t p = 0; (p = folded.find(term, p)) != folded.npos; p += term.size())
            if (p != selected) mark(p, false);
        if (selected != value.npos) mark(selected, true);
        ink(rgb);
        target->DrawTextLayout(Point2F(area.left, area.top), lines, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        target->PopAxisAlignedClip();
    }
    void body_text(IDWriteTextFormat *format, std::wstring_view s, D2D1_RECT_F area, unsigned rgb, float size,
                   bool bold, unsigned paper) {
        if (s.empty() || area.bottom - area.top < 4)
            return;
        auto *l = layout(format, s, area.right - area.left, area.bottom - area.top, size, bold);
        ink(rgb);
        target->DrawTextLayout(Point2F(area.left, area.top), l, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        DWRITE_TEXT_METRICS m{};
        check(l->GetMetrics(&m));
        if (m.height > area.bottom - area.top + 0.5f) {
            const float band = std::min(18.f, 1.1f * size);
            fade(RectF(area.left - 1, area.bottom - band, area.right + 1, area.bottom + 1), paper, 0, 1);
        }
    }
    void document(float cx, float top, std::wstring_view file) {
        const float w = 32, h = 40, l = cx - w / 2, fold = 9;
        auto sheet = path([&](ID2D1GeometrySink *s) {
            const D2D1_POINT_2F outline[] = {
                {l, top}, {l + w - fold, top}, {l + w, top + fold}, {l + w, top + h}, {l, top + h}};
            polyline(s, outline, true);
        });
        ink(0x000000, 0.08f);
        D2D1_MATRIX_3X2_F previous{};
        target->GetTransform(&previous);
        target->SetTransform(Matrix3x2F::Translation(0.8f, 1.6f) * previous);
        target->FillGeometry(sheet.Get(), brush.Get());
        target->SetTransform(previous);
        ink(0xfffdf8);
        target->FillGeometry(sheet.Get(), brush.Get());
        stroke(sheet.Get(), 0x8c7b68, 1.1f);
        const D2D1_POINT_2F corner[] = {{l + w - fold, top}, {l + w - fold, top + fold}, {l + w, top + fold}};
        stroke(path([&](ID2D1GeometrySink *s) { polyline(s, corner); }).Get(), 0x8c7b68, 1.1f);
        const auto ext = presentation::extension(file);
        if (ext.empty())
            return;
        unsigned color = skin::icon;
        const auto is = [&](std::initializer_list<std::wstring_view> list) {
            return std::find(list.begin(), list.end(), ext) != list.end();
        };
        if (ext == L"PDF")
            color = 0xc8473b;
        else if (is({L"DOC", L"DOCX", L"RTF"}))
            color = 0x2f6db5;
        else if (is({L"XLS", L"XLSX", L"CSV"}))
            color = 0x2e8b57;
        else if (is({L"PPT", L"PPTX"}))
            color = 0xd0622f;
        else if (is({L"ZIP", L"7Z", L"RAR"}))
            color = 0x8d6e4a;
        else if (is({L"PNG", L"JPG", L"JPEG", L"GIF", L"BMP", L"SVG", L"WEBP", L"TIF", L"TIFF"}))
            color = 0x7a5ba6;
        const auto label_width = std::min(w + 6, std::max(18.f, width_of(ext, badge.Get()) + 7));
        const auto badge_rect = RectF(cx - label_width / 2, top + h - 15, cx + label_width / 2, top + h - 5);
        box(badge_rect, color, 1, 2);
        text(ext, badge.Get(), badge_rect, 0xffffff);
    }
    void folder_art(float cx, float top) {
        const float w = 40, h = 30, l = cx - w / 2, t = top + 6;
        box(RectF(l + 0.8f, t + 1.6f, l + w + 0.8f, t + h + 1.6f), 0x000000, 0.08f, 3);
        box(RectF(l, t - 4, l + 16, t + 4), 0xd4a85a, 1, 2.5f);
        box(RectF(l, t, l + w, t + h), 0xe5c07b, 1, 3);
        box(RectF(l, t + 5, l + w, t + h), 0xedcd8e, 1, 3);
        ink(0xa07c3c);
        target->DrawRoundedRectangle(D2D1::RoundedRect(RectF(l, t, l + w, t + h), 3, 3), brush.Get(), 1);
    }
    void file_card(const application::CardSummary &card, D2D1_RECT_F area, std::wstring_view term) {
        const float cx = (area.left + area.right) / 2, w = area.right - area.left, h = area.bottom - area.top;
        const bool folder = card.kind == CardKind::folder;
        const float top = area.top + std::max(0.f, std::min(6.f, (h - 100) / 2));
        if (folder)
            folder_art(cx, top);
        else
            document(cx, top + 2, card.items ? card.text : L"");
        const std::wstring title = card.items ? card.text : std::wstring(strings().tr(localization::TextKey::card_file_list));
        const auto title_box=RectF(area.left,top+48,area.right,top+66);
        if (!term.empty() && card.items) find_text(name.Get(),title,title_box,skin::label,0,false,term);
        else text(title,name.Get(),title_box,skin::label);
        std::wstring kind;
        if (card.items > 1 || card.more_items)
            kind = strings().format(card.more_items ? localization::TextKey::card_more_item_count : localization::TextKey::card_item_count, {{"count", std::to_wstring(card.items)}});
        else if (card.items)
            kind = presentation::describe_file(card.text, folder, strings());
        text(kind, detail.Get(), RectF(area.left, top + 66, area.right, top + 81), skin::muted);
        if (!card.location.empty() && h >= 96) {
            const auto limit = static_cast<std::size_t>(std::max(8.f, w / 5.2f));
            const auto location=presentation::elide_path(card.location,limit);
            const auto rect=RectF(area.left,top+81,area.right,top+95);
            if (term.empty()) text(location,caption.Get(),rect,skin::faint);
            else find_text(caption.Get(),location,rect,skin::faint,0,false,term);
        }
    }
    void notice(CardKind kind, std::wstring_view message, D2D1_RECT_F area, unsigned rgb) {
        const float cx = (area.left + area.right) / 2, cy = area.top + (area.bottom - area.top) * 0.38f;
        icon(kind, RectF(cx - 12, cy - 12, cx + 12, cy + 12), rgb, 1.5f);
        text(message, note.Get(), RectF(area.left, cy + 18, area.right, cy + 38), rgb);
    }
    void content(const application::CardSummary &card, D2D1_RECT_F area, unsigned paper, std::wstring_view term = {}, const application::SearchHit* hit = nullptr) {
        const float size = static_cast<float>(std::clamp(card.style.font_size, 6.0, 96.0));
        const auto kind = card.image_primary ? CardKind::image : card.kind;
        switch (kind) {
        case CardKind::folder:
        case CardKind::files: {
            using Kind = application::CardSummary::FilePreviewKind;
            const bool missing = card.availability == domain::FileAvailability::missing;
            const bool offline = card.availability == domain::FileAvailability::inaccessible;
            const bool deferred = card.availability == domain::FileAvailability::on_request;
            const std::wstring_view state = missing ? strings().tr(localization::TextKey::card_original_missing_notice) : offline ? strings().tr(localization::TextKey::card_original_inaccessible_notice)
                                  : deferred ? strings().tr(localization::TextKey::card_original_refresh) : card.availability == domain::FileAvailability::decode_failed ? strings().tr(localization::TextKey::card_preview_decode_failed) : std::wstring_view{};
            const auto bottom = area.bottom - (state.empty() ? 0.f : 15.f);
            auto file_area = RectF(area.left, area.top, area.right, bottom);
            if (auto* bitmap = preview(card); bitmap && card.file_preview != Kind::none) {
                picture(bitmap, *card.thumbnail, RectF(file_area.left, file_area.top, file_area.right, std::max(file_area.top + 1, bottom - 18)));
                const auto filename = card.file_preview == Kind::collage ? strings().format(localization::TextKey::card_image_count, {{"count", std::to_wstring(card.items)}}) : card.text;
                const auto rect=RectF(area.left,bottom-17,area.right,bottom);
                if (!term.empty() && card.file_preview!=Kind::collage)
                    find_text(name.Get(),filename,rect,skin::label,0,false,term);
                else text(filename,name.Get(),rect,skin::label);
            } else file_card(card, file_area, term);
            if (!state.empty())
                text(state, caption.Get(),
                     RectF(area.left, bottom, area.right, area.bottom), skin::warning);
            return;
        }
        case CardKind::unavailable:
            notice(CardKind::unavailable, strings().tr(localization::TextKey::card_unavailable), area, skin::warning);
            return;
        case CardKind::rich:
            notice(CardKind::rich, strings().tr(localization::TextKey::card_formatted), area, skin::muted);
            return;
        case CardKind::link: {
            const auto host_name = presentation::link_host(card.text);
            const auto split = std::min(area.bottom, area.top + size * 1.5f);
            if (hit && split + 3 + size * 1.3f > area.bottom) {
                find_text(url.Get(), card.text, area, card.style.ink_rgb(), std::min(size, 16.f), card.style.bold,
                          term, hit);
                return;
            }
            if (!term.empty())
                find_text(host.Get(), host_name, RectF(area.left, area.top, area.right, split), card.style.ink_rgb(),
                          size, card.style.bold, term);
            else
                body_text(host.Get(), host_name, RectF(area.left, area.top, area.right, split), card.style.ink_rgb(),
                          size, card.style.bold, paper);
            if (split + 3 < area.bottom && !term.empty())
                find_text(url.Get(),card.text,RectF(area.left,split+3,area.right,area.bottom),
                          card.style.ink_rgb(),size,card.style.bold,term,hit);
            else if (split + 3 < area.bottom)
                body_text(url.Get(), card.text, RectF(area.left, split + 3, area.right, area.bottom),
                          card.style.ink_rgb(), size, card.style.bold, paper);
            return;
        }
        case CardKind::image:
        case CardKind::text:
            break;
        }
        auto *bitmap = preview(card);
        if (kind == CardKind::image) {
            if (bitmap)
                picture(bitmap, *card.thumbnail, area);
            else {
                const float cx = (area.left + area.right) / 2, cy = (area.top + area.bottom) / 2;
                icon(CardKind::image, RectF(cx - 15, cy - 15, cx + 15, cy + 15), skin::faint, 1.4f);
            }
            return;
        }
        if (bitmap) {
            const float split = area.top + (area.bottom - area.top) * 0.58f;
            picture(bitmap, *card.thumbnail, RectF(area.left, area.top, area.right, split));
            area.top = split + 6;
        }
        const auto& paper_text = card.paper_text ? *card.paper_text : card.text;
        if (!term.empty()) {
            find_text(body.Get(),paper_text,area,
                      card.style.ink_rgb(),size,card.style.bold,term,hit);
            return;
        }
        body_text(body.Get(), paper_text, area,
                  card.style.ink_rgb(), size, card.style.bold, paper);
    }
    void status(const application::CardSummary &card, D2D1_POINT_2F at) {
        if (card.status == CardStatus::complete)
            return;
        const unsigned color = card.status == CardStatus::partial ? skin::partial : skin::damaged;
        ink(color);
        target->FillEllipse(D2D1::Ellipse(at, 3, 3), brush.Get());
        ink(mix(color, 0x000000, 0.3f));
        target->DrawEllipse(D2D1::Ellipse(at, 3, 3), brush.Get(), 0.8f);
    }
    // A soft contact shadow from a few expanding translucent rounded rectangles.
    // The sideways spread stays under half of the gap between neighboring
    // papers, so the gap remains transparent to input even while hovered.
    void shadow(D2D1_RECT_F r, bool lifted) {
        const float dx = lifted ? 1.2f : 0.8f, dy = lifted ? 4.6f : 2.4f, spread = lifted ? 7.f : 4.5f;
        const float alpha = lifted ? 0.05f : 0.035f;
        constexpr int rings = 6;
        for (int i = rings; i >= 1; --i) {
            const float g = spread * i / rings;
            box(RectF(r.left + dx - g * 0.5f, r.top + dy + g * 0.2f, r.right + dx + g * 0.5f, r.bottom + dy + g),
                0x000000, alpha, 2 + g);
        }
    }
    void pencil(D2D1_RECT_F r) {
        const auto x = r.left, y = r.top;
        line(Point2F(x + 3, y + 9), Point2F(x + 10, y + 2), skin::label, 3.3f);
        line(Point2F(x + 1, y + 12), Point2F(x + 3, y + 9), skin::label, 1.2f);
        line(Point2F(x + 8, y + 2), Point2F(x + 11, y + 5), skin::wood_light, 1);
    }
    void close_mark(D2D1_POINT_2F c, bool chip, bool hot, unsigned paper) {
        if (chip) {
            ink(0x000000, 0.1f);
            target->FillEllipse(D2D1::Ellipse(Point2F(c.x, c.y + 0.7f), 10.4f, 10.4f), brush.Get());
            ink(paper, 0.96f);
            target->FillEllipse(D2D1::Ellipse(c, 10, 10), brush.Get());
            ink(mix(paper, skin::paper_edge, 0.4f));
            target->DrawEllipse(D2D1::Ellipse(c, 9.6f, 9.6f), brush.Get(), 0.8f);
        }
        if (hot) {
            ink(0x000000, 0.08f);
            target->FillEllipse(D2D1::Ellipse(c, 10, 10), brush.Get());
        }
        const float d = 3.6f;
        line(Point2F(c.x - d, c.y - d), Point2F(c.x + d, c.y + d), skin::icon, 1.5f);
        line(Point2F(c.x + d, c.y - d), Point2F(c.x - d, c.y + d), skin::icon, 1.5f);
    }
    CardKind icon_kind(const application::CardSummary &card) const {
        if (card.image_primary)
            return CardKind::image;
        return card.kind == CardKind::rich ? CardKind::text : card.kind;
    }

    // One hanging card in its own frame: origin at the rope above the hook.
    struct Placement {
        Matrix3x2F transform;
        bool hovered{};
    };
    Placement place(const application::CardSummary &card, float left, float row_top, float paper,
                    const presentation::RopeCurve &curve, const Feedback &feedback) const {
        const float hook_x = left + paper / 2, r = curve.contact(hook_x);
        const auto angle = feedback.held == card.entry.id ? feedback.held_angle :
                           feedback.swing ? static_cast<float>(feedback.swing->angle(card.entry.id)) : 0.f;
        return {Matrix3x2F::Rotation(angle, Point2F(0, Hanging::pivot)) *
                    Matrix3x2F::Translation(hook_x, row_top + r),
                feedback.dragged.empty() && feedback.hovered == card.entry.id};
    }
    void hook_behind(const Placement &p) {
        target->SetTransform(p.transform);
        stroke(hook_back.Get(), skin::steel_edge, wire + 1.2f);
        stroke(hook_back.Get(), mix(skin::steel, skin::steel_dark, 0.35f), wire);
        target->SetTransform(Matrix3x2F::Translation(-0.55f, -0.55f) * p.transform);
        stroke(hook_back.Get(), skin::steel_light, 0.8f, 0.6f);
    }
    static auto face_inputs(const application::CardSummary &c) {
        return std::tie(c.entry.snapshot_id, c.text, c.paper_text, c.generated_text, c.empty_text, c.text_unavailable,
                        c.formats, c.capabilities, c.style, c.image_primary, c.kind, c.status,
                        c.location, c.items, c.more_items, c.file_preview, c.availability, c.source, c.image_preferences);
    }
    void moving_face(const application::CardSummary &card, const domain::Rope &rope, std::size_t index,
                     const ImageCardLayout& g, UINT dpi, const Placement &placement) {
        const auto bounds = RectF(-g.width / 2 - 12, Hanging::hook_top - 2, g.width / 2 + 12,
                                  g.bottom + 14);
        auto it = faces.find(card.entry.id);
        if (it != faces.end()) {
            const auto &f = it->second;
            if (f.index != index || f.capacity != rope.capacity || f.geometry != g || f.dpi != dpi ||
                face_inputs(f.summary) != face_inputs(card) || f.thumbnail.owner_before(card.thumbnail) ||
                card.thumbnail.owner_before(f.thumbnail)) {
                face_bytes -= bytes_of(f.bitmap.Get());
                faces.erase(it);
                it = faces.end();
            }
        }
        if (it == faces.end()) {
            ComPtr<ID2D1BitmapRenderTarget> tile;
            check(dc_target->CreateCompatibleRenderTarget(
                D2D1::SizeF(bounds.right - bounds.left, bounds.bottom - bounds.top), &tile));
            tile->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            TileDraw drawing(tile.Get());
            tile->Clear(ColorF(0, 0));
            target = tile.Get();
            try {
                const Placement local{Matrix3x2F::Translation(-bounds.left, -bounds.top)};
                hanging(card, rope, index, g, local, {});
            } catch (...) {
                target = dc_target.Get();
                throw;
            }
            target = dc_target.Get();
            drawing.finish();
            ComPtr<ID2D1Bitmap> bitmap;
            check(tile->GetBitmap(&bitmap));
            const auto bytes = bytes_of(bitmap.Get());
            constexpr std::size_t budget = 8u * 1024u * 1024u;
            if (bytes > budget) {
                target->SetTransform(placement.transform);
                target->DrawBitmap(bitmap.Get(), bounds);
                return;
            }
            while (!faces.empty() && face_bytes + bytes > budget) {
                const auto oldest = std::min_element(faces.begin(), faces.end(), [](const auto &a, const auto &b) {
                    return a.second.used < b.second.used;
                });
                face_bytes -= bytes_of(oldest->second.bitmap.Get());
                faces.erase(oldest);
            }
            CardFace face{card, card.thumbnail, index, rope.capacity, g, dpi, std::move(bitmap), 0};
            face.summary.thumbnail.reset();
            it = faces.emplace(card.entry.id, std::move(face)).first;
            face_bytes += bytes;
        }
        it->second.used = ++tick;
        target->SetTransform(placement.transform);
        target->DrawBitmap(it->second.bitmap.Get(), bounds);
    }
    void hanging(const application::CardSummary &source, const domain::Rope &rope, std::size_t index,
                 const ImageCardLayout &g, const Placement &p, const Feedback &feedback) {
        presentation::PreviewCard preview(source, feedback.colors);
        const auto &card = preview.get();
        target->SetTransform(p.transform);
        // Hook in front of the rope, with a twisted neck.
        stroke(hook_front.Get(), skin::steel_edge, wire + 1.2f, 1, emerging.Get());
        stroke(hook_front.Get(), skin::steel, wire, 1, emerging.Get());
        target->SetTransform(Matrix3x2F::Translation(-0.55f, -0.55f) * p.transform);
        stroke(hook_front.Get(), skin::steel_light, 0.9f, 0.9f, emerging.Get());
        target->SetTransform(p.transform);
        const float half = wire / 2 + 0.2f;
        for (float y = neck_top + 1.4f; y < Hanging::bar_top - 0.6f; y += 1.9f) {
            line(Point2F(-half, y + 0.95f), Point2F(half, y - 0.95f), skin::steel_dark, 0.85f, 0.9f);
            line(Point2F(-half + 0.5f, y - 0.2f), Point2F(half - 0.3f, y - 1.45f), skin::steel_light, 0.6f, 0.8f);
        }
        match_color = feedback.match_color;
        auto hit=feedback.find_hit;
        if (hit && (hit->entry!=card.entry.id || hit->snapshot!=card.entry.snapshot_id ||
                    feedback.editing==hit->entry || feedback.edit_preview==&card)) hit=nullptr;
        const bool hidden_field=hit && (hit->field!=application::SearchField::body ||
            card.image_primary || (card.kind!=CardKind::text && card.kind!=CardKind::link));
        if (g.adaptive) {
            const auto opacity = static_cast<float>(std::clamp(card.style.opacity, 0., 1.));
            if (opacity > 0) {
                const auto r = RectF(-g.body_width / 2, g.top, g.body_width / 2, g.bottom);
                if (opacity < 1) target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, Matrix3x2F::Identity(), opacity), paper_layer.Get());
                image_body(card, r, p.hovered);
                if (p.hovered) resolution(card, r);
                if (opacity < 1) target->PopLayer();
            }
        } else
            paper_sheet(card, g, p, feedback.editing == card.entry.id, feedback.find_term,
                        hidden_field ? nullptr : hit);
        if (hit) {
            const auto r = RectF(-g.body_width / 2, g.top, g.body_width / 2, g.bottom);
            ink(0x97683f);
            target->DrawRectangle(r, brush.Get(), 1.4f);
            if (hidden_field || g.adaptive || card.style.opacity <= 0) {
                const auto strip = RectF(r.left + 2, std::max(r.top + 1, r.bottom - 22), r.right - 2, r.bottom - 2);
                box(strip, 0xfff6e0, .96f, 2);
                find_text(body.Get(), {}, RectF(strip.left + 3, strip.top + 1, strip.right - 3, strip.bottom), 0x392c21,
                          12, false, feedback.find_term, hit, true);
            }
        }
        wood_bar(card, rope, index, g, p.hovered && feedback.editing != card.entry.id &&
                 card.menu == application::CardMenuKind::text);
        const bool hidden = card.style.opacity == 0;
        if ((p.hovered || hidden) && feedback.editing != card.entry.id)
            close_mark(Point2F(g.adaptive ? g.width / 2 - 38 : g.width / 2 - Hanging::close_size / 2, g.adaptive ? g.bar_bottom - 12 : Hanging::paper_top + 15), true,
                       p.hovered && feedback.hover_close, paper_color(card.style.background_color));
    }
    void resolution(const application::CardSummary& card, D2D1_RECT_F image) {
        if (!card.hover_resolution || !card.source.width || !card.source.height) return;
        const auto dimensions = std::to_wstring(card.source.width) + L" × " + std::to_wstring(card.source.height) + L" px";
        const float center = (image.left + image.right) / 2;
        DWRITE_TEXT_METRICS metrics{};
        check(layout(caption.Get(), dimensions, 180, 20)->GetMetrics(&metrics));
        const float half = metrics.width / 2 + 4;
        auto band = RectF(center - half, image.bottom - metrics.height - 4, center + half, image.bottom);
        box(band, 0x202020, .85f, 2);
        text(dimensions, caption.Get(), RectF(band.left + 4, band.top + 2, band.right, band.bottom), 0xffffff);
    }
    void image_body(const application::CardSummary& card, D2D1_RECT_F r, bool lifted) {
        // Leave transparent source pixels clear, including when a shadow is visible.
        const float spread = lifted ? 7.f : 4.5f, dy = lifted ? 4.6f : 2.4f;
        for (int i = 6; i > 0; --i) {
            const float d = spread * i / 6;
            const float alpha = lifted ? .05f : .035f;
            box(RectF(r.left - d / 2, r.top + 2, r.left, r.bottom), 0, alpha);
            box(RectF(r.right, r.top + 2, r.right + d / 2, r.bottom), 0, alpha);
            box(RectF(r.left - d / 2, r.bottom, r.right + d / 2, r.bottom + dy + d), 0, alpha, 2);
        }
        if (card.image_preferences.checkerboard) {
            constexpr float tile = 8;
            box(r, 0xf6f6f6);
            for (int row = 0; r.top + row * tile < r.bottom; ++row)
                for (float x = r.left + (row % 2) * tile; x < r.right; x += 2 * tile)
                    box(RectF(x, r.top + row * tile, std::min(r.right, x + tile),
                              std::min(r.bottom, r.top + (row + 1) * tile)), 0xe2e2e2);
        }
        if (auto* bitmap = preview(card))
            target->DrawBitmap(bitmap, r, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        else {
            box(r, 0xf0eeea);
            notice(CardKind::image, strings().tr(localization::TextKey::card_preview_unavailable), r, skin::muted);
        }
        ink(skin::paper_edge, 0.3f);
        target->DrawRectangle(r, brush.Get(), 0.8f);
    }
    void paper_sheet(const application::CardSummary &card, const ImageCardLayout& geometry, const Placement &p, bool editing, std::wstring_view term = {}, const application::SearchHit* hit = nullptr) {
        const float w = geometry.width, x0 = -w / 2, x1 = w / 2;
        const float top = geometry.top - Hanging::paper_tuck, bottom = geometry.bottom;
        const auto paper = paper_color(card.style.background_color);
        const auto opacity = editing && card.style.opacity == 0 ? 1.f : static_cast<float>(std::clamp(card.style.opacity, 0.0, 1.0));
        if (opacity <= 0)
            return;
        const bool layer = opacity < 1;
        if (layer)
            target->PushLayer(D2D1::LayerParameters(RectF(x0 - 12, top - 2, x1 + 12, bottom + 14), nullptr,
                                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, Matrix3x2F::Identity(),
                                                    opacity),
                              paper_layer.Get());
        shadow(RectF(x0, top, x1, bottom), p.hovered);
        box(RectF(x0, top, x1, bottom), paper);
        const auto seed = seed_of(card.entry.id);
        // Fibers stay clear of an 8-DIP margin so edge pixels keep the exact paper color.
        if (fibers && x1 - x0 > 20) {
            fibers->SetTransform(Matrix3x2F::Translation(float(seed % 96), float((seed >> 8) % 96)));
            target->FillRectangle(RectF(x0 + 8, Hanging::paper_top + 8, x1 - 8, bottom - 8), fibers.Get());
        }
        ink(mix(paper, skin::paper_edge, 0.18f));
        target->DrawRectangle(RectF(x0 + 0.5f, top + 0.5f, x1 - 0.5f, bottom - 0.5f), brush.Get(), 1);
        fade(RectF(x0 + 1, Hanging::paper_top, x1 - 1, Hanging::paper_top + 7), 0x000000, 0.14f, 0);
        const auto area = RectF(x0 + TextInset::side, Hanging::paper_top + TextInset::hanging_top, x1 - TextInset::side, bottom - TextInset::hanging_bottom);
        target->PushAxisAlignedClip(RectF(x0 + 2, Hanging::paper_top, x1 - 2, bottom - 2),
                                    D2D1_ANTIALIAS_MODE_ALIASED);
        if (!editing) content(card, area, paper, term, hit);
        if (!editing && term.empty() && p.hovered && !card.capabilities.empty() && w >= 120) {
            fade(RectF(x0 + 1, bottom - 26, x1 - 1, bottom - 1), paper, 0, 0.96f);
            text(card.capabilities, caption.Get(), RectF(x0 + 11, bottom - 20, x1 - 18, bottom - 6), skin::muted);
        }
        target->PopAxisAlignedClip();
        status(card, Point2F(x1 - 9, bottom - 9));
        if (layer)
            target->PopLayer();
    }
    void wood_bar(const application::CardSummary &card, const domain::Rope &rope, std::size_t index, const ImageCardLayout& g, bool edit_affordance) {
        auto &b = bar(g.hanger_width(), g.bar_bottom);
        const float h = b.width / 2;
        target->FillGeometry(b.profile.Get(), wood_finish(Hanging::bar_top, Hanging::bar_bottom));
        const auto variant = seed_of(card.entry.id) % b.grain.size();
        stroke(b.grain[variant].Get(), skin::wood_grain, 0.7f, 0.22f);
        stroke(b.streaks[variant].Get(), 0x8a5a2b, 0.55f, 0.3f);
        stroke(b.highlight.Get(), skin::wood_light, 1, 0.85f);
        line(Point2F(-h + 2.5f, g.bar_bottom - 0.9f), Point2F(h - 2.5f, g.bar_bottom - 0.9f),
             skin::wood_bottom, 1.1f, 0.75f);
        stroke(b.profile.Get(), skin::wood_edge, 0.9f);
        ink(0x6b4724, 0.55f);
        target->FillEllipse(D2D1::Ellipse(Point2F(0, Hanging::bar_top + 0.9f), 2.3f, 0.95f), brush.Get());
        // Engraved number and type icon. The middle of the bar stays empty.
        const float icon_size = 13, pad = 9;
        const auto icon_box = RectF(h - pad - icon_size, 27.5f, h - pad, 27.5f + icon_size);
        const float room = icon_box.left - 5 - (-h + pad);
        std::wstring number = L"#" + std::to_wstring(index + 1);
        const auto full = number + L"/" + (rope.capacity ? std::to_wstring(*rope.capacity) : L"\u221e");
        if (width_of(full, label.Get()) <= room)
            number = full;
        const auto label_rect = RectF(-h + pad, Hanging::bar_end, -h + pad + room, g.bar_bottom);
        text(number, label.Get(), RectF(label_rect.left, label_rect.top + 0.7f, label_rect.right, label_rect.bottom + 0.7f),
             skin::engraving, 0.55f);
        text(number, label.Get(), label_rect, skin::label);
        if (edit_affordance) {
            pencil(icon_box);
            return;
        }
        const auto kind = icon_kind(card);
        icon(kind, RectF(icon_box.left, icon_box.top + 0.7f, icon_box.right, icon_box.bottom + 0.7f), skin::engraving,
             1.25f);
        icon(kind, icon_box, skin::icon, 1.25f);
    }
};

WorkspacePaint::WorkspacePaint() : device_(std::make_unique<Device>()) {}
WorkspacePaint::~WorkspacePaint() = default;
void WorkspacePaint::release_hanging() {
    device_->faces.clear();
    device_->face_bytes = 0;
    device_->ropes.clear();
}
namespace {
thread_local std::weak_ptr<WorkspacePaint> shared_paint;
}
void WorkspacePaint::use_localization(const localization::LocalizationManager* manager, std::wstring_view ui_family) {
    current_labels = manager;
    chrome_face = ui_family;
    if (auto paint = shared_paint.lock()) {
        auto& device = *paint->device_;
        device.texts.clear();
        device.faces.clear();
        device.face_bytes = 0;
        device.refresh_ui_formats();
    }
}
void WorkspacePaint::discard_target() {
    if (auto paint = shared_paint.lock())
        paint->device_->discard_target();
}
std::uint64_t WorkspacePaint::target_creations() { return created_targets; }
std::shared_ptr<WorkspacePaint> WorkspacePaint::acquire() {
    if (auto existing = shared_paint.lock())
        return existing;
    auto result = std::shared_ptr<WorkspacePaint>(new WorkspacePaint);
    shared_paint = result;
    return result;
}
std::size_t WorkspacePaint::preview_bitmap_bytes() {
    auto shared = shared_paint.lock();
    return shared ? shared->device_->preview_bytes() : 0;
}
std::size_t WorkspacePaint::moving_face_bytes() {
    auto shared = shared_paint.lock();
    return shared ? shared->device_->face_bytes : 0;
}
std::size_t WorkspacePaint::resident_bitmap_bytes() {
    auto shared = shared_paint.lock();
    return shared ? shared->device_->bitmap_bytes() : 0;
}

bool WorkspacePaint::draw(HDC dc, RECT bounds, UINT dpi, std::span<const PaintedRow> rows,
                          const Feedback &feedback) {
    auto &d = *device_;
    auto session = d.begin(dc, bounds, dpi);
    d.target->Clear(ColorF(0, 0));
    d.ensure_fibers(float(dpi));
    const float width = float(bounds.right - bounds.left) * 96 / dpi;
    const presentation::RopeCurve curve(width);
    struct Visible {
        const application::CardSummary *card;
        std::size_t index;
        Device::Placement placement;
        ImageCardLayout layout;
    };
    std::vector<Visible> cards;
    for (const auto &row : rows) {
        const auto &view = *row.view;
        const float paper = view.paper();
        cards.clear();
        auto index = view.first_realized();
        for (const auto &saved : view.cards()) {
            const auto &card = feedback.edit_preview && feedback.edit_preview->entry.id == saved.entry.id
                ? *feedback.edit_preview : saved;
            const auto pos = row.preview.position(index++, row.rope->slot_limit());
            if (!pos)
                continue;
            const auto g = view.geometry(index - 1);
            const auto x = view.projected_left(*pos, row.preview);
            const float pad = g.carried_padding();
            if (x + g.width + pad > 0 && x - pad < width && card.entry.id != feedback.dragged)
                cards.push_back({&card, *pos, d.place(card, x, row.top, g.width, curve, feedback), g});
        }
        for (const auto &c : cards)
            d.hook_behind(c.placement);
        d.target->SetTransform(Matrix3x2F::Identity());
        const auto &material =
            feedback.colors && feedback.colors->rope && feedback.colors->rope->rope == row.rope->id
                ? feedback.colors->rope->material
                : row.rope->material;
        const auto &art = d.rope_art(curve, material, float(dpi));
        d.target->DrawBitmap(art.bitmap.Get(), RectF(0, row.top + art.top, width, row.top + art.top + art.height), 1,
                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        d.rope_plaque(curve, row.top, row.rope->name, feedback.rope_count);
        if (feedback.moving_rope == row.rope->id) {
            for (float x : {8.f, width - 48.f})
                d.line(Point2F(x, row.top + curve.y(x)), Point2F(x + 40, row.top + curve.y(x + 40)), skin::drop, 3);
        }
        if (row.preview.gap) {
            const float hook = view.projected_left(*row.preview.gap, row.preview) + (row.preview.gap_width > 0 ? row.preview.gap_width : paper) / 2;
            d.ink(skin::drop);
            d.target->DrawEllipse(D2D1::Ellipse(Point2F(hook, row.top + curve.contact(hook) + Hanging::pivot),
                                                7, 7), d.brush.Get(), 1.6f);
        }
        for (const auto &c : cards) {
            if (feedback.find_term.empty() && feedback.swing && feedback.swing->moving() && !c.placement.hovered &&
                feedback.editing != c.card->entry.id && c.card != feedback.edit_preview &&
                !(feedback.colors && feedback.colors->card && feedback.colors->card->entry == c.card->entry.id))
                d.moving_face(*c.card, *row.rope, c.index, c.layout, dpi, c.placement);
            else
                d.hanging(*c.card, *row.rope, c.index, c.layout, c.placement, feedback);
        }
        d.target->SetTransform(Matrix3x2F::Identity());
    }
    return session.finish();
}

bool WorkspacePaint::draw_carried(HDC dc, RECT bounds, UINT dpi, const application::CardSummary &card,
                                  const domain::Rope &rope, std::size_t index, const ImageCardLayout& geometry,
                                  presentation::CardFrame frame, const presentation::ColorPreview* colors) {
    auto &d = *device_;
    auto session = d.begin(dc, bounds, dpi);
    d.target->Clear(ColorF(0, 0));
    d.ensure_fibers(float(dpi));
    const Device::Placement placement{Matrix3x2F::Rotation(frame.degrees, Point2F(0, Hanging::pivot)) *
                                      Matrix3x2F::Translation(frame.hook_x, frame.rope)};
    d.hook_behind(placement);
    presentation::PreviewCard face(card, colors);
    d.moving_face(face.get(), rope, index, geometry, dpi, placement);
    return session.finish();
}

HBRUSH WorkspacePaint::editor_paper(HWND owner, UINT dpi, domain::ColorSelection background, std::string_view id,
                                    float x, float y, bool floating) {
    const int side = MulDiv(96, dpi, 96);
    HDC screen = GetDC(owner);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP tile = CreateCompatibleBitmap(screen, side, side);
    ReleaseDC(owner, screen);
    auto previous = SelectObject(dc, tile);
    HBRUSH brush{};
    try {
        if (previous) {
            auto& d = *device_;
            auto session = d.begin(dc, {0, 0, side, side}, dpi);
            d.target->Clear(ColorF(paper_color(background)));
            d.ensure_fibers(float(dpi));
            const auto seed = seed_of(id);
            d.fibers->SetTransform(Matrix3x2F::Translation(float(seed % 96) - x,
                (floating ? 0.f : float((seed >> 8) % 96)) - y));
            d.target->FillRectangle(RectF(0, 0, 96, 96), d.fibers.Get());
            if (session.finish()) brush = CreatePatternBrush(tile);
        }
    } catch (...) {
        // Retain a solid paper backing if the graphics device is being recreated.
    }
    if (previous) SelectObject(dc, previous);
    DeleteObject(tile);
    DeleteDC(dc);
    return brush;
}

bool WorkspacePaint::draw_floating(HDC dc, RECT bounds, UINT dpi, const application::CardSummary &source,
                                   FloatingControl hover, bool editing, const presentation::ColorPreview* colors) {
    presentation::PreviewCard preview(source, colors);
    const auto &card = preview.get();
    auto &d = *device_;
    auto session = d.begin(dc, bounds, dpi);
    const auto paper = paper_color(card.style.background_color);
    const auto g = ImageCardLayout::measure(card);
    d.target->Clear(g.adaptive ? ColorF(0, 0) : ColorF(paper));
    d.ensure_fibers(float(dpi));
    const float w = float(bounds.right - bounds.left) * 96 / dpi, h = float(bounds.bottom - bounds.top) * 96 / dpi;
    if (g.adaptive) {
        const auto image = g.fitted_image(w, h);
        const float x = (w - image.width) / 2;
        const auto rect = RectF(x, g.toolbar, x + image.width, g.toolbar + image.height);
        d.image_body(card, rect, false);
        if (hover == FloatingControl::body) d.resolution(card, rect);
        d.box(RectF(0, 0, w, g.toolbar), skin::warm_paper);
    } else {
        if (d.fibers && w > 24 && h > 60) {
            d.fibers->SetTransform(Matrix3x2F::Translation(float(seed_of(card.entry.id) % 96), 0));
            d.target->FillRectangle(RectF(8, 46, w - 8, h - 8), d.fibers.Get());
        }
        d.ink(mix(paper, skin::paper_edge, 0.24f));
        d.target->DrawRectangle(RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), d.brush.Get(), 1);
        d.line(Point2F(10, 38.5f), Point2F(w - 10, 38.5f), mix(paper, skin::paper_edge, 0.12f), 1);
        if (w >= 200) {
            // A brass pushpin holds the sheet to the desktop.
            const auto pin = Point2F(w / 2, 13);
            d.ink(0x000000, 0.18f);
            d.target->FillEllipse(D2D1::Ellipse(Point2F(pin.x + 1.4f, pin.y + 2.2f), 5.2f, 5.2f), d.brush.Get());
            d.ink(0xb7473b);
            d.target->FillEllipse(D2D1::Ellipse(pin, 5.2f, 5.2f), d.brush.Get());
            d.ink(0xe58b7f, 0.9f);
            d.target->FillEllipse(D2D1::Ellipse(Point2F(pin.x - 1.6f, pin.y - 1.7f), 1.8f, 1.6f), d.brush.Get());
            d.ink(0x7d2a22, 0.8f);
            d.target->DrawEllipse(D2D1::Ellipse(pin, 5.2f, 5.2f), d.brush.Get(), 0.8f);
        }
    }
    const bool wide = w >= 200;
    const auto pill = RectF(8, 7, return_control_right(w), 31);
    d.box(pill, mix(paper, 0x8a6a40, hover == FloatingControl::restore ? 0.17f : 0.09f), 1, 12);
    d.ink(mix(paper, skin::paper_edge, 0.24f));
    d.target->DrawRoundedRectangle(D2D1::RoundedRect(pill, 12, 12), d.brush.Get(), 1);
    d.glyph(d.return_icon.Get(), RectF(16, 12.5f, 28, 24.5f), skin::label, 1.5f);
    if (wide)
        d.text(strings().tr(localization::TextKey::card_return), d.control.Get(), RectF(32, 7, pill.right - 6, 31), skin::label);
    const auto type_icon = RectF(w - 52, 12.5f, w - 39, 25.5f);
    if (!editing && hover != FloatingControl::none && card.menu == application::CardMenuKind::text)
        d.pencil(type_icon);
    else d.icon(d.icon_kind(card), type_icon, skin::icon, 1.25f);
    d.close_mark(Point2F(w - 19, 19), false, hover == FloatingControl::close, paper);
    if (g.adaptive) return session.finish();
    d.target->PushAxisAlignedClip(RectF(2, 40, w - 2, h - 2), D2D1_ANTIALIAS_MODE_ALIASED);
    if (!editing) d.content(card, RectF(TextInset::side, TextInset::floating_top, w - TextInset::side, h - TextInset::floating_bottom), paper);
    d.target->PopAxisAlignedClip();
    d.status(card, Point2F(w - 9, h - 9));
    return session.finish();
}

void set_translucent(HWND hwnd, bool enabled) {
    auto style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (bool(style & WS_EX_LAYERED) == enabled)
        return;
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowLongPtrW(hwnd, GWL_EXSTYLE, enabled ? style | WS_EX_LAYERED : style & ~WS_EX_LAYERED) &&
        GetLastError())
        throw std::runtime_error("Unable to change window composition");
}

namespace {
// Keep common shapes in one DIB. A larger real surface can still paint at full resolution.
constexpr std::uint64_t retained_frame_bytes = 8ull * 1024 * 1024;
std::uint64_t frame_bytes(LONG width, LONG height) { return std::uint64_t(width) * height * 4; }
struct Frame {
    HDC dc{};
    HBITMAP image{};
    HGDIOBJ previous{};
    BYTE *bits{};
    LONG width{}, height{};
    unsigned allocations{};
    unsigned users{};
    bool trim_pending{};
    ~Frame() { release(); }
    void trim() {
        if (frame_bytes(width, height) <= retained_frame_bytes) return;
        if (users) trim_pending = true;
        else release();
    }
    void release() {
        trim_pending = false;
        if (previous)
            SelectObject(dc, previous);
        if (image)
            DeleteObject(image);
        if (dc)
            DeleteDC(dc);
        dc = nullptr;
        image = nullptr;
        previous = nullptr;
        bits = nullptr;
        width = height = 0;
    }
    void prepare(LONG w, LONG h, FrameCoverage coverage = FrameCoverage::partial) {
        if (!dc || w > width || h > height) {
            LONG next_w = std::max(w, width), next_h = std::max(h, height);
            if (frame_bytes(next_w, next_h) > retained_frame_bytes) {
                next_w = w;
                next_h = h;
            }
            ++allocations;
            release();
            dc = CreateCompatibleDC(nullptr);
            if (!dc)
                throw std::runtime_error("Frame surface unavailable");
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = next_w;
            info.bmiHeader.biHeight = -next_h;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biPlanes = 1;
            void *raw{};
            image = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
            if (!image || !raw) {
                release();
                throw std::runtime_error("Frame bitmap unavailable");
            }
            bits = static_cast<BYTE *>(raw);
            previous = SelectObject(dc, image);
            width = next_w;
            height = next_h;
        }
        GdiFlush();
        if (coverage == FrameCoverage::full) return;
        for (LONG y = 0; y < h; ++y)
            std::fill_n(bits + static_cast<std::size_t>(y) * width * 4, static_cast<std::size_t>(w) * 4, BYTE{0});
    }
};
thread_local Frame frame;
struct FrameUse {
    FrameUse() { ++frame.users; }
    ~FrameUse() { if (!--frame.users && frame.trim_pending) frame.trim(); }
};
} // namespace
void trim_frame_after_hide() { frame.trim(); }
#ifdef CLIPROPE_DRAW_PROBES
FrameBacking frame_backing() { return {frame.width,frame.height,frame.allocations}; }
#endif

bool paint_window(HWND hwnd, HDC dc, RECT bounds, std::span<const AlphaArea> areas,
                  const std::function<bool(HDC)> &draw, POINT *screen_origin, FrameCoverage coverage) {
    if (bounds.right <= 0 || bounds.bottom <= 0)
        return true;
    FrameUse use;
    frame.prepare(bounds.right, bounds.bottom, coverage);
    if (!draw(frame.dc))
        return false;
    GdiFlush();
    if (!(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED)) {
        if (!BitBlt(dc, 0, 0, bounds.right, bounds.bottom, frame.dc, 0, 0, SRCCOPY))
            throw std::runtime_error("Could not present card");
        return true;
    }
    for (const auto &patch : areas) {
        RECT r{};
        if (!IntersectRect(&r, &patch.rect, &bounds))
            continue;
        const auto scale = static_cast<unsigned>(std::lround(std::clamp(patch.opacity, 0., 1.) * 255));
        for (LONG y = r.top; y < r.bottom; ++y) {
            auto *pixel = frame.bits + (static_cast<std::size_t>(y) * frame.width + r.left) * 4;
            for (LONG x = r.left; x < r.right; ++x, pixel += 4)
                for (int c = 0; c < 4; ++c)
                    pixel[c] = static_cast<BYTE>((pixel[c] * scale + 127) / 255);
        }
    }
    SIZE size{bounds.right, bounds.bottom};
    POINT origin{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
#ifdef CLIPROPE_DRAW_PROBES
    if (draw_fault == DrawFault::present_layer) throw std::runtime_error("Injected layer presentation failure");
#endif
    if (!UpdateLayeredWindow(hwnd, nullptr, screen_origin, &size, frame.dc, &origin, 0, &blend, ULW_ALPHA))
        throw std::runtime_error("Could not present translucent card");
    return true;
}

bool print_window(HDC dc, RECT bounds, const std::function<bool(HDC)> &draw) {
    if (bounds.right <= 0 || bounds.bottom <= 0)
        return true;
    FrameUse use;
    frame.prepare(bounds.right, bounds.bottom);
    if (!draw(frame.dc))
        return false;
    GdiFlush();
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    return AlphaBlend(dc, 0, 0, bounds.right, bounds.bottom, frame.dc, 0, 0, bounds.right, bounds.bottom, blend) !=
           FALSE;
}
} // namespace cliprope::rendering
