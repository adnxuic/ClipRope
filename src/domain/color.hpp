#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cliprope::domain {
struct ColorRGBA8 {
    std::uint8_t r{}, g{}, b{}, a{255};
    static constexpr ColorRGBA8 rgb(std::uint32_t value, std::uint8_t alpha = 255) {
        return {std::uint8_t(value >> 16), std::uint8_t(value >> 8), std::uint8_t(value), alpha};
    }
    constexpr std::uint32_t rgb() const { return (std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | b; }
    bool operator==(const ColorRGBA8 &) const = default;
};
inline std::string color_hex(ColorRGBA8 color, bool alpha = false) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string out = "#";
    for (auto c : {color.r, color.g, color.b, color.a}) {
        if (!alpha && out.size() == 7)
            break;
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}
inline std::optional<ColorRGBA8> parse_color(std::string_view text, bool alpha = true) {
    if (text.size() != 7 && !(alpha && text.size() == 9))
        return {};
    if (text.front() != '#')
        return {};
    std::uint32_t value = 0;
    for (char c : text.substr(1)) {
        unsigned n = c >= '0' && c <= '9'   ? c - '0'
                     : c >= 'a' && c <= 'f' ? c - 'a' + 10
                     : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                            : 16;
        if (n > 15)
            return {};
        value = (value << 4) | n;
    }
    return text.size() == 7 ? ColorRGBA8::rgb(value) : ColorRGBA8::rgb(value >> 8, std::uint8_t(value));
}
struct ColorSelection {
    std::optional<ColorRGBA8> custom;
    ColorSelection() = default;
    ColorSelection(std::uint32_t rgb) : custom(ColorRGBA8::rgb(rgb)) {
        if (rgb > 0xffffff) throw std::invalid_argument("RGB exceeds 24 bits");
    }
    ColorSelection(ColorRGBA8 rgba) : custom(rgba) {}
    bool is_default() const { return !custom; }
    ColorRGBA8 resolve(std::uint32_t fallback) const { return custom.value_or(ColorRGBA8::rgb(fallback)); }
    std::string encode() const { return custom ? color_hex(*custom, true) : "default"; }
    static ColorSelection decode(std::string_view text) {
        if (text == "default")
            return {};
        if (auto c = parse_color(text))
            return *c;
        throw std::invalid_argument("Invalid stored color");
    }
    bool operator==(const ColorSelection &) const = default;
};
inline constexpr unsigned paper_default = 0xfbf7ef, ink_default = 0, match_default = 0xe5a85e,
                          rope_default = 0xc9a26b;
inline unsigned composite(ColorRGBA8 color, unsigned background) {
    auto base = ColorRGBA8::rgb(background);
    auto blend = [&](unsigned c, unsigned b) {
        return std::uint8_t((c * color.a + b * (255 - color.a) + 127) / 255);
    };
    return ColorRGBA8{blend(color.r, base.r), blend(color.g, base.g), blend(color.b, base.b), 255}.rgb();
}
struct ColorHSV {
    double h{}, s{}, v{};
    static ColorHSV from(ColorRGBA8 c, double previous_hue = 0) {
        double r = c.r / 255., g = c.g / 255., b = c.b / 255.;
        double hi = std::max({r, g, b}), lo = std::min({r, g, b}), d = hi - lo, h = previous_hue;
        if (d > 0) {
            h = hi == r   ? 60 * std::fmod((g - b) / d, 6.)
                : hi == g ? 60 * ((b - r) / d + 2)
                          : 60 * ((r - g) / d + 4);
            if (h < 0)
                h += 360;
        }
        return {h, hi == 0 ? 0 : d / hi, hi};
    }
    ColorRGBA8 rgba(std::uint8_t alpha = 255) const {
        const double hue = std::fmod(std::fmod(h, 360.) + 360., 360.) / 60;
        const double value = std::clamp(v, 0., 1.), c = value * std::clamp(s, 0., 1.),
                     x = c * (1 - std::abs(std::fmod(hue, 2.) - 1)), m = value - c;
        std::array<double, 3> rgb = hue < 1   ? std::array{c, x, 0.}
                                    : hue < 2 ? std::array{x, c, 0.}
                                    : hue < 3 ? std::array{0., c, x}
                                    : hue < 4 ? std::array{0., x, c}
                                    : hue < 5 ? std::array{x, 0., c}
                                              : std::array{c, 0., x};
        return {std::uint8_t(std::lround((rgb[0] + m) * 255)), std::uint8_t(std::lround((rgb[1] + m) * 255)),
                std::uint8_t(std::lround((rgb[2] + m) * 255)), alpha};
    }
};
inline double contrast(unsigned first, unsigned second) {
    auto luminance = [](unsigned rgb) {
        auto c = ColorRGBA8::rgb(rgb);
        auto linear = [](double v) {
            v /= 255;
            return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4);
        };
        return .2126 * linear(c.r) + .7152 * linear(c.g) + .0722 * linear(c.b);
    };
    auto a = luminance(first), b = luminance(second);
    return (std::max(a, b) + .05) / (std::min(a, b) + .05);
}
struct RopeMaterial {
    bool natural = true;
    std::array<unsigned, 5> tones{0x6f5230, 0x9a7447, rope_default, 0xddbe8a, 0xf0ddb4};
    unsigned seed = 0xc0ffee;
    static RopeMaterial tinted(unsigned rgb) {
        auto mix = [](unsigned a, unsigned b, float t) {
            auto c = ColorRGBA8::rgb(a), d = ColorRGBA8::rgb(b);
            auto channel = [&](unsigned x, unsigned y) {
                float u = x / 255.f, v = y / 255.f;
                return unsigned(std::lround(std::clamp(u + (v - u) * t, 0.f, 1.f) * 255));
            };
            return (channel(c.r, d.r) << 16) | (channel(c.g, d.g) << 8) | channel(c.b, d.b);
        };
        return {false,
                {mix(rgb, 0x140c04, .55f), mix(rgb, 0x1e140a, .32f), rgb, mix(rgb, 0xffffff, .3f),
                 mix(rgb, 0xfff6e0, .55f)},
                0xc0ffeeu ^ rgb};
    }
    static RopeMaterial from(ColorSelection selection) {
        return selection.is_default() ? RopeMaterial{} : tinted(selection.custom->rgb());
    }
    bool operator==(const RopeMaterial &) const = default;
};
} // namespace cliprope::domain
