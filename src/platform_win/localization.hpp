#pragma once
#include "localization/localization.hpp"
#include <memory>
#include <variant>
#include <vector>
#include <windows.h>

namespace cliprope::win {
using localization::TextKey;
inline constexpr UINT language_changed = WM_APP + 0x48e;
localization::Locale preferred_ui_locale() noexcept;
const wchar_t* ui_font_family() noexcept;
void invalidate_ui_fonts() noexcept;
// The entry point supplies the desktop's manager for this UI thread. Standalone
// component fixtures retain deterministic English unless they supply a binding.
class UiLocalization final {
  public:
    explicit UiLocalization(localization::LocalizationManager& manager) noexcept;
    ~UiLocalization();
    UiLocalization(const UiLocalization&) = delete;
    UiLocalization& operator=(const UiLocalization&) = delete;
  private:
    const localization::LocalizationManager* previous_;
};
const localization::LocalizationManager& ui_localization() noexcept;
const wchar_t* ui_text(localization::TextKey key) noexcept;
std::wstring ui_format(localization::TextKey key, std::initializer_list<localization::Argument> args);
void notify_language_changed() noexcept;

// Resource identity is explicit: plain user strings never become translations.
struct UiArgument;
class UiText final {
  public:
    UiText() = default;
    UiText(const wchar_t* value) : value_(std::wstring(value ? value : L"")) {}
    UiText(std::wstring value) : value_(std::move(value)) {}
    UiText(std::wstring_view value) : value_(std::wstring(value)) {}
    UiText(TextKey key) : value_(key) {}
    static UiText message(TextKey key, std::initializer_list<UiArgument> args);
    std::wstring_view view() const;
    const wchar_t* c_str() const { return view().data(); }
    const wchar_t* data() const { return c_str(); }
    size_t size() const { return view().size(); }
    auto begin() const { return view().begin(); }
    auto end() const { return view().end(); }
    bool empty() const { return view().empty(); }
    bool localized() const noexcept { return value_.index() != 0; }
    void clear() { value_ = std::wstring{}; }
    UiText display(bool remove_mnemonics, bool remove_ellipsis = false) const;
    UiText native_literal() const;
    operator std::wstring_view() const { return view(); }
    operator std::wstring() const { return std::wstring(view()); }
    UiText& operator+=(UiText other);
    friend UiText operator+(UiText left, UiText right);
    friend bool operator==(const UiText& a, const UiText& b) { return a.view() == b.view(); }
  private:
    struct Expression;
    std::variant<std::wstring, TextKey, std::shared_ptr<Expression>> value_;
};
inline UiText tr(TextKey key) { return UiText(key); }
struct UiArgument { std::string_view name; UiText value; };
inline UiText message(TextKey key, std::initializer_list<UiArgument> args) { return UiText::message(key, args); }
void bind_ui_text(HWND window, UiText text);
// Retain a bound label's resource identity when decorating it; unbound text stays literal.
UiText ui_caption(HWND window);
bool set_ui_text(HWND window, UiText text);
bool set_ui_item_text(HWND window, int id, UiText text);
HWND create_ui_window(DWORD extended, const wchar_t* type, UiText title, DWORD style,
                      int x, int y, int width, int height, HWND owner, HMENU menu, HINSTANCE instance, void* data);
void bind_ui_accessibility(HWND window, UiText text, bool description);
void refresh_ui_bindings(HWND root);
} // namespace cliprope::win
