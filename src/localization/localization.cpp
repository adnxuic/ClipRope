#include "localization/localization.hpp"
#include <algorithm>
#include <array>
#include <optional>

namespace cliprope::localization {
std::string_view language_code(Language value) noexcept {
    switch (value) {
    case Language::english: return "en-US";
    case Language::simplified_chinese: return "zh-CN";
    default: return "auto";
    }
}
Language parse_language(std::string_view value) noexcept {
    if (value == "en-US") return Language::english;
    if (value == "zh-CN") return Language::simplified_chinese;
    return Language::automatic;
}
std::wstring_view locale_name(Locale value) noexcept { return value == Locale::zh_cn ? L"zh-CN" : L"en-US"; }
namespace {
bool same(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}
std::optional<Locale> match(std::wstring_view tag) noexcept {
    auto part = [&]() {
        const auto end = tag.find(L'-');
        const auto result = tag.substr(0, end);
        tag = end == tag.npos ? std::wstring_view{} : tag.substr(end + 1);
        return result;
    };
    const auto language = part();
    if (same(language, L"en")) return Locale::en_us;
    if (!same(language, L"zh")) return {};
    std::wstring_view script, region;
    while (!tag.empty()) {
        const auto token = part();
        if (token.size() == 1) break; // Extension subtags cannot change the script or region.
        if (token.size() == 4 && script.empty()) script = token;
        else if ((token.size() == 2 || token.size() == 3) && region.empty()) region = token;
    }
    if (!script.empty()) return same(script, L"Hans") ? std::optional(Locale::zh_cn) : std::nullopt;
    if (same(region, L"CN") || same(region, L"SG")) return Locale::zh_cn;
    return {};
}
bool render(std::wstring_view pattern, std::span<const Argument> args, std::wstring& output) {
    for (size_t i = 0; i < pattern.size();) {
        const auto c = pattern[i++];
        if (c != L'{' && c != L'}') { output += c; continue; }
        if (i < pattern.size() && pattern[i] == c) { output += c; ++i; continue; }
        if (c == L'}') return false;
        const auto end = pattern.find(L'}', i);
        if (end == pattern.npos || end == i) return false;
        const auto name = pattern.substr(i, end - i);
        auto found = args.end();
        for (auto it = args.begin(); it != args.end(); ++it) {
            if (it->name.size() != name.size() || !std::equal(name.begin(), name.end(), it->name.begin())) continue;
            if (found != args.end()) return false;
            found = it;
        }
        if (found == args.end()) return false;
        output += found->value;
        i = end + 1;
    }
    return true;
}
}
Locale match_preferred_languages(std::span<const std::wstring> preferred) noexcept {
    for (const auto& tag : preferred) if (const auto locale = match(tag)) return *locale;
    return Locale::en_us;
}
LocalizationManager::LocalizationManager(std::span<const Translation> catalog, Diagnostic diagnostic)
    : catalog_(catalog), diagnostic_(std::move(diagnostic)) {}
bool LocalizationManager::set_language(Language selected, Locale automatic_locale) noexcept {
    selected_ = selected;
    const auto next = selected == Language::english ? Locale::en_us :
        selected == Language::simplified_chinese ? Locale::zh_cn : automatic_locale;
    if (next == effective_) return false;
    effective_ = next;
    ++revision_;
    return true;
}
void LocalizationManager::report(std::string_view message) const noexcept {
    try { if (diagnostic_) diagnostic_(message); } catch (...) {}
}
std::wstring_view LocalizationManager::tr(TextKey key) const noexcept {
    const auto index = static_cast<size_t>(key);
    if (index >= catalog_.size()) { report("Unknown translation key"); return L"[unknown]"; }
    const auto& entry = catalog_[index];
    if (effective_ == Locale::zh_cn && !entry.chinese.empty()) return entry.chinese;
    if (!entry.english.empty()) return entry.english;
    report(entry.key);
    return entry.missing;
}
std::wstring LocalizationManager::format(TextKey key, std::span<const Argument> args) const {
    std::wstring output;
    if (render(tr(key), args, output)) return output;
    const auto index = static_cast<size_t>(key);
    if (index >= catalog_.size()) return L"[unknown]";
    const auto& entry = catalog_[index];
    output.clear();
    report(entry.key);
    if (!entry.english.empty() && render(entry.english, args, output)) return output;
    return std::wstring(entry.missing);
}
} // namespace cliprope::localization
