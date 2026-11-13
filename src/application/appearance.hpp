#pragma once
#include "domain/color.hpp"
namespace cliprope::application {
enum class Theme { light, dark, system };
struct Appearance {
    Theme theme = Theme::light;
    domain::ColorSelection match_color;
    unsigned match_rgb() const { return match_color.resolve(domain::match_default).rgb(); }
    bool operator==(const Appearance&) const = default;
};
} // namespace cliprope::application
