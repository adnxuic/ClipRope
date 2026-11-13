#include "persistence/manifest.hpp"
#include <utility>

namespace cliprope::persistence {
namespace {
void field(std::vector<unsigned char>& bytes, std::string_view value) {
    const auto size = std::to_string(value.size());
    bytes.insert(bytes.end(), size.begin(), size.end());
    bytes.push_back(':');
    bytes.insert(bytes.end(), value.begin(), value.end());
}
template<class T> T number(const Statement& row, int column) {
    const auto value = row.integer(column);
    if (!std::in_range<T>(value)) throw StorageFailure(StorageProblem::damaged_manifest);
    return static_cast<T>(value);
}
}
domain::Format read_manifest_format(const Statement& row) {
    domain::Format f;
    f.standard_id = number<std::uint32_t>(row, 0);
    f.name = row.text(1);
    if (f.name.size() > 1024) throw StorageFailure(StorageProblem::damaged_manifest);
    f.tymed = number<std::uint32_t>(row, 2);
    f.aspect = number<std::uint32_t>(row, 3);
    f.index = number<std::int32_t>(row, 4);
    f.codec_version = number<std::int32_t>(row, 5);
    f.priority = number<std::int32_t>(row, 6);
    f.outcome = row.text(7);
    return f;
}
void append_manifest(std::vector<unsigned char>& bytes, const domain::Format& f, std::string_view blob) {
    field(bytes, std::to_string(f.standard_id));
    field(bytes, f.name);
    field(bytes, std::to_string(f.tymed));
    field(bytes, std::to_string(f.aspect));
    field(bytes, std::to_string(f.index));
    field(bytes, std::to_string(f.codec_version));
    field(bytes, std::to_string(f.priority));
    field(bytes, f.outcome);
    field(bytes, blob);
}
void verify_manifest(std::span<const unsigned char> bytes, std::string_view seal) {
    if (seal.size() != 64 || sha256(bytes) != seal) throw StorageFailure(StorageProblem::damaged_manifest);
}
}
