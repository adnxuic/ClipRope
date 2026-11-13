#include "platform_win/localization.hpp"
#include "platform_win/fluent.hpp"
#include <commctrl.h>
#include <algorithm>
#include <stdexcept>
#include <array>
#include <vector>

namespace cliprope::win {
namespace { thread_local const localization::LocalizationManager* current{}; }
localization::Locale preferred_ui_locale() noexcept {
    try {
#ifdef CLIPROPE_DESKTOP_PROBES
        wchar_t forced[16]{};
        const auto length = GetEnvironmentVariableW(L"CLIPROPE_PROBE_UI_LANGUAGE", forced, 16);
        if (length && length < 16) {
            const std::wstring name(forced, length);
            return localization::match_preferred_languages(std::span(&name, 1));
        }
#endif
        ULONG count{}, size{};
        if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) || size < 2)
            return localization::Locale::en_us;
        std::vector<wchar_t> buffer(size);
        if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &size))
            return localization::Locale::en_us;
        std::vector<std::wstring> names;
        for (size_t offset = 0; offset < buffer.size() && buffer[offset];) {
            auto end = offset;
            while (end < buffer.size() && buffer[end]) ++end;
            if (end == buffer.size()) break;
            names.emplace_back(buffer.data() + offset, end - offset);
            offset = end + 1;
        }
        return localization::match_preferred_languages(names);
    } catch (...) { return localization::Locale::en_us; }
}
UiLocalization::UiLocalization(localization::LocalizationManager& manager) noexcept : previous_(current) { current = &manager; }
UiLocalization::~UiLocalization() { current = previous_; }
const localization::LocalizationManager& ui_localization() noexcept {
    static const localization::LocalizationManager english;
    return current ? *current : english;
}
namespace { thread_local std::array<const wchar_t*, 2> ui_fonts{}; }
void invalidate_ui_fonts() noexcept { ui_fonts.fill(nullptr); }
const wchar_t* ui_font_family() noexcept {
    const auto index = ui_localization().effective_locale() == localization::Locale::zh_cn ? 1u : 0u;
    if (ui_fonts[index]) return ui_fonts[index];
    const auto* requested = index ? L"Microsoft YaHei UI" : L"Segoe UI Variable Text";
    LOGFONTW font{};
    font.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(font.lfFaceName, requested);
#ifdef CLIPROPE_DESKTOP_PROBES
    wchar_t fallback[2]{};
    if (GetEnvironmentVariableW(L"CLIPROPE_PROBE_FONT_FALLBACK", fallback, 2) == 1 && fallback[0] == L'1')
        wcscpy_s(font.lfFaceName, L"ClipRope Missing Test Font");
#endif
    bool exists{};
    if (auto dc = CreateCompatibleDC(nullptr)) {
        EnumFontFamiliesExW(dc, &font, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM data) -> int {
            *reinterpret_cast<bool*>(data) = true;
            return 0;
        }, LPARAM(&exists), 0);
        DeleteDC(dc);
    }
    return ui_fonts[index] = exists ? requested : L"Segoe UI";
}
const wchar_t* ui_text(localization::TextKey key) noexcept { return ui_localization().tr(key).data(); }
std::wstring ui_format(localization::TextKey key, std::initializer_list<localization::Argument> args) {
    return ui_localization().format(key, args);
}
void notify_language_changed() noexcept {
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
        PostMessageW(window, language_changed, 0, 0);
        return TRUE;
    }, 0);
}
struct UiText::Expression {
    TextKey key = TextKey::count;
    std::vector<UiArgument> args;
    std::vector<UiText> parts;
    bool strip_mnemonics{}, strip_ellipsis{}, escape_ampersands{};
    mutable std::wstring cached;
    mutable std::uint64_t revision = UINT64_MAX;
    mutable localization::Locale locale = localization::Locale::en_us;
    mutable const localization::LocalizationManager* owner{};
};
UiText UiText::message(TextKey key, std::initializer_list<UiArgument> args) {
    UiText text;
    auto expression = std::make_shared<Expression>();
    expression->key = key;
    expression->args.assign(args);
    text.value_ = std::move(expression);
    return text;
}
std::wstring_view UiText::view() const {
    if (const auto* plain = std::get_if<std::wstring>(&value_)) return *plain;
    const auto& manager = ui_localization();
    if (const auto* key = std::get_if<TextKey>(&value_)) return manager.tr(*key);
    auto& expression = *std::get<std::shared_ptr<Expression>>(value_);
    if (expression.owner != &manager || expression.revision != manager.revision() || expression.locale != manager.effective_locale()) {
        std::vector<localization::Argument> args;
        args.reserve(expression.args.size());
        for (const auto& argument : expression.args) args.push_back({argument.name, std::wstring(argument.value.view())});
        auto result = expression.key != TextKey::count ? manager.format(expression.key, args) : std::wstring{};
        for (const auto& part : expression.parts) result += part.view();
        if (expression.strip_mnemonics) std::erase(result, L'&');
        if (expression.strip_ellipsis && result.ends_with(L"…")) result.pop_back();
        if (expression.escape_ampersands) {
            std::wstring escaped;
            for (auto unit : result) { escaped += unit; if (unit == L'&') escaped += unit; }
            result = std::move(escaped);
        }
        expression.cached = std::move(result);
        expression.revision = manager.revision();
        expression.locale = manager.effective_locale();
        expression.owner = &manager;
    }
    return expression.cached;
}
UiText operator+(UiText left, UiText right) {
    UiText text;
    auto expression = std::make_shared<UiText::Expression>();
    expression->parts = {std::move(left), std::move(right)};
    text.value_ = std::move(expression);
    return text;
}
UiText& UiText::operator+=(UiText other) { return *this = *this + std::move(other); }
UiText UiText::display(bool remove_mnemonics, bool remove_ellipsis) const {
    UiText text;
    auto expression = std::make_shared<Expression>();
    expression->parts = {*this};
    expression->strip_mnemonics = remove_mnemonics;
    expression->strip_ellipsis = remove_ellipsis;
    text.value_ = std::move(expression);
    return text;
}
UiText UiText::native_literal() const {
    UiText text;
    auto expression = std::make_shared<Expression>();
    expression->parts = {*this};
    expression->escape_ampersands = true;
    text.value_ = std::move(expression);
    return text;
}
namespace {
constexpr wchar_t binding_property[] = L"ClipRope.LocalizedText";
struct TextBinding { UiText text, name, description; };
LRESULT CALLBACK binding_proc(HWND window, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
    if (msg == WM_NCDESTROY) {
        RemovePropW(window, binding_property);
        RemoveWindowSubclass(window, binding_proc, 1);
        delete reinterpret_cast<TextBinding*>(data);
    }
    return DefSubclassProc(window, msg, w, l);
}
TextBinding& binding(HWND window) {
    auto* data = static_cast<TextBinding*>(GetPropW(window, binding_property));
    if (!data) {
        auto created = std::make_unique<TextBinding>();
        if (!SetPropW(window, binding_property, created.get()) ||
            !SetWindowSubclass(window, binding_proc, 1, reinterpret_cast<DWORD_PTR>(created.get()))) {
            RemovePropW(window, binding_property);
            throw std::runtime_error("Cannot bind localized control text");
        }
        data = created.release();
    }
    return *data;
}
void refresh_binding(HWND window) {
    auto* data = static_cast<TextBinding*>(GetPropW(window, binding_property));
    if (!data) return;
    if (data->text.localized()) SetWindowTextW(window, data->text.c_str());
    if (data->name.localized()) fluent::accessible_name(window, data->name);
    if (data->description.localized()) fluent::accessible_description(window, data->description);
}
}
void bind_ui_text(HWND window, UiText text) {
    if (!window) return;
    if (text.localized() || GetPropW(window, binding_property)) binding(window).text = std::move(text);
}
UiText ui_caption(HWND window) {
    const auto* data = static_cast<const TextBinding*>(GetPropW(window, binding_property));
    return data && data->text.localized() ? data->text : UiText(fluent::caption(window));
}
bool set_ui_text(HWND window, UiText text) {
    if (!window || !SetWindowTextW(window, text.c_str())) return false;
    bind_ui_text(window, std::move(text));
    return true;
}
bool set_ui_item_text(HWND window, int id, UiText text) { return set_ui_text(GetDlgItem(window, id), std::move(text)); }
HWND create_ui_window(DWORD extended, const wchar_t* type, UiText title, DWORD style,
                      int x, int y, int width, int height, HWND owner, HMENU menu, HINSTANCE instance, void* data) {
    const auto window = CreateWindowExW(extended, type, title.c_str(), style, x, y, width, height, owner, menu, instance, data);
    if (window) bind_ui_text(window, std::move(title));
    return window;
}
void bind_ui_accessibility(HWND window, UiText text, bool description) {
    if (!window) return;
    if (text.localized() || GetPropW(window, binding_property))
        (description ? binding(window).description : binding(window).name) = std::move(text);
}
void refresh_ui_bindings(HWND root) {
    refresh_binding(root);
    EnumChildWindows(root, [](HWND child, LPARAM) -> BOOL { refresh_binding(child); return TRUE; }, 0);
}
} // namespace cliprope::win
