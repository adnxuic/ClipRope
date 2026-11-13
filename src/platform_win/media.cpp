#include "platform_win/media.hpp"
#include "rendering/image_preview.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <cstdint>
#include <limits>
#include <string_view>

namespace cliprope::media {
namespace {
using Bytes = std::span<const unsigned char>;
std::uint32_t be(Bytes data, std::size_t i) {
    return (std::uint32_t(data[i]) << 24) | (std::uint32_t(data[i + 1]) << 16) |
           (std::uint32_t(data[i + 2]) << 8) | data[i + 3];
}
std::uint32_t le(Bytes data, std::size_t i) {
    return data[i] | (std::uint32_t(data[i + 1]) << 8) | (std::uint32_t(data[i + 2]) << 16) |
           (std::uint32_t(data[i + 3]) << 24);
}
bool dimensions(std::uint32_t w, std::uint32_t h) {
    return w && h && w <= max_side && h <= max_side && w <= max_pixels / h;
}
bool png(std::vector<unsigned char>& bytes) {
    constexpr std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < 33 || !std::equal(signature.begin(), signature.end(), bytes.begin()))
        return false;
    bool pixels = false;
    for (std::size_t i = 8; i + 12 <= bytes.size();) {
        const auto length = be(bytes, i), type = be(bytes, i + 4);
        if (length > bytes.size() - i - 12)
            return false;
        if (i == 8 &&
            (type != 0x49484452 || length != 13 || !dimensions(be(bytes, i + 8), be(bytes, i + 12))))
            return false;
        pixels |= type == 0x49444154;
        i += 12 + length;
        if (type == 0x49454e44) {
            if (length || !pixels)
                return false;
            bytes.resize(i);
            return rendering::validate_png_pixels(bytes);
        }
    }
    return false;
}
bool rtf(std::vector<unsigned char>& bytes) {
    const std::string_view s(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!s.starts_with("{\\rtf1") || s.size() < 7 || (s[6] >= '0' && s[6] <= '9'))
        return false;
    int depth = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '{') {
            if (++depth > 256)
                return false;
            continue;
        }
        if (c == '}') {
            if (--depth == 0) {
                ++i;
                while (i < s.size() && (s[i] == '\r' || s[i] == '\n' || s[i] == ' '))
                    ++i;
                if ((i < s.size() && s[i] != '\0') || i >= 8u * 1024u * 1024u)
                    return false;
                bytes.resize(i + 1);
                bytes[i] = 0;
                return true;
            }
        }
        if (!c)
            return false;
        if (c != '\\')
            continue;
        if (++i >= s.size())
            return false;
        if (s[i] == '\'') {
            if (i + 2 >= s.size())
                return false;
            const auto hex = [](char v) {
                return (v >= '0' && v <= '9') || (v >= 'a' && v <= 'f') || (v >= 'A' && v <= 'F');
            };
            if (!hex(s[i + 1]) || !hex(s[i + 2]))
                return false;
            i += 2;
        } else if (s.substr(i, 3) == "bin" && i + 3 < s.size() && s[i + 3] >= '0' && s[i + 3] <= '9') {
            std::size_t length{};
            auto [next, ec] = std::from_chars(s.data() + i + 3, s.data() + s.size(), length);
            if (ec != std::errc{})
                return false;
            auto offset = static_cast<std::size_t>(next - s.data());
            if (offset < s.size() && s[offset] == ' ')
                ++offset;
            if (length > s.size() - offset)
                return false;
            i = offset + length - 1;
        } else if (!s[i])
            return false;
    }
    return false;
}
} // namespace
bool supports(UINT id) {
    return id == CF_DIB || id == CF_DIBV5 || id == RegisterClipboardFormatW(L"PNG") ||
           id == RegisterClipboardFormatW(L"Rich Text Format");
}
std::size_t byte_limit(UINT id) {
    return id == RegisterClipboardFormatW(L"Rich Text Format") ? 8u * 1024u * 1024u : 32u * 1024u * 1024u;
}
std::optional<Dib> inspect_dib(UINT id, Bytes bytes) {
    if ((id != CF_DIB && id != CF_DIBV5) || bytes.size() < 40)
        return {};
    const auto header = le(bytes, 0), width = le(bytes, 4), raw_h = le(bytes, 8);
    if ((header != 40 && header != 108 && header != 124) || bytes.size() < header ||
        (id == CF_DIBV5 && header != 124))
        return {};
    const bool bottom = (raw_h & 0x80000000u) == 0;
    const auto height = bottom ? raw_h : 0u - raw_h;
    const auto bits = static_cast<unsigned>(bytes[14] | (bytes[15] << 8));
    const auto comp = le(bytes, 16);
    if (!dimensions(width, height) || bytes[12] != 1 || bytes[13] != 0 || (bits != 24 && bits != 32) ||
        (comp != BI_RGB && !(comp == BI_BITFIELDS && bits == 32)) || le(bytes, 32) != 0)
        return {};
    std::size_t offset = header;
    bool alpha = false;
    if (comp == BI_BITFIELDS) {
        if (header == 40)
            offset += 12;
        if (bytes.size() < offset || le(bytes, 40) != 0xff0000 || le(bytes, 44) != 0xff00 ||
            le(bytes, 48) != 0xff)
            return {};
        if (header > 40) {
            const auto mask = le(bytes, 52);
            if (mask != 0 && mask != 0xff000000)
                return {};
            alpha = mask != 0;
        }
    }
    if (header >= 108 && (le(bytes, 56) == PROFILE_LINKED || le(bytes, 56) == PROFILE_EMBEDDED))
        return {};
    if (header == 124 && (le(bytes, 112) || le(bytes, 116)))
        return {};
    const auto stride = ((width * bits + 31) / 32) * 4;
    const auto payload = static_cast<std::size_t>(stride) * height;
    if (payload > bytes.size() - offset || (le(bytes, 20) && le(bytes, 20) != payload))
        return {};
    return Dib{width, height, bits, stride, offset, offset + payload, bottom, alpha};
}
bool validate(UINT format, std::vector<unsigned char>& bytes) {
    if (bytes.empty() || bytes.size() > byte_limit(format))
        return false;
    if (format == RegisterClipboardFormatW(L"PNG"))
        return png(bytes);
    if (format == RegisterClipboardFormatW(L"Rich Text Format"))
        return rtf(bytes);
    auto dib = inspect_dib(format, bytes);
    if (!dib)
        return false;
    bytes.resize(dib->size);
    return true;
}
std::optional<domain::ImageMetadata> screenshot_header(UINT format, Bytes bytes) {
    if (bytes.empty() || bytes.size() > byte_limit(format)) return {};
    if (format == CF_DIB || format == CF_DIBV5) {
        if (auto dib = inspect_dib(format, bytes)) return domain::ImageMetadata{dib->width, dib->height};
        return {};
    }
    constexpr std::array<unsigned char, 8> signature{137,80,78,71,13,10,26,10};
    if (format != RegisterClipboardFormatW(L"PNG") || bytes.size() < 33 ||
        !std::equal(signature.begin(), signature.end(), bytes.begin()) ||
        be(bytes, 8) != 13 || be(bytes, 12) != 0x49484452 || !dimensions(be(bytes,16),be(bytes,20))) return {};
    return domain::ImageMetadata{be(bytes,16), be(bytes,20)};
}
} // namespace cliprope::media
