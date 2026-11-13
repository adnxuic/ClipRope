#pragma once

#include "presentation/card_labels.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace cliprope::presentation {
inline constexpr std::size_t text_preview_units = application::card_text_prefix_units;
inline std::wstring text_preview(std::wstring_view value, std::optional<std::wstring>& paper_override) {
    std::wstring text, paper;
    for (auto unit : value.substr(0, text_preview_units)) {
        if (!unit) break;
        paper.push_back(unit < L' ' && unit != L'\r' && unit != L'\n' && unit != L'\t' ? L' ' : unit);
        if (unit <= L' ') {
            if (!text.empty() && text.back() != L' ') text.push_back(L' ');
        } else text.push_back(unit);
    }
    if (!text.empty() && text.back() >= 0xd800 && text.back() <= 0xdbff) text.pop_back();
    if (!text.empty() && text.back() == L' ') text.pop_back();
    if (!paper.empty() && paper.back() >= 0xd800 && paper.back() <= 0xdbff) paper.pop_back();
    if (paper == text && !text.empty()) paper_override.reset();
    else paper_override = std::move(paper);
    return text;
}
// The first item of a saved file list. Parsing reads a bounded byte prefix and
// never touches the file system, so `folder` is only a hint for dropped paths.
struct FileList {
    std::wstring first;
    std::size_t count = 0;
    bool truncated = false, folder = false;
};

inline std::wstring_view leaf(std::wstring_view path) {
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        path.remove_suffix(1);
    const auto slash = path.find_last_of(L"\\/");
    return slash == path.npos ? path : path.substr(slash + 1);
}

inline std::wstring_view parent(std::wstring_view path) {
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        path.remove_suffix(1);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == path.npos)
        return {};
    // Keep the separator of a drive root such as "C:\".
    return path.substr(0, slash == 2 && path[1] == L':' ? slash + 1 : slash);
}

inline std::wstring extension(std::wstring_view name) {
    name = leaf(name);
    const auto dot = name.find_last_of(L'.');
    if (dot == name.npos || dot == 0 || dot + 1 == name.size() || name.size() - dot > 6)
        return {};
    std::wstring result(name.substr(dot + 1));
    for (auto &c : result)
        c = static_cast<wchar_t>(std::towupper(c));
    return result;
}

// Normalized CF_HDROP bytes: a DROPFILES header, then NUL-separated wide paths
// ending with an empty string. A partial trailing path is ignored.
inline std::optional<FileList> drop_files(std::span<const unsigned char> bytes) {
    if (bytes.size() < 20)
        return {};
    std::uint32_t offset{}, wide{};
    std::memcpy(&offset, bytes.data(), 4);
    std::memcpy(&wide, bytes.data() + 16, 4);
    if (!wide || offset < 20 || offset >= bytes.size())
        return {};
    FileList list;
    list.truncated = true;
    std::wstring current;
    std::size_t length = 0;
    for (std::size_t i = offset; i + 1 < bytes.size(); i += 2) {
        const auto unit = static_cast<wchar_t>(bytes[i] | (bytes[i + 1] << 8));
        if (unit) {
            // Later names are counted but not retained.
            if (!list.count && current.size() < 32767)
                current.push_back(unit);
            ++length;
            continue;
        }
        if (!length) {
            list.truncated = false;
            break;
        }
        if (!list.count)
            list.first = std::move(current);
        ++list.count;
        length = 0;
    }
    if (!list.count)
        return {};
    list.folder = (!list.first.empty() && (list.first.back() == L'\\' || list.first.back() == L'/')) ||
                  extension(list.first).empty();
    return list;
}

// FileGroupDescriptorW: a 32-bit count, then 592-byte FILEDESCRIPTORW records
// whose WCHAR[260] name starts at offset 72.
inline std::optional<FileList> file_descriptors(std::span<const unsigned char> bytes) {
    constexpr std::size_t record = 592, name = 72, attributes = 36;
    constexpr std::uint32_t has_attributes = 0x4, directory = 0x10;
    if (bytes.size() < 4 + record)
        return {};
    std::uint32_t items{}, flags{}, attrs{};
    std::memcpy(&items, bytes.data(), 4);
    if (!items)
        return {};
    const auto *first = bytes.data() + 4;
    std::memcpy(&flags, first, 4);
    std::memcpy(&attrs, first + attributes, 4);
    FileList list;
    list.count = items;
    for (std::size_t i = 0; i < 260; ++i) {
        const auto unit = static_cast<wchar_t>(first[name + 2 * i] | (first[name + 2 * i + 1] << 8));
        if (!unit)
            break;
        list.first.push_back(unit);
    }
    if (list.first.empty())
        return {};
    list.folder = (flags & has_attributes) && (attrs & directory);
    return list;
}

inline bool is_link(std::wstring_view text) {
    if (text.empty() || text.size() > 2048 || text.find_first_of(L" \t\r\n") != text.npos)
        return false;
    auto starts = [&](std::wstring_view prefix) {
        if (text.size() <= prefix.size())
            return false;
        for (std::size_t i = 0; i < prefix.size(); ++i)
            if (std::towlower(text[i]) != prefix[i])
                return false;
        return true;
    };
    return starts(L"http://") || starts(L"https://") || starts(L"www.") || starts(L"ftp://");
}

// The host of an address such as "https://example.com/a" is "example.com".
inline std::wstring_view link_host(std::wstring_view url) {
    if (const auto scheme = url.find(L"://"); scheme != url.npos)
        url.remove_prefix(scheme + 3);
    const auto end = url.find_first_of(L"/?#");
    return end == url.npos ? url : url.substr(0, end);
}

inline std::wstring describe_file(std::wstring_view name, bool folder,
                                  const localization::LocalizationManager& labels = english_labels()) {
    static constexpr std::array<std::pair<std::wstring_view, localization::TextKey>, 40> known{{
        {L"PDF", localization::TextKey::file_kind_pdf_document},       {L"DOC", localization::TextKey::file_kind_word_document},       {L"DOCX", localization::TextKey::file_kind_word_document},
        {L"XLS", localization::TextKey::file_kind_excel_workbook},     {L"XLSX", localization::TextKey::file_kind_excel_workbook},     {L"CSV", localization::TextKey::file_kind_csv_table},
        {L"PPT", localization::TextKey::file_kind_powerpoint_slides},  {L"PPTX", localization::TextKey::file_kind_powerpoint_slides},  {L"TXT", localization::TextKey::file_kind_text_document},
        {L"MD", localization::TextKey::file_kind_markdown_document},   {L"RTF", localization::TextKey::file_kind_rich_text_document},  {L"PNG", localization::TextKey::file_kind_png_image},
        {L"JPG", localization::TextKey::file_kind_jpeg_image},         {L"JPEG", localization::TextKey::file_kind_jpeg_image},         {L"GIF", localization::TextKey::file_kind_gif_image},
        {L"BMP", localization::TextKey::file_kind_bitmap_image},       {L"SVG", localization::TextKey::file_kind_svg_image},           {L"WEBP", localization::TextKey::file_kind_webp_image},
        {L"TIF", localization::TextKey::file_kind_tiff_image},         {L"TIFF", localization::TextKey::file_kind_tiff_image},         {L"ZIP", localization::TextKey::file_kind_zip_archive},
        {L"7Z", localization::TextKey::file_kind_seven_zip_archive},       {L"RAR", localization::TextKey::file_kind_rar_archive},         {L"EXE", localization::TextKey::file_kind_application},
        {L"MSI", localization::TextKey::file_kind_windows_installer},  {L"MP3", localization::TextKey::file_kind_audio_file},          {L"WAV", localization::TextKey::file_kind_audio_file},
        {L"FLAC", localization::TextKey::file_kind_audio_file},        {L"MP4", localization::TextKey::file_kind_video_file},          {L"MOV", localization::TextKey::file_kind_video_file},
        {L"MKV", localization::TextKey::file_kind_video_file},         {L"HTML", localization::TextKey::file_kind_web_page},           {L"HTM", localization::TextKey::file_kind_web_page},
        {L"JSON", localization::TextKey::file_kind_json_file},         {L"XML", localization::TextKey::file_kind_xml_file},            {L"PY", localization::TextKey::file_kind_python_script},
        {L"CPP", localization::TextKey::file_kind_c_source},         {L"HPP", localization::TextKey::file_kind_c_header},          {L"LNK", localization::TextKey::file_kind_shortcut},
        {L"URL", localization::TextKey::file_kind_internet_shortcut},
    }};
    const auto ext = extension(name);
    if (ext.empty())
        return std::wstring(labels.tr(folder ? localization::TextKey::card_kind_folder : localization::TextKey::file_kind_generic));
    for (const auto &[key, label] : known)
        if (key == ext)
            return std::wstring(labels.tr(label));
    return labels.format(localization::TextKey::file_kind_extension, {{"extension", ext}});
}

// Shortens a location by keeping its root and as many trailing folders as fit.
inline std::wstring elide_path(std::wstring_view path, std::size_t limit) {
    if (path.size() <= limit || limit < 4)
        return std::wstring(path);
    std::size_t root = 0;
    if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'))
        root = 3;
    else if (path.starts_with(L"\\\\")) {
        const auto server = path.find(L'\\', 2);
        const auto share = server == path.npos ? path.npos : path.find(L'\\', server + 1);
        root = share == path.npos ? 0 : share + 1;
    }
    const std::wstring_view head = path.substr(0, root);
    std::wstring_view tail;
    for (auto cut = path.size(); cut > root;) {
        const auto slash = path.find_last_of(L"\\/", cut - 1);
        if (slash == path.npos || slash < root)
            break;
        const auto candidate = path.substr(slash + 1);
        if (head.size() + 2 + candidate.size() > limit)
            break;
        tail = candidate;
        cut = slash;
    }
    if (tail.empty()) {
        const auto keep = limit - 1;
        return L"…" + std::wstring(path.substr(path.size() - keep));
    }
    return std::wstring(head) + L"…\\" + std::wstring(tail);
}
} // namespace cliprope::presentation
