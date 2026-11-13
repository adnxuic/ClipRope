#pragma once
#include "translation_keys.hpp"
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>

namespace cliprope::localization {
enum class Language { automatic, english, simplified_chinese };
enum class Locale { en_us, zh_cn };
struct Argument {
    std::string_view name;
    std::wstring value;
};
struct Translation {
    std::string_view key;
    std::wstring_view english, chinese, missing;
};
std::span<const Translation> builtin_catalog() noexcept;
std::string_view language_code(Language value) noexcept;
Language parse_language(std::string_view value) noexcept;
std::wstring_view locale_name(Locale value) noexcept;
Locale match_preferred_languages(std::span<const std::wstring> preferred) noexcept;

// Owned by the desktop host. Selection and event dispatch belong to its UI thread.
class LocalizationManager final {
  public:
    using Diagnostic = std::function<void(std::string_view)>;
    explicit LocalizationManager(std::span<const Translation> catalog = builtin_catalog(), Diagnostic diagnostic = {});
    bool set_language(Language selected, Locale automatic_locale = Locale::en_us) noexcept;
    Language selected_language() const noexcept { return selected_; }
    Locale effective_locale() const noexcept { return effective_; }
    std::uint64_t revision() const noexcept { return revision_; }
    std::wstring_view tr(TextKey key) const noexcept;
    std::wstring format(TextKey key, std::span<const Argument> arguments) const;
    std::wstring format(TextKey key, std::initializer_list<Argument> arguments) const {
        return format(key, std::span<const Argument>(arguments.begin(), arguments.size()));
    }

  private:
    void report(std::string_view message) const noexcept;
    std::span<const Translation> catalog_;
    Diagnostic diagnostic_;
    Language selected_ = Language::automatic;
    Locale effective_ = Locale::en_us;
    std::uint64_t revision_{};
};
} // namespace cliprope::localization
