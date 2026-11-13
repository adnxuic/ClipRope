#pragma once

namespace cliprope::win::fluent {
// Segoe Fluent Icons names are shared by every native surface. Keep code points
// here; labels and user content must never be used to identify an icon.
namespace icon_values {
enum Icon : wchar_t {
    none = 0,
    chevron_down = 0xe70d, chevron_up = 0xe70e, edit = 0xe70f,
    add = 0xe710, close = 0xe711, settings = 0xe713, link = 0xe71b,
    search = 0xe721, refresh = 0xe72c, lock = 0xe72e, remove = 0xe738,
    check = 0xe73e, up = 0xe74a, down = 0xe74b, erase = 0xe74d,
    keyboard = 0xe765, appearance = 0xe790, opacity = 0xe793,
    rebin = 0xe7b8, exit = 0xe7e8, overview = 0xe80a, first = 0xe80f,
    clear = 0xe894, document = 0xe8a5, copy = 0xe8a7, density = 0xe8a9,
    save_as = 0xe8b3, folder = 0xe8b7, ink = 0xe8d3, bold = 0xe8dd,
    font_size = 0xe8e9, rope = 0xe8fd, information = 0xe946,
    general = 0xe77f
};
}
using Icon = icon_values::Icon;
} // namespace cliprope::win::fluent
