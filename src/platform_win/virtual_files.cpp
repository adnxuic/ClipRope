#include "platform_win/virtual_files.hpp"
#include "platform_win/file_basename.hpp"
#include "rendering/image_preview.hpp"
#include <stdexcept>
#include <algorithm>
#include <cstring>

namespace cliprope::virtual_files {
UINT descriptor_format() { return RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW); }
UINT contents_format() { return RegisterClipboardFormatW(CFSTR_FILECONTENTS); }
std::vector<FILEDESCRIPTORW> descriptors(const Representation& group) {
    constexpr auto header = offsetof(FILEGROUPDESCRIPTORW, fgd);
    if (group.format != descriptor_format() || group.bytes.size() < header) return {};
    UINT count{};
    std::memcpy(&count, group.bytes.data(), sizeof(count));
    if (!count || count > file_limit || count * sizeof(FILEDESCRIPTORW) > group.bytes.size() - header) return {};
    std::vector<FILEDESCRIPTORW> files(count);
    std::memcpy(files.data(), group.bytes.data() + header, files.size() * sizeof(FILEDESCRIPTORW));
    for (size_t i = 0; i < files.size(); ++i) {
        auto& file = files[i];
        const auto end = std::find(std::begin(file.cFileName), std::end(file.cFileName), L'\0');
        const std::wstring_view name(file.cFileName, end);
        if (!win::safe_file_basename(name) || ((file.dwFlags & FD_ATTRIBUTES) &&
            (file.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_REPARSE_POINT)))) return {};
        for (size_t j = 0; j < i; ++j)
            if (CompareStringOrdinal(files[j].cFileName, -1, file.cFileName, -1, TRUE) == CSTR_EQUAL) return {};
    }
    return files;
}

bool validate_descriptors(Representation& group) {
    const auto files = descriptors(group);
    if (files.empty()) return false;
    group.bytes.resize(offsetof(FILEGROUPDESCRIPTORW, fgd) + sizeof(FILEDESCRIPTORW) * files.size());
    return true;
}

std::optional<std::size_t> append_screenshot(std::vector<Representation>& formats) {
    const auto png = RegisterClipboardFormatW(L"PNG");
    const auto find = [&](UINT id) {
        return std::find_if(formats.begin(), formats.end(), [id](const auto& f) { return f.format == id; });
    };
    auto source = find(png);
    std::optional<std::size_t> alias;
    Representation contents{contents_format(), {}, 0, TYMED_ISTREAM};
    if (source != formats.end()) {
        alias = static_cast<std::size_t>(source - formats.begin());
    } else {
        source = find(CF_DIBV5);
        if (source == formats.end()) source = find(CF_DIB);
        if (source == formats.end()) throw std::invalid_argument("Screenshot has no saved pixels");
        contents.bytes = rendering::encode_dib_png(source->format, source->bytes);
    }
    FILEGROUPDESCRIPTORW group{};
    group.cItems = 1;
    auto& file = group.fgd[0];
    file.dwFlags = FD_ATTRIBUTES | FD_FILESIZE | static_cast<DWORD>(FD_UNICODE);
    file.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    file.nFileSizeLow = static_cast<DWORD>(alias ? formats[*alias].bytes.size() : contents.bytes.size());
    SYSTEMTIME time{};
    GetLocalTime(&time);
    swprintf_s(file.cFileName, L"Screenshot-%04u%02u%02u-%02u%02u%02u-%03u.png", time.wYear, time.wMonth,
               time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    Representation descriptor{descriptor_format(), std::vector<unsigned char>(sizeof group)};
    std::memcpy(descriptor.bytes.data(), &group, sizeof group);
    formats.push_back(std::move(descriptor));
    formats.push_back(std::move(contents));
    return alias;
}

bool complete(const std::vector<Representation>& formats) {
    const auto desc = descriptor_format(), content = contents_format();
    const auto group = std::find_if(formats.begin(), formats.end(), [&](const auto& item) { return item.format == desc; });
    const auto count = std::count_if(formats.begin(), formats.end(), [&](const auto& item) { return item.format == content; });
    if (group == formats.end()) return count == 0;
    const auto files = descriptors(*group);
    if (files.empty() || static_cast<size_t>(count) != files.size()) return false;
    std::size_t total{};
    for (size_t i = 0; i < files.size(); ++i) {
        const auto item = std::find_if(formats.begin(), formats.end(),
            [&](const auto& stream) { return stream.format == content && stream.index == static_cast<LONG>(i); });
        if (item == formats.end() || item->tymed != TYMED_ISTREAM || item->bytes.size() > stream_limit) return false;
        if ((files[i].dwFlags & FD_FILESIZE) && (files[i].nFileSizeHigh || files[i].nFileSizeLow != item->bytes.size())) return false;
        total += item->bytes.size();
    }
    return total <= group_limit;
}
} // namespace cliprope::virtual_files
