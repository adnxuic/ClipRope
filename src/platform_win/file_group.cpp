#include "platform_win/file_group.hpp"
#include "platform_win/image_file_access.hpp"

#include <shlobj.h>
#include <algorithm>
#include <cstring>
#include <span>

namespace cliprope {
namespace {
UINT preference() { return RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT); }

} // namespace

application::ExpandFileGroup inspect_expansion(application::ExpandFileGroup group) {
    for (auto& member : group.members) member.kind = win::local_path_kind(member.path);
    return group;
}

bool is_file_group_format(UINT format) { return format == CF_HDROP || format == preference(); }

std::vector<std::wstring> file_group_paths(const Representation& value) {
    return value.format == CF_HDROP ? file_group_paths(value.bytes) : std::vector<std::wstring>{};
}

bool validate_file_group(Representation& value) {
    if (value.format == CF_HDROP) {
        auto paths = file_group_paths(value);
        if (paths.empty()) return false;
        auto bytes = encode_file_group(paths);
        if (bytes.size() > max_group_bytes) return false;
        value.bytes = std::move(bytes);
        return true;
    }
    if (value.format != preference() || value.bytes.size() < sizeof(DWORD)) return false;
    DWORD effect{};
    std::memcpy(&effect, value.bytes.data(), sizeof effect);
    if (!effect || (effect & ~(DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK))) return false;
    value.bytes.resize(sizeof effect);
    return true;
}

bool file_group_available(const Representation& value) {
    const auto paths = file_group_paths(value);
    const auto images = image_file_group(paths);
    return !paths.empty() && std::all_of(paths.begin(), paths.end(), [images](const auto& path) {
        if (images) {
            WIN32_FILE_ATTRIBUTE_DATA info{};
            return win::inspect_image_file(path, info) == domain::FileAvailability::available;
        }
        return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    });
}

Representation copy_drop_effect() {
    return {preference(), copy_effect_bytes()};
}

bool prepare_file_group(std::vector<Representation>& saved) {
    auto group = std::find_if(saved.begin(), saved.end(), [](const auto& item) { return item.format == CF_HDROP; });
    if (group == saved.end()) {
        std::erase_if(saved, [](const auto& value) { return value.format == preference(); });
        return !saved.empty();
    }
    if (!file_group_available(*group)) return false;
    auto effect = std::find_if(saved.begin(), saved.end(), [](const auto& item) { return item.format == preference(); });
    if (effect == saved.end()) saved.push_back(copy_drop_effect());
    else *effect = copy_drop_effect();
    return true;
}
} // namespace cliprope
