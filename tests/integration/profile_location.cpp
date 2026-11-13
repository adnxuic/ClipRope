#include "history_fixture.hpp"
#include "clipboard_process.hpp"
#include "../clipboard_sources/image_file_sample.hpp"
#include "../clipboard_sources/media_sample.hpp"
#include "persistence/profile_location.hpp"
#include "persistence/history_store.hpp"
#include "persistence/hotkey_settings.hpp"
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace cliprope;
using namespace persistence;
using namespace history_fixture;

void transfers() {
    Directory temp;
    const auto source = temp.path / "ClipRope", target = temp.path / L"新目录", backup = source / "backups" / "initial";
    fs::create_directory(target);
    std::string hanging, floating, recycled, later, screenshot;
    const application::Appearance custom{application::Theme::dark, 0x125ca5};
    {
        ProfileLocation location(temp.path);
        HistoryStore store(source);
        require(read_appearance(store.database()) == application::Appearance{}, "Fresh profile must default to Light.");
        save_appearance(store.database(), custom);
        rejects(
            [&] {
                save_appearance(store.database(), {application::Theme::light, 0xffffffff});
            },
            "Invalid color committed");
        require(read_appearance(store.database()) == custom, "Failed appearance write changed the setting");
        domain::Format blob;
        blob.name = "Synthetic";
        blob.bytes.resize(9000, 45);
        const auto png = media_sample::png().bytes;
        domain::Format image;
        image.name = "PNG";
        image.bytes = png;
        hanging = store.admit("main", {{blob, image}});
        floating = store.copy(hanging, "main");
        store.detach(floating, {"sample", 51, 66, 215, 175, true});
        recycled = store.copy(hanging, domain::rebin_id);
        store.database().backup_to(backup);
        screenshot = managed_screenshot(store.database(), store.entry(hanging), png).filename().string();
        std::ofstream(source / "unrelated.txt") << "keep";
        const auto unrelated_png = source / "screenshots" / "00000000000000000000000000000000.png";
        std::ofstream(unrelated_png) << "unregistered screenshot-shaped file";
        domain::Format file;
        file.standard_id = CF_HDROP;
        file.bytes = image_file_sample::drop({temp.path / "external.bin"}).bytes;
        store.admit("main", {{file}});
        const auto usage = profile_disk_usage(source);
        require(usage > 0, "No allocated disk usage");
        const auto staged = source / "staging" / "12345678123456781234567812345678.part";
        fs::create_directories(staged.parent_path());
        { std::ofstream(staged, std::ios::binary) << std::string(65536, 'S'); }
        DWORD upper{};
        const auto lower = GetCompressedFileSizeW(staged.c_str(), &upper);
        require(lower != INVALID_FILE_SIZE &&
                    profile_disk_usage(source) == usage + ((std::uint64_t(upper) << 32) | lower),
                "Usage did not match the allocated size of the staging sample");
        fs::remove(staged);
        std::ofstream(source / "unrelated.bin", std::ios::binary) << std::string(90000, 'X');
        std::ofstream(temp.path / "external.bin", std::ios::binary) << std::string(90000, 'Y');
        require(profile_disk_usage(source) == usage, "Counted unowned or external content");
        fs::create_directory(source / "nested");
        rejects([&] { location.schedule(source / "nested"); }, "Nested destination allowed");
        location.schedule(target);
        require(profile_disk_usage(location.active()) == usage, "Pending destination changed the measured root");
        require(location.active() == source && location.pending() == target, "Scheduling changed active history");
        location.schedule({});
        require(location.pending().empty(), "Cancel retained the pending path");
        location.schedule(target);
        later = store.copy(hanging, "main");
        rejects([&] { location.resume(); }, "Migration allowed a live writer");
    }
    { // A file arriving after scheduling cannot be overwritten.
        ProfileLocation location(temp.path);
        std::ofstream(target / "other.txt") << "keep";
        rejects([&] { location.resume(); }, "Reused an occupied destination");
        require(location.active() == source && fs::exists(target / "other.txt"), "Overwrote an occupied folder");
        fs::remove(target / "other.txt");
    }
    for (auto boundary : {"copy", "install", "cleanup"}) {
        ProfileLocation location(temp.path);
        bool injected = false;
        rejects(
            [&] {
                location.resume([&](auto point) {
                    if (point == boundary) {
                        injected = true;
                        throw StorageFailure(StorageProblem::full);
                    }
                });
            },
            "Injected failure was ignored");
        require(injected, boundary);
        if (std::string_view(boundary) != "cleanup") {
            require(location.active() == source && fs::exists(source / "history.db"), "Source lost before cutover");
            HistoryStore continued(source);
            later = continued.copy(hanging, "main");
        } else
            require(location.active() == target, "Lost committed destination during cleanup");
    }
    {
        ProfileLocation location(temp.path);
        location.resume();
        require(location.active() == target && location.pending().empty() && !fs::exists(source / "history.db"),
                "Relocation incomplete");
        require(fs::exists(source / "unrelated.txt") && !fs::exists(target / "unrelated.txt"), "Moved unrelated file");
        require(fs::exists(source / "screenshots" / "00000000000000000000000000000000.png"),
                "Unregistered screenshot removed");
        require(profile_disk_usage(location.active()) > 0, "Relocated profile has no measured usage");
        Database::verify_store(target / "backups" / "initial");
        HistoryStore restored(target);
        require(restored.hanging("main").front().id == later &&
                    restored.hanging(domain::rebin_id).front().id == recycled &&
                    restored.floating().front().id == floating && restored.layout(floating).x == 51,
                "History order or layout lost");
        require(restored.entry(floating).snapshot_id == restored.entry(hanging).snapshot_id &&
                    restored.load(floating).snapshot.formats[0].bytes.size() == 9000 &&
                    fs::exists(target / "screenshots" / screenshot) && read_appearance(restored.database()) == custom,
                "Shared content, screenshot or appearance missing");
    }
    {
        ProfileLocation location(temp.path);
        location.purge();
        require(!fs::exists(target) && fs::exists(source / "unrelated.bin") &&
                    !fs::exists(temp.path / "ClipRope.Settings"),
                "Purge used the wrong root");
    }
}
void interrupted_cleanup() {
    Directory temp;
    const auto target = temp.path / "target";
    fs::create_directory(target);
    ProfileLocation location(temp.path);
    const auto source = location.active();
    const fs::path file = fs::path("staging") / "12345678123456781234567812345678.part";
    { HistoryStore store(source); }
    fs::create_directories((source / file).parent_path());
    std::ofstream(source / file) << "pending";
    location.schedule(target);
    std::unique_ptr<void, decltype(&CloseHandle)> locked(nullptr, CloseHandle);
    rejects(
        [&] {
            location.resume([&](auto point) {
                if (point == "cleanup") {
                    auto h = CreateFileW((source / file).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                         0, nullptr);
                    require(h != INVALID_HANDLE_VALUE, "Cannot hold the staged file");
                    locked.reset(h);
                }
            });
        },
        "Partial cleanup fault was ignored");
    require(locked && location.active() == target && !fs::exists(source / "history.db") && fs::exists(source / file),
            "Partial cleanup changed the authoritative store");
    locked.reset();
    location.resume();
    require(!fs::exists(source / file) && fs::exists(target / file), "Cleanup did not resume");
}

void unsupported_source() {
    Directory temp;
    const auto target = temp.path / "target";
    fs::create_directory(target);
    ProfileLocation location(temp.path);
    const auto source = location.active();
    {
        HistoryStore store(source);
        location.schedule(target);
        exec(store.database().handle(), "PRAGMA user_version=10");
    }
    const auto hash = file_sha256(source / "history.db");
    try {
        location.resume();
        throw std::runtime_error("Relocated an unsupported database");
    } catch (const StorageFailure& error) {
        require(error.reason() == StorageProblem::unsupported_format, "Wrong relocation diagnostic");
    }
    require(location.active() == source && location.pending() == target && fs::is_empty(target) &&
                file_sha256(source / "history.db") == hash, "Rejected relocation changed data");
}

void corrupt_destination() {
    Directory temp;
    const auto target = temp.path / "target";
    fs::create_directory(target);
    ProfileLocation location(temp.path);
    const auto source = location.active();
    { HistoryStore store(source); }
    std::ofstream(source / "diagnostics.txt") << "original";
    location.schedule(target);
    rejects(
        [&] {
            location.resume([&](auto point) {
                if (point == "install" && fs::exists(target / "diagnostics.txt"))
                    std::ofstream(target / "diagnostics.txt") << "altered";
            });
        },
        "Destination digest mismatch allowed cutover");
    require(location.active() == source && fs::exists(source / "history.db"), "Damaged destination took ownership");
}
void unavailable_target() {
    Directory temp;
    const auto first = temp.path / "first", second = temp.path / "second", unplugged = temp.path / "unmounted";
    fs::create_directories(first);
    fs::create_directory(second);
    ProfileLocation location(temp.path);
    { HistoryStore fresh(location.active()); }
    location.schedule(first);
    rejects(
        [&] {
            location.resume([](auto point) {
                if (point == "install") throw StorageFailure(StorageProblem::access);
            });
        },
        "Installation interruption did not occur");
    // Antivirus readers may briefly hold the containing directory after the last copy.
    auto move_folder = [](const fs::path& from, const fs::path& to) {
        for (int trial = 0; trial < 50; ++trial) {
            if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return;
            Sleep(20);
        }
        throw std::runtime_error("Cannot simulate a disconnected data folder");
    };
    move_folder(first, unplugged);
    location.schedule(second);
    move_folder(unplugged, first);
    location.resume();
    require(location.active() == second && fs::is_empty(first), "Did not recover a disconnected abandoned target");
}
void abrupt_restart(const fs::path& executable, const wchar_t* boundary) {
    Directory temp;
    const auto target = temp.path / "target";
    fs::create_directory(target);
    std::string entry;
    {
        ProfileLocation location(temp.path);
        HistoryStore store(location.active());
        entry = store.create_floating_text({"test", 18, 25, 230, 180, true});
        location.schedule(target);
    }
    auto process =
        probe::launch({executable.wstring(), L"--interrupt", temp.path.wstring(), boundary}, probe::desktop_path());
    require(process != nullptr, "Interruption worker did not launch");
    DWORD code{};
    const auto done = WaitForSingleObject(process, 10000);
    if (done == WAIT_TIMEOUT) TerminateProcess(process, 1);
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    require(done == WAIT_OBJECT_0 && code == 73, "Interruption worker missed its boundary");
    ProfileLocation location(temp.path);
    location.resume();
    HistoryStore restored(location.active());
    require(location.active() == target && restored.floating().front().id == entry && restored.layout(entry).x == 18 &&
                !fs::exists(temp.path / "ClipRope" / "history.db"),
            "Restart lost the authoritative profile");
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 4 && std::wstring_view(argv[1]) == L"--interrupt") {
            ProfileLocation location(argv[2]);
            const auto boundary = fs::path(argv[3]).string();
            location.resume([&](auto point) {
                if (point == boundary) TerminateProcess(GetCurrentProcess(), 73);
            });
            return 2;
        }
        for (auto boundary : {L"copy", L"install", L"cleanup"})
            abrupt_restart(fs::absolute(argv[0]), boundary);
        transfers();
        unavailable_target();
        unsupported_source();
        corrupt_destination();
        interrupted_cleanup();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
