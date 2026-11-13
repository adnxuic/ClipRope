#include "platform_win/search_text.hpp"
#include "application/card_search.hpp"
#include <windows.h>
#include <richedit.h>
#include <roapi.h>
#include <winrt/Windows.Data.Html.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <cwctype>
#include <span>

namespace cliprope::win {
using application::SearchFields;
namespace {
using application::SearchField;
using Bytes = std::span<const unsigned char>;

std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    std::wstring result(count, L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), result.data(), count);
    return result;
}
std::wstring units(Bytes bytes) {
    std::wstring result;
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
        wchar_t c = wchar_t(bytes[i] | (bytes[i + 1] << 8));
        if (!c) break;
        result.push_back(c);
    }
    return result;
}
unsigned word(Bytes b, size_t at) {
    unsigned value{};
    if (at + sizeof(value) <= b.size()) std::memcpy(&value, b.data() + at, sizeof(value));
    return value;
}
DWORD CALLBACK stream_rtf(DWORD_PTR cookie, LPBYTE data, LONG length, LONG* read) {
    auto& input = *reinterpret_cast<Bytes*>(cookie);
    *read = LONG(std::min(input.size(), size_t(length)));
    std::memcpy(data, input.data(), *read);
    input = input.subspan(*read);
    return 0;
}
std::wstring rtf_text(Bytes bytes) {
    static const auto rich_edit = LoadLibraryExW(L"msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!rich_edit) throw std::runtime_error("Rich Edit unavailable");
    HWND edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"", ES_MULTILINE, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!edit) throw std::runtime_error("Cannot extract saved RTF");
    struct Close {
        HWND h;
        ~Close() {
            DestroyWindow(h);
        }
    } close{edit};
    SendMessageW(edit, EM_EXLIMITTEXT, 0, 0x7ffffffe);
    EDITSTREAM stream{reinterpret_cast<DWORD_PTR>(&bytes), 0, stream_rtf};
    SendMessageW(edit, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&stream));
    if (stream.dwError) return {};
    int size = GetWindowTextLengthW(edit);
    std::wstring value(size + 1, L'\0');
    value.resize(GetWindowTextW(edit, value.data(), size + 1));
    return value;
}
std::wstring html_text(std::wstring_view fragment) {
    // HtmlUtilities converts inert markup without a browser or network context.
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    struct Uninit {
        HRESULT hr;
        ~Uninit() {
            if (SUCCEEDED(hr)) RoUninitialize();
        }
    } guard{hr};
    return std::wstring(winrt::Windows::Data::Html::HtmlUtilities::ConvertToText(fragment));
}
std::wstring comparable(std::wstring_view value) {
    std::wstring result;
    for (auto c : value) {
        if (iswspace(c) || c == 0xa0) {
            if (!result.empty() && result.back() != L' ') result.push_back(L' ');
        } else
            result.push_back(c);
    }
    if (!result.empty() && result.back() == L' ') result.pop_back();
    return result;
}
struct Fields {
    std::vector<std::pair<SearchField, std::wstring>> values;
    void add(SearchField kind, std::wstring value) {
        if (value.empty()) return;
        if (kind == SearchField::body || kind == SearchField::link) {
            const auto normalized = comparable(value);
            if (normalized.empty()) return;
            for (const auto& [existing, text] : values) {
                if ((existing == SearchField::body || existing == SearchField::link) &&
                    (text == value || comparable(text) == normalized ||
                     (kind == SearchField::link && existing == SearchField::body && text.find(value) != text.npos)))
                    return;
            }
        }
        values.emplace_back(kind, std::move(value));
    }
    void html(Bytes bytes) {
        const std::string_view data(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        auto offset = [&](std::string_view key) {
            auto pos = data.find(key);
            size_t result = data.npos;
            if (pos != data.npos) std::from_chars(data.data() + pos + key.size(), data.data() + data.size(), result);
            return result;
        };
        const size_t first = offset("StartFragment:"), end = offset("EndFragment:");
        if (first > end || end > data.size()) return;
        auto fragment = wide(data.substr(first, end - first));
        add(SearchField::body, html_text(fragment));
        const auto header = data.substr(0, std::min(first,offset("StartHTML:")));
        const auto source = header.find("\nSourceURL:");
        if (source != header.npos) {
            auto url = header.substr(source + 11);
            add(SearchField::link, wide(url.substr(0, url.find_first_of("\r\n"))));
        }
        auto folded = fragment;
        application::fold_find(folded);
        // Only quoted or unquoted href attributes on anchors are saved targets.
        for (size_t p = 0; (p = folded.find(L"<a", p)) != folded.npos;) {
            p += 2;
            if (p == folded.size() || !iswspace(folded[p])) continue;
            while (p < folded.size() && folded[p] != L'>') {
                if (iswspace(folded[p]) || folded[p] == L'/') {
                    ++p;
                    continue;
                }
                size_t start = p;
                while (p < folded.size() && !iswspace(folded[p]) && folded[p] != L'=' && folded[p] != L'>')
                    ++p;
                bool href = std::wstring_view(folded).substr(start, p - start) == L"href";
                while (p < folded.size() && iswspace(folded[p]))
                    ++p;
                if (p == folded.size() || folded[p] != L'=') continue;
                ++p;
                while (p < folded.size() && iswspace(folded[p]))
                    ++p;
                const wchar_t quote = p < folded.size() && (folded[p] == L'\'' || folded[p] == L'"') ? folded[p++] : 0;
                start = p;
                while (p < folded.size() && (quote ? folded[p] != quote : folded[p] != L'>' && !iswspace(folded[p])))
                    ++p;
                if (href && p > start) add(SearchField::link, html_text(fragment.substr(start, p - start)));
                if (quote && p < folded.size()) ++p;
            }
        }
    }
};
} // namespace
SearchFields extract_search_fields(const domain::Snapshot& snapshot, const SearchTextProbe& probe) {
    Fields fields;
    for (auto& f : snapshot.formats)
        if (f.outcome == "saved" && f.standard_id == CF_UNICODETEXT) fields.add(SearchField::body, units(f.bytes));
    for (auto& f : snapshot.formats) {
        if (f.outcome != "saved") continue;
        Bytes b(f.bytes);
        if (f.name == "HTML Format") {
            if (probe) probe("search-html-convert");
            fields.html(b);
        }
        if (f.name == "Rich Text Format") {
            if (probe) probe("search-rich-edit-create");
            fields.add(SearchField::body, rtf_text(b));
        }
        if (f.standard_id == CF_HDROP && word(b, 16)) {
            for (size_t at = word(b, 0); at >= 20 && at + 1 < b.size();) {
                auto path = units(b.subspan(at));
                if (path.empty()) break;
                at += (path.size() + 1) * 2;
                fields.add(SearchField::path, std::move(path));
            }
        }
        if (f.name == "FileGroupDescriptorW") {
            for (size_t i = 0; i < word(b, 0) && 4 + (i + 1) * 592 <= b.size(); ++i)
                fields.add(SearchField::path, units(b.subspan(4 + i * 592 + 72, 520)));
        }
    }
    append_search_metadata(fields.values, snapshot.screenshot_path, snapshot.source_hint);
    return std::move(fields.values);
}
void append_search_metadata(SearchFields& fields, std::string_view path, std::string_view source) {
    for (const auto& [kind, bytes] : {std::pair{SearchField::path, path}, std::pair{SearchField::metadata, source}}) {
        auto value = wide(bytes);
        if (!value.empty()) fields.emplace_back(kind, std::move(value));
    }
}
} // namespace cliprope::win
