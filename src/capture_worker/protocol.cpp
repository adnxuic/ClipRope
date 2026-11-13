#include "capture_worker/protocol.hpp"
#include <algorithm>
#include "platform_win/file_group.hpp"
#include <cstring>
#include <stdexcept>

namespace cliprope::worker {
namespace {
constexpr unsigned magic = 0x43525036;
constexpr unsigned version = 3;
constexpr unsigned max_rows = 256;
struct Row {
    UINT format;
    DWORD offered_medium, aspect;
    LONG index;
    DWORD saved_medium, codec, bytes;
    char status[48];
};
struct Header {
    unsigned magic, version;
    std::uint64_t serial;
    DWORD sequence, count, payload_size, path_chars, path_kind;
    HRESULT error;
    unsigned retries;
    OomEvidence oom;
    char status[48];
    Row rows[max_rows];
};
struct View {
    unsigned char* base;
    View(HANDLE section, DWORD access) : base(static_cast<unsigned char*>(MapViewOfFile(section, access, 0, 0, section_size()))) {
        if (!base) throw std::runtime_error("Worker section unavailable");
    }
    ~View() { UnmapViewOfFile(base); }
};
void check(bool valid) {
    if (!valid) throw std::runtime_error("Invalid capture response");
}
bool committed(const unsigned char* base, std::size_t needed) {
    std::size_t present{};
    while (present < needed) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(base + present, &region, sizeof(region)) || region.State != MEM_COMMIT || !region.RegionSize ||
            region.AllocationBase != base || region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        present += region.RegionSize;
    }
    return true;
}
void label(char (&field)[48], const std::string& value) {
    check(!value.empty() && value.size() < sizeof(field));
    std::memcpy(field, value.c_str(), value.size() + 1);
}
std::string label(const char (&field)[48]) {
    const auto end = std::find(field, field + sizeof(field), '\0');
    check(end != field && end != field + sizeof(field) && std::all_of(field, end, [](char ch) {
        return (ch >= 'a' && ch <= 'z') || ch == '-';
    }));
    return {field, end};
}
}

std::size_t section_size() { return sizeof(Header) + snapshot_byte_limit + 65536; }

void write_response(HANDLE section, std::uint64_t serial, DWORD sequence, const CaptureResult& result) {
    check(result.formats.size() <= max_rows && result.saved.size() <= result.formats.size());
    std::size_t total{};
    for (const auto& item : result.saved) {
        check(item.bytes.size() <= snapshot_byte_limit - total);
        total += item.bytes.size();
    }
    check(result.screenshot_path.size() <= 32767);
    const auto path_bytes = result.screenshot_path.size() * sizeof(wchar_t);
    View view(section, FILE_MAP_WRITE);
    check(VirtualAlloc(view.base, sizeof(Header) + total + path_bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr);
    auto& header = *reinterpret_cast<Header*>(view.base);
    std::memset(&header, 0, sizeof(header));
    header.version = version;
    header.serial = serial;
    header.sequence = sequence;
    header.count = static_cast<DWORD>(result.formats.size());
    header.payload_size = static_cast<DWORD>(total);
    header.path_chars = static_cast<DWORD>(result.screenshot_path.size());
    header.path_kind = static_cast<DWORD>(result.path_kind);
    header.error = result.error;
    header.retries = result.retries;
    header.oom = result.oom;
    label(header.status, result.outcome);
    auto* payload = view.base + sizeof(Header);
    std::size_t used{}, saved{};
    for (size_t i = 0; i < result.formats.size(); ++i) {
        const auto& obs = result.formats[i];
        auto& row = header.rows[i];
        row.format = obs.format;
        row.offered_medium = obs.tymed;
        row.aspect = obs.aspect;
        row.index = obs.index;
        label(row.status, obs.outcome);
        if (obs.outcome != "saved") continue;
        const auto item = std::find_if(result.saved.begin(), result.saved.end(), [&](const auto& v) {
            return v.format == obs.format && v.index == obs.index && v.aspect == obs.aspect && (v.tymed & obs.tymed);
        });
        check(item != result.saved.end() && item->bytes.size() <= total - used);
        row.saved_medium = item->tymed;
        row.codec = item->codec_version;
        row.bytes = static_cast<DWORD>(item->bytes.size());
        if (row.bytes) std::memcpy(payload + used, item->bytes.data(), row.bytes);
        used += row.bytes;
        ++saved;
    }
    check(used == total && saved == result.saved.size());
    if (path_bytes) std::memcpy(payload + total, result.screenshot_path.data(), path_bytes);
    header.magic = magic;
}

CaptureResult read_response(HANDLE section, std::uint64_t serial, DWORD sequence) {
    View view(section, FILE_MAP_READ);
    check(committed(view.base, sizeof(Header)));
    const auto& header = *reinterpret_cast<const Header*>(view.base);
    check(header.magic == magic && header.version == version && header.serial == serial && header.sequence == sequence &&
          header.count <= max_rows && header.payload_size <= snapshot_byte_limit &&
          header.path_chars <= 32767 && header.path_kind <= 2 &&
          committed(view.base, sizeof(Header) + header.payload_size + header.path_chars * sizeof(wchar_t)));
    CaptureResult result;
    result.path_kind = static_cast<domain::PathKind>(header.path_kind);
    if (header.path_chars) {
        result.screenshot_path.resize(header.path_chars);
        std::memcpy(result.screenshot_path.data(), view.base + sizeof(Header) + header.payload_size, header.path_chars * sizeof(wchar_t));
        check(result.screenshot_path.find(L'\0') == std::wstring::npos &&
              disk_path(result.screenshot_path) && image_file_group({result.screenshot_path}));
    }
    result.outcome = label(header.status);
    result.error = header.error;
    result.retries = header.retries;
    check(header.oom.reacquisitions <= 2 && header.oom.guard <= NativeGuard::recovered &&
          header.oom.native_count <= 5 && header.oom.first_format <= 0xc000);
    result.oom = header.oom;
    const auto* payload = view.base + sizeof(Header);
    std::size_t used{};
    for (unsigned i = 0; i < header.count; ++i) {
        const auto& row = header.rows[i];
        check(row.format && row.format <= 0xffff);
        result.formats.push_back({row.format, row.offered_medium, row.aspect, row.index, label(row.status)});
        if (result.formats.back().outcome != "saved") { check(!row.saved_medium && !row.bytes); continue; }
        check(row.codec == 1 && row.aspect == DVASPECT_CONTENT && (row.saved_medium & row.offered_medium) &&
              (row.saved_medium == TYMED_HGLOBAL || row.saved_medium == TYMED_ISTREAM) &&
              row.bytes <= header.payload_size - used);
        check(std::none_of(result.saved.begin(), result.saved.end(), [&](const auto& item) {
            return item.format == row.format && item.index == row.index;
        }));
        result.saved.push_back({row.format, {payload + used, payload + used + row.bytes},
                                row.index, row.saved_medium, row.aspect, static_cast<int>(row.codec)});
        used += row.bytes;
    }
    check(used == header.payload_size);
    return result;
}
} // namespace cliprope::worker
