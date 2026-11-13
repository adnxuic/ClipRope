#include "presentation/card_labels.hpp"
#include "../integration/clipboard_process.hpp"
#include "persistence/history_store.hpp"
#include "rendering/card_browser.hpp"
#include "platform_win/history_clipboard.hpp"
#include "platform_win/office_selection.hpp"
#include "platform_win/process_image.hpp"
#include <cstring>
#include <iostream>

namespace {
using namespace cliprope;
DWORD owner() {
    DWORD pid{};
    GetWindowThreadProcessId(GetClipboardOwner(), &pid);
    return pid;
}
std::string utf8(std::wstring_view wide) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                        nullptr, nullptr);
    std::string text(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), len, nullptr,
                        nullptr);
    return text;
}
std::string name(UINT id) {
    auto wide = format_name(id);
    // Office registers both file-like names and public MIME names. Preserve
    // the latter in the inventory, without exporting any filesystem path.
    const auto slash = wide.find(L'/');
    if (wide.find_first_of(L"\\\r\n\t") != wide.npos ||
        (slash != wide.npos &&
         (!slash || wide.find(L'/', slash + 1) != wide.npos ||
          wide.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/.-+") !=
              wide.npos)))
        return "registered-name-redacted";
    return utf8(wide);
}
int inspect_source(const std::filesystem::path &root, DWORD pid) {
    if (!pid || owner() != pid)
        throw std::runtime_error("Verified Office process is not the clipboard owner");
    // This fixture is an explicit import of a synthetic object. Measure the
    // passive policy separately, and never change that policy for the desktop.
    auto result = capture_clipboard(GetClipboardSequenceNumber(), CaptureMode::local_history);
    std::cout << "policy=" << result.outcome << '\n';
    if (result.outcome != "partial" && result.outcome != "captured") {
        if (owner() != pid)
            throw std::runtime_error("Office owner changed before explicit import");
        result = capture_clipboard(GetClipboardSequenceNumber(), CaptureMode::all_formats_probe);
    }
    if (owner() != pid)
        throw std::runtime_error("Office owner changed during fixture read");
    for (const auto &f : result.formats)
        std::cout << "format\t" << f.format << '\t' << name(f.format) << '\t' << f.tymed << '\t' << f.aspect
                  << '\t' << f.index << '\t' << f.outcome << '\n';
    for (const auto &f : result.saved)
        std::cout << "saved\t" << name(f.format) << '\t' << f.bytes.size() << '\t' << f.tymed << '\t' << f.index
                  << '\n';
    persistence::HistoryStore store(root);
    std::string id;
    if (!result.saved.empty())
        id = store.admit("main", win::capture_snapshot(std::move(result)));
    std::cout << "explicit-entry=" << (id.empty() ? "none" : id) << '\n';
    return 0;
}
int manifest(const std::filesystem::path &root, std::string_view id) {
    persistence::HistoryStore store(root);
    auto card = rendering::CardBrowser(persistence::Browser(store)).summary(id);
    std::cout << "ui-label=" << utf8(card.capabilities) << '\n';
    const auto snapshot = store.load(id);
    for (const auto &format : snapshot.snapshot.formats) {
        if (format.outcome != "saved")
            continue;
        std::cout << "resident-format\t" << (format.standard_id ? name(format.standard_id) : format.name)
                  << '\t' << format.tymed << '\t' << format.bytes.size() << '\t'
                  << persistence::sha256(format.bytes) << '\n';
    }
    return 0;
}
int inspect_publication(DWORD pid) {
    if (!pid || owner() != pid)
        throw std::runtime_error("Resident ClipRope does not own restored object");
    const auto data = capture_clipboard(GetClipboardSequenceNumber(), CaptureMode::all_formats_probe);
    if (data.saved.empty())
        throw std::runtime_error("No recoverable representation on the published object");
    for (const auto &format : data.saved)
        std::cout << "readable\t" << name(format.format) << '\t' << format.tymed << '\t' << format.bytes.size()
                  << '\t' << persistence::sha256(format.bytes) << '\n';
    return 0;
}
int selection_probe(HWND field) {
    const auto app = win::office_app(win::process_basename(win::window_process(field)).c_str());
    const HWND view = win::office_document_view(field, app);
    if (!view) throw std::runtime_error("Missing Office document pane");
    win::OfficeSelection selection;
    bool bound{};
    for (std::string command; std::getline(std::cin, command);) {
        if (command == "bind") bound = selection.bind(view, app);
        else if (command != "verify") return 2;
        std::cout << ((command == "bind" ? bound : bound && selection.matches()) ? "yes" : "no") << std::endl;
    }
    return 0;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc < 3 || !probe::private_station() || FAILED(OleInitialize(nullptr)))
        return 2;
    int result = 1;
    try {
        const std::wstring_view action(argv[1]);
        if (action == L"--selection" && argc == 3)
            result = selection_probe(reinterpret_cast<HWND>(_wcstoui64(argv[2], nullptr, 10)));
        else if (action == L"--manifest" && argc == 4)
            result = manifest(argv[2], utf8(argv[3]));
        else if (action == L"--published" && argc == 3)
            result = inspect_publication(wcstoul(argv[2], nullptr, 10));
        else if (argc == 3)
            result = inspect_source(argv[1], wcstoul(argv[2], nullptr, 10));
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
    }
    OleUninitialize();
    return result;
}
