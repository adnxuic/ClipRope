#include "capture_worker/stream_reader.hpp"
#include <vector>

namespace cliprope {
std::string read_bounded_stream(IDataObject& source, Representation& item, std::size_t limit,
                                std::optional<std::uint64_t> declared_size, HRESULT& error,
                                const std::function<bool()>& superseded) {
    if (declared_size && *declared_size > limit) return "size-rejected";
    FORMATETC request{static_cast<CLIPFORMAT>(item.format), nullptr, item.aspect, item.index, TYMED_ISTREAM};
    STGMEDIUM medium{};
    const auto got = source.GetData(&request, &medium);
    if (FAILED(got)) { error = got; return got == CLIPBRD_E_CANT_OPEN ? "clipboard-busy" : "get-data-failed"; }
    struct Release { STGMEDIUM& medium; ~Release() { ReleaseStgMedium(&medium); } } release{medium};
    if (medium.tymed != TYMED_ISTREAM || !medium.pstm) return "invalid-medium";
    auto& stream = *medium.pstm;
    STATSTG stat{};
    const bool reported_size = SUCCEEDED(stream.Stat(&stat, STATFLAG_NONAME));
    if (reported_size && stat.cbSize.QuadPart > limit) return "size-rejected";
    LARGE_INTEGER start{};
    if (FAILED(stream.Seek(start, STREAM_SEEK_SET, nullptr))) return "unsupported-stream-position";
    std::vector<unsigned char> chunk(32 * 1024);
    for (;;) {
        if (superseded()) return "discarded-superseded";
        ULONG read{};
        const auto hr = stream.Read(chunk.data(), static_cast<ULONG>(chunk.size()), &read);
        if (FAILED(hr)) { error = hr; return "stream-read-failed"; }
        if (read > chunk.size() || read > limit - item.bytes.size()) return "size-rejected";
        item.bytes.insert(item.bytes.end(), chunk.begin(), chunk.begin() + read);
        if (!read || hr == S_FALSE) break;
    }
    if ((declared_size && item.bytes.size() != *declared_size) ||
        (reported_size && item.bytes.size() != stat.cbSize.QuadPart)) return "length-mismatch";
    return "saved";
}
}
