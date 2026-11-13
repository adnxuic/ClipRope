#pragma once
#include "platform_win/virtual_files.hpp"
#include <array>
#include <cstring>

namespace sample {
inline std::vector<cliprope::Representation> virtual_group() {
    using namespace cliprope;
    const UINT count = 2;
    std::array<FILEDESCRIPTORW, 2> files{};
    wcscpy_s(files[0].cFileName, L"notes-\u03a9.txt");
    wcscpy_s(files[1].cFileName, L"binary.dat");
    std::vector<unsigned char> first{0xCE, 0xA9, 0x0d, 0x0a}, second{0, 255, 48, 127, 0, 1, 2};
    for (auto& file : files) { file.dwFlags = FD_FILESIZE | FD_ATTRIBUTES; file.dwFileAttributes = FILE_ATTRIBUTE_NORMAL; }
    files[0].nFileSizeLow = static_cast<DWORD>(first.size());
    files[1].nFileSizeLow = static_cast<DWORD>(second.size());
    std::vector<unsigned char> descriptor(offsetof(FILEGROUPDESCRIPTORW, fgd) + sizeof(files));
    std::memcpy(descriptor.data(), &count, sizeof(count));
    std::memcpy(descriptor.data() + offsetof(FILEGROUPDESCRIPTORW, fgd), files.data(), sizeof(files));
    return {{virtual_files::descriptor_format(), std::move(descriptor)},
            {virtual_files::contents_format(), std::move(first), 0, TYMED_ISTREAM},
            {virtual_files::contents_format(), std::move(second), 1, TYMED_ISTREAM}};
}
} // namespace sample
