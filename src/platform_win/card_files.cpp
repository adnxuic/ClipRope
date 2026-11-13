#include "platform_win/popup_menu.hpp"
#include "platform_win/card_files.hpp"
#include "platform_win/file_group.hpp"
#include "platform_win/screenshot_folder.hpp"
#include "rendering/image_preview.hpp"
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <memory>

namespace cliprope::win {
namespace fs = std::filesystem;
namespace {
fs::path from_utf8(const std::string& value) {
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
fs::path snip_folder(const domain::Snapshot& snapshot) {
    if (!snapshot.snip_directory.empty()) return from_utf8(snapshot.snip_directory);
    return system_snip_source(snapshot.source_hint) ? fs::path(native_screenshot_folder()) : fs::path{};
}
bool same_png_bytes(const fs::path& path, std::span<const unsigned char> saved, std::stop_token stop) {
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 16384> chunk;
    while (!saved.empty()) {
        if (stop.stop_requested()) return false;
        const auto n = std::min(saved.size(), chunk.size());
        if (!input.read(reinterpret_cast<char*>(chunk.data()), n) || !std::equal(chunk.begin(), chunk.begin() + n, saved.begin()))
            return false;
        saved = saved.subspan(n);
    }
    return input.peek() == std::char_traits<char>::eof() && input.eof() && !input.bad();
}
using ShellId = std::unique_ptr<std::remove_pointer_t<PIDLIST_ABSOLUTE>, decltype(&CoTaskMemFree)>;
ShellId shell_id(const fs::path& path) {
    PIDLIST_ABSOLUTE item{};
    SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr);
    return ShellId(item, CoTaskMemFree);
}
bool launch(HWND owner, const fs::path& path) {
    SHELLEXECUTEINFOW request{sizeof request};
    request.hwnd = owner;
    request.fMask = SEE_MASK_FLAG_NO_UI;
    request.lpVerb = L"open";
    request.lpFile = path.c_str();
    request.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&request) != FALSE;
}
struct ParentOrder {
    bool operator()(const fs::path& a, const fs::path& b) const {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }
};
using Groups = std::map<fs::path, std::vector<fs::path>, ParentOrder>;
Groups::const_iterator choose_group(HWND owner, const Groups& groups) {
    if (groups.size() <= 1) return groups.begin();
    std::vector<PopupItem> choices;
    for (const auto& group : groups) {
        PopupItem item{int(choices.size()+101),group.first.wstring(),fluent::Icon::folder};
        item.literal=true;
        choices.push_back(std::move(item));
    }
    POINT mouse{}; GetCursorPos(&mouse);
    auto choice = PopupMenuHost::track(owner, mouse, tr(TextKey::card_reveal), tr(TextKey::file_choose_folder), std::move(choices));
    if (choice < 101) return groups.end();
    choice -= 100;
    return choice && UINT(choice) <= groups.size() ? std::next(groups.begin(), choice - 1) : groups.end();
}

fs::path resolve_snip_original(persistence::HistoryStore& store, const domain::Entry& card,
                               const domain::Snapshot& snapshot, bool remember, std::stop_token stop = {}) {
    if (!snapshot.screenshot_path.empty()) return from_utf8(snapshot.screenshot_path);
    const bool tracked = snapshot.snip_begin > 0 && snapshot.snip_end >= snapshot.snip_begin;
    if (!tracked && (!system_snip_source(snapshot.source_hint) || snapshot.captured_at <= 0)) return {};
    const auto dir = snip_folder(snapshot);
    if (!disk_path(dir.wstring())) return {};
    const domain::Format* pixels{};
    for (const auto& f : snapshot.formats) {
        if (f.outcome != "saved") continue;
        if (!f.standard_id && _stricmp(f.name.c_str(), "PNG") == 0) { pixels = &f; break; }
        if (f.standard_id == CF_DIBV5 || (!pixels && f.standard_id == CF_DIB)) pixels = &f;
    }
    if (!pixels || (!tracked && pixels->standard_id)) return {};
    const UINT format = pixels->standard_id ? pixels->standard_id : RegisterClipboardFormatW(L"PNG");
    WIN32_FIND_DATAW data{};
    const auto search = FindFirstFileW((dir / L"*.png").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return {};
    struct Close { HANDLE handle; ~Close() { FindClose(handle); } } close{search};
    fs::path match;
    std::int64_t match_second{};
    constexpr std::int64_t capture_tolerance = 3; // Capture tickets expire after three seconds.
    do {
        if (stop.stop_requested()) throw std::runtime_error("Screenshot inspection cancelled");
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)) continue;
        const auto created = (std::int64_t(data.ftCreationTime.dwHighDateTime) << 32) | data.ftCreationTime.dwLowDateTime;
        const auto second = created / 10000000LL - 11644473600LL;
        if (tracked) {
            if (created < snapshot.snip_begin || created > snapshot.snip_end) continue;
        } else {
            if (snapshot.captured_at < second - capture_tolerance || snapshot.captured_at > second + capture_tolerance) continue;
            if (data.nFileSizeHigh || data.nFileSizeLow != pixels->bytes.size()) continue;
        }
        const auto candidate = dir / data.cFileName;
        if (tracked ? rendering::matches_image_file(candidate.wstring(), format, pixels->bytes, stop)
                    : same_png_bytes(candidate, pixels->bytes, stop)) {
            if (!match.empty()) return {};
            match = candidate;
            match_second = second;
        }
    } while (FindNextFileW(search, &data));
    if (stop.stop_requested() || GetLastError() != ERROR_NO_MORE_FILES || match.empty()) return {};
    const auto utf8 = match.u8string();
    std::optional<std::pair<std::int64_t, std::int64_t>> window;
    if (!tracked) window.emplace(match_second - capture_tolerance, match_second + capture_tolerance);
    return from_utf8(store.snip_original(card.snapshot_id, {reinterpret_cast<const char*>(utf8.data()), utf8.size()},
                                         window, remember));
}
}

fs::path find_snip_original(persistence::HistoryStore& store, const domain::Entry& entry, const domain::Snapshot& snapshot) {
    return resolve_snip_original(store, entry, snapshot, false);
}

fs::path bind_snip_original(persistence::HistoryStore& store, const domain::Entry& entry, const domain::Snapshot& snapshot) {
    return resolve_snip_original(store, entry, snapshot, true);
}

ScreenshotTarget screenshot_target(persistence::HistoryStore& store, const domain::Entry& entry,
                                   const domain::Snapshot& snapshot, CardCommand command, std::stop_token stop) {
    const auto original = resolve_snip_original(store, entry, snapshot, true, stop);
    if (stop.stop_requested()) throw std::runtime_error("Screenshot preparation cancelled");
    if (!original.empty()) {
        if (!disk_path(original.wstring()) || !image_file_group({original.wstring()}))
            throw std::runtime_error("Invalid screenshot origin");
        return {original};
    }
    if (command == CardCommand::reveal) {
        const auto directory = snip_folder(snapshot);
        if (disk_path(directory.wstring())) return {directory, true};
    }
    for (UINT type : {RegisterClipboardFormatW(L"PNG"), UINT(CF_DIBV5), UINT(CF_DIB)}) {
        for (const auto& f : snapshot.formats) {
            if (f.outcome != "saved") continue;
            if (type >= 0xc000 && !f.standard_id && _stricmp(f.name.c_str(), "PNG") == 0)
                return {store.materialize_screenshot(entry, f.bytes)};
            if (type == f.standard_id) {
                const auto png = rendering::encode_dib_png(type, f.bytes);
                if (stop.stop_requested()) throw std::runtime_error("Screenshot encoding cancelled");
                return {store.materialize_screenshot(entry, png)};
            }
        }
    }
    throw std::runtime_error("No saved screenshot pixels");
}

void visit_card_files(HWND owner, CardCommand command, const application::CardSummary& card,
                      persistence::HistoryStore& store, const ScreenshotTarget* prepared) {
    const auto live = [&] {
        const auto current = store.entry(card.entry.id);
        return current.snapshot_id == card.entry.snapshot_id && current.revision == card.entry.revision &&
               current.rope_id == card.entry.rope_id;
    };
    if (!live()) return;
    std::vector<std::wstring> paths;
    if (card.menu == application::CardMenuKind::screenshot) {
        const auto target = prepared ? *prepared :
            screenshot_target(store, card.entry, store.load_snapshot(card.entry.snapshot_id).snapshot, command);
        if (target.folder_only) {
            if (!live()) return;
            const auto attrs = GetFileAttributesW(target.path.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) || !launch(owner, target.path))
                CardEditor::file_notice(owner, tr(TextKey::error_screenshot_folder), target.path.wstring());
            return;
        }
        paths.push_back(target.path.wstring());
    } else {
        const auto saved = store.load_snapshot(card.entry.snapshot_id);
        for (const auto& f : saved.snapshot.formats)
            if (f.standard_id == CF_HDROP && f.outcome == "saved") { paths = file_group_paths(f.bytes); break; }
    }
    if (paths.empty() || !live()) return;
    if (command == CardCommand::open && paths.size() == 1) {
        const auto& path = paths.front();
        const auto attrs = GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) || !launch(owner, path))
            CardEditor::file_notice(owner, tr(TextKey::error_original_open), path);
        return;
    }
    Groups groups;
    for (fs::path path : paths) {
        path = path.lexically_normal();
        if (!path.has_filename() && path != path.root_path()) path = path.parent_path();
        groups[path.parent_path()].push_back(path);
    }
    const auto group = choose_group(owner, groups);
    if (group == groups.end() || !live()) return;
    auto parent = shell_id(group->first);
    std::vector<ShellId> items;
    std::vector<PCUITEMID_CHILD> children;
    fs::path missing;
    for (const auto& path : group->second) {
        if (path == group->first) continue;
        auto item = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES ? shell_id(path) : ShellId(nullptr, CoTaskMemFree);
        if (!item) { if (missing.empty()) missing = path; continue; }
        children.push_back(ILFindLastID(item.get()));
        items.push_back(std::move(item));
    }
    if (!live()) return;
    const bool ok = children.empty() ? parent && launch(owner, group->first) :
        parent && SUCCEEDED(SHOpenFolderAndSelectItems(parent.get(), static_cast<UINT>(children.size()), children.data(), 0));
    if (!missing.empty()) CardEditor::file_notice(owner, tr(TextKey::error_original_access), missing.wstring());
    else if (!ok) CardEditor::file_notice(owner, tr(TextKey::error_containing_folder), group->first.wstring());
}
} // namespace cliprope::win
