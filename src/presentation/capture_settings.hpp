#pragma once
#include "application/capture.hpp"
#include <string_view>

namespace cliprope::presentation {
inline application::CapturePolicy capture_settings(bool paused, std::string_view lines) {
    application::CapturePolicy policy{paused, {}};
    while (!lines.empty()) {
        const auto end = lines.find_first_of("\r\n");
        const auto name = lines.substr(0, end);
        if (!name.empty())
            policy.excluded_apps.emplace_back(name);
        if (end == lines.npos)
            break;
        lines.remove_prefix(end + 1);
    }
    return policy;
}
} // namespace cliprope::presentation
