#pragma once
#include <algorithm>
#include "platform_win/localization.hpp"
#include "platform_win/history_clipboard.hpp"
#include "platform_win/file_group.hpp"

namespace cliprope::win {
inline std::wstring restore_details(const domain::Snapshot& snapshot, std::wstring inventory) {
    auto formats = restorable_formats(snapshot);
    bool has_stream_files = false, private_formats = false;
    for (const auto& f : snapshot.formats) {
        if (f.standard_id == CF_HDROP && f.outcome == "saved" && !file_group_available({CF_HDROP, f.bytes}))
            return tr(TextKey::clipboard_files_missing) + L"\r\n\r\n" + inventory;
        if (f.outcome == "skipped" && !f.name.empty())
            private_formats = true;
    }
    UiText result =
        formats.empty() ? tr(TextKey::clipboard_no_formats) : tr(TextKey::clipboard_available);
    for (const auto& f : formats) {
        result += L"\r\n" + format_name(f.format);
        if (f.index >= 0)
            result += L" [" + std::to_wstring(f.index) + L"]";
        if (f.format == CF_HDROP)
            result += L" · " + message(TextKey::clipboard_references, {{"count", std::to_wstring(file_group_paths(f).size())}});
        else if (f.format == RegisterClipboardFormatW(L"FileContents")) {
            result += L" · " + tr(TextKey::clipboard_file_stream);
            has_stream_files = true;
        }
        else if (f.format == CF_DIB || f.format == CF_DIBV5 || f.format == RegisterClipboardFormatW(L"PNG"))
            result += L" · " + tr(TextKey::clipboard_image_pixels);
        if(f.tymed==TYMED_ISTREAM)result+=L" · IStream";
    }
    const bool image_files = std::any_of(formats.begin(), formats.end(), [](const auto& f) {
        return f.format == CF_HDROP && image_file_group(file_group_paths(f));
    });
    if (image_files) result += L"\r\n\r\n" + tr(TextKey::clipboard_files_help);
    else if (!formats.empty()) result += L"\r\n\r\n" + tr(TextKey::clipboard_paste_help);
    if (private_formats)
        result += L"\r\n" + tr(TextKey::clipboard_private_unverified);
    if (has_stream_files)
        result += L"\r\n" + tr(TextKey::clipboard_virtual_help);
    return result + L"\r\n\r\n" + inventory;
}
} // namespace cliprope::win
