#pragma once

namespace cliprope::application {
struct CardPreferences {
    bool paste_on_double_click = true;
    double font_size = 14;
    bool hover_resolution = true;
    bool image_file_previews = true;
    bool keep_expanded_group = true;
    bool screenshot_hover_preview = true;
    bool follow_card_head = true;
    bool follow_collector = true;
    bool recycle_hanging = false;
    bool recycle_floating = true;
    bool operator==(const CardPreferences&) const = default;
};
}
