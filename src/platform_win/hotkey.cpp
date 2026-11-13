#include "platform_win/hotkey.hpp"
#include <charconv>

namespace cliprope::win {
namespace {
std::string key_name(UINT key) {
    if (key == VK_TAB) return "Tab";
    if (key == VK_SPACE) return "Space";
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
        return std::string(1, static_cast<char>(key));
    if (key >= VK_F1 && key <= VK_F24)
        return "F" + std::to_string(key - VK_F1 + 1);
    return {};
}
} // namespace
std::string hotkey_name(Hotkey key) {
    if (key.modifiers & ~(MOD_CONTROL | MOD_ALT | MOD_SHIFT))
        return {};
    const auto name = key_name(key.key);
    if (name.empty())
        return {};
    std::string out;
    if (key.modifiers & MOD_CONTROL)
        out += "Ctrl+";
    if (key.modifiers & MOD_ALT)
        out += "Alt+";
    if (key.modifiers & MOD_SHIFT)
        out += "Shift+";
    return out + name;
}
std::optional<Hotkey> parse_hotkey(std::string_view text) {
    Hotkey result{};
    for (auto split = text.find('+'); split != text.npos; split = text.find('+')) {
        auto flag = text.substr(0, split);
        UINT value = flag == "Ctrl" ? MOD_CONTROL : flag == "Shift" ? MOD_SHIFT : flag == "Alt" ? MOD_ALT : 0;
        if (!value || (result.modifiers & value))
            return {};
        result.modifiers |= value;
        text.remove_prefix(split + 1);
    }
    if (text.size() == 1 && ((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= '0' && text[0] <= '9')))
        result.key = static_cast<unsigned char>(text.front());
    else if (text == "Space") result.key = VK_SPACE;
    else if (text == "Tab") result.key = VK_TAB;
    else if (text.starts_with('F')) {
        int number = 0;
        auto parsed = std::from_chars(text.data() + 1, text.data() + text.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || number < 1 || number > 24)
            return {};
        result.key = VK_F1 + number - 1;
    }
    if (hotkey_name(result).empty())
        return {};
    return result;
}

bool valid_hotkey(Hotkey chord, bool global) {
    if (hotkey_name(chord).empty()) return false;
    const bool function = chord.key >= VK_F1 && chord.key <= VK_F24;
    if (!chord.modifiers && (global || (chord.key != VK_TAB && !function))) return false;
    if (chord.key == VK_TAB && (chord.modifiers & MOD_ALT)) return false;
    return !global || chord.key != VK_F12;
}
}
