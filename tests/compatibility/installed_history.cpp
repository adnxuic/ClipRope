#include "desktop_app.hpp"
#include "../clipboard_sources/external_copy.hpp"
#include "platform_win/history_clipboard.hpp"
#include <shlobj.h>

namespace {
using namespace workspace_fixture;
fs::path default_profile() {
    PWSTR raw{};
    require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)), "No local user profile");
    fs::path path(raw);
    CoTaskMemFree(raw);
    return path / L"ClipRope";
}
void normal_run(const fs::path &bin, const wchar_t *producer, const fs::path &profile, bool seed,
                bool use_default = true) {
    const auto exe = (bin / L"ClipRope.exe").wstring();
    std::vector<std::wstring> args{exe};
    if (!use_default) {
        args.emplace_back(L"--profile");
        args.push_back(profile.wstring());
    }
    HANDLE process = probe::launch(args, probe::desktop_path());
    require(process != nullptr, "Installed executable did not launch");
    std::unique_ptr<void, decltype(&CloseHandle)> guard(process, CloseHandle);
    const auto pid = GetProcessId(process);
    HWND listener{};
    try {
        require(until(
                    [&] {
                        listener = owned_window(pid, L"ClipRope.Listener");
                        wchar_t title[40]{};
                        if (listener)
                            GetWindowTextW(listener, title, 40);
                        return std::wstring_view(title) == L"ClipRope";
                    },
                    8s),
                "Packaged listener did not become ready");
        HANDLE token{};
        require(OpenProcessToken(process, TOKEN_QUERY, &token) != FALSE, "Could not inspect resident token");
        TOKEN_ELEVATION elevation{};
        DWORD size{};
        bool inspected =
            GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
        CloseHandle(token);
        require(inspected && !elevation.TokenIsElevated, "Resident process is elevated");
        if (seed) {
            probe::ExternalCopy source(producer, L"first");
            require(until([&] { return order(profile).size() == 1; }, 7s),
                    "Packaged resident did not admit a copy");
        }
        PostMessageW(listener, WM_CLOSE, 0, 0);
        require(until([&] { return WaitForSingleObject(process, 0) == WAIT_OBJECT_0; }, 5s),
                "Packaged resident did not exit");
        DWORD code{};
        require(GetExitCodeProcess(process, &code) && code == 0, "Installed executable reported an error");
    } catch (...) {
        if (listener)
            PostMessageW(listener, WM_CLOSE, 0, 0);
        if (WaitForSingleObject(process, 2000) == WAIT_TIMEOUT) {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 2000);
        }
        throw;
    }
}
std::string fingerprint(HistoryStore &store) {
    auto hanging = store.hanging("main"), floating = store.floating();
    require(hanging.size() == 1 && floating.size() == 1, "Wrong history owner count");
    std::ostringstream all;
    for (const auto &row : {hanging.front(), floating.front()}) {
        all << row.id << ' ' << row.snapshot_id << ' ' << row.rope_id.value_or("Floating") << '\n';
        auto data = store.load(row.id);
        for (const auto &format : data.snapshot.formats)
            if (format.outcome == "saved")
                all << format.standard_id << ' ' << format.name << ' ' << sha256(format.bytes) << '\n';
    }
    return all.str();
}
void check_paste(const fs::path &bin, const fs::path &profile) {
    HANDLE recall = probe::launch({(bin / L"cliprope_headless.exe").wstring(), profile.wstring(), L"head"},
                                  probe::desktop_path());
    require(recall != nullptr, "Recovery tool missing");
    require(until([&] { return WaitForSingleObject(recall, 0) == WAIT_OBJECT_0; }, 7s),
            "Installed recall timed out");
    DWORD code{};
    GetExitCodeProcess(recall, &code);
    CloseHandle(recall);
    require(code == 0, "Installed recall failed");
    const auto seq = GetClipboardSequenceNumber();
    require(capture_clipboard(seq).saved == sample::formats(false), "Installed paste payload changed");
    if (OpenClipboard(nullptr)) {
        if (seq == GetClipboardSequenceNumber())
            EmptyClipboard();
        CloseClipboard();
    }
}
void scenario(bool seed, const fs::path &bin, const wchar_t *producer, const fs::path &receipt) {
    const auto profile = default_profile();
    if (seed)
        require(!fs::exists(profile) && !fs::exists(receipt),
                "Seed needs an empty application profile and a new receipt");
    else
        require(fs::is_regular_file(receipt) && fs::is_regular_file(profile / "history.db"),
                "Retained history or receipt missing");
    normal_run(bin, producer, profile, seed);
    {
        HistoryStore store(profile);
        if (seed) {
            domain::Format format;
            format.standard_id = CF_UNICODETEXT;
            format.bytes.resize(12002, 0);
            for (size_t i = 0; i + 2 < format.bytes.size(); i += 2)
                format.bytes[i] = 'Z';
            auto id = store.admit("main", {{format}});
            store.detach(id, {"", 0, 0, 240, 160, false});
            std::ofstream out(receipt, std::ios::binary);
            out << fingerprint(store);
            require(out.good(), "Could not save rehearsal fingerprint");
        } else {
            std::ifstream in(receipt, std::ios::binary);
            const std::string expected{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            require(fingerprint(store) == expected, "IDs, Blob hashes or ownership changed");
        }
    }
    check_paste(bin, profile);
    std::cout << (seed ? "seed" : "verify")
              << " normal-user listener, independent local paste, Hanging and Floating fingerprint passed.\n";
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 5 || !probe::private_station() || FAILED(OleInitialize(nullptr)))
        return 2;
    int result{};
    try {
        auto mode = std::wstring_view(argv[1]);
        if (mode == L"--check-installed-worker") {
            history_fixture::Directory scratch;
            normal_run(argv[2], argv[3], scratch.path, true, false);
            check_paste(argv[2], scratch.path);
        } else {
            require(mode == L"--seed-empty-installed-profile" || mode == L"--verify-retained-profile",
                    "Explicit installed-profile operation required");
            scenario(mode == L"--seed-empty-installed-profile", argv[2], argv[3], argv[4]);
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    OleUninitialize();
    return result;
}
