#include "localization/localization.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

using namespace cliprope::localization;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Locale match(std::initializer_list<std::wstring> values) {
    return match_preferred_languages(std::span<const std::wstring>(values.begin(), values.size()));
}
}
int main() {
    try {
        require(match({L"fr-FR", L"zh-CN", L"en-US"}) == Locale::zh_cn, "Preference order ignored");
        require(match({L"en-GB", L"zh-CN"}) == Locale::en_us, "English variant rejected");
        require(match({L"zh-TW", L"zh-SG"}) == Locale::zh_cn, "Unsupported language stopped the search");
        require(match({L"ZH-hANS-HK"}) == Locale::zh_cn, "Explicit script did not override region");
        require(match({L"zh-Hant-CN", L"ja-JP"}) == Locale::en_us, "Traditional Chinese matched Simplified");
        require(match({L"zh-HansFake", L"zh", L"zh-HK"}) == Locale::en_us, "Invalid or unsupported tag matched");
        require(match({}) == Locale::en_us, "Empty list did not fall back");
        require(parse_language("unknown") == Language::automatic, "Unknown preference did not default");
        for (const auto value : {Language::automatic, Language::english, Language::simplified_chinese})
            require(parse_language(language_code(value)) == value, "Language code failed to round-trip");
        LocalizationManager manager;
        const auto original = manager.tr(TextKey::common_cancel);
        require(!manager.set_language(Language::english, Locale::zh_cn) && !manager.revision(), "Override generated a false event");
        require(manager.set_language(Language::simplified_chinese) && manager.revision() == 1, "Language event missing");
        require(manager.tr(TextKey::common_cancel) == L"取消" && original == L"Cancel", "Translation or view lifetime changed");
        require(manager.format(TextKey::rope_card_count_other, {{"count", L"12"}}) == L"12 张卡片", "Named substitution failed");
        require(manager.format(TextKey::rope_card_count_other, {}) == L"[rope.card_count.other]", "Missing parameter was exposed");
        require(manager.set_language(Language::automatic, Locale::en_us), "Auto selection not recomputed");
        const std::array<Translation, 1> incomplete{{{"test.value", L"{{{value}}}", L"", L"[test.value]"}}};
        unsigned reports{};
        LocalizationManager fallback(incomplete, [&](auto) { ++reports; });
        fallback.set_language(Language::simplified_chinese);
        const auto key = static_cast<TextKey>(0);
        require(fallback.format(key, {{"value", L"kept"}}) == L"{kept}", "English fallback or escaped braces failed");
        require(fallback.format(key, {{"value", L"a"}, {"value", L"b"}}) == L"[test.value]" && reports, "Duplicate parameters were accepted");
        const std::array<Translation, 1> broken{{{"test.value", L"{value}", L"{wrong}", L"[test.value]"}}};
        LocalizationManager defensive(broken);
        defensive.set_language(Language::simplified_chinese);
        require(defensive.format(key, {{"value", L"English"}}) == L"English", "Invalid translation did not fall back");
        require(defensive.tr(static_cast<TextKey>(99999)) == L"[unknown]", "Invalid key was unsafe");
        std::cout << "Language matching, static lifetime, revisions and formatting passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
