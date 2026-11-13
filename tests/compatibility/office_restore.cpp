#include "capture_worker/capture.hpp"
#include "../integration/clipboard_process.hpp"

#include <charconv>
#include <cstring>
#include <iostream>
#include <string_view>

namespace {
DWORD owner_pid() {
    DWORD id{};
    if (HWND owner = GetClipboardOwner()) GetWindowThreadProcessId(owner, &id);
    return id;
}

int measure(DWORD excel_pid) {
    if (owner_pid() != excel_pid) {
        std::cerr << "The synthetic Excel instance does not own this clipboard.\n";
        return 1;
    }
    auto sample = cliprope::capture_clipboard(GetClipboardSequenceNumber());
    for (const auto& format : sample.formats) {
        std::wcout << L"format=" << cliprope::format_name(format.format)
                   << L" medium=" << format.tymed << L" aspect=" << format.aspect
                   << L" index=" << format.index << L" "
                   << std::wstring(format.outcome.begin(), format.outcome.end()) << L'\n';
    }
    if (sample.saved.empty() || owner_pid() != excel_pid) {
        std::cerr << "Excel capture failed: " << sample.outcome << '\n';
        return 1;
    }
    constexpr wchar_t sentinel[] = L"P0 overwrite";
    cliprope::Representation replacement{CF_UNICODETEXT, std::vector<unsigned char>(sizeof sentinel)};
    std::memcpy(replacement.bytes.data(), sentinel, sizeof sentinel);
    const std::vector<cliprope::Representation> overwrite{replacement};
    cliprope::ClipboardPublisher publisher;
    if (FAILED(publisher.publish(overwrite)) ||
        cliprope::capture_clipboard(GetClipboardSequenceNumber()).saved != overwrite) {
        std::cerr << "Could not verify the intervening overwrite.\n";
        return 1;
    }
    if (FAILED(publisher.publish(sample.saved)) || publisher.flush_if_current() != S_OK) {
        std::cerr << "Could not restore and flush the saved representations.\n";
        return 1;
    }
    std::wcout << L"restored=" << sample.saved.size() << L" intervening-overwrite=verified\n"
               << L"restore-sequence=" << GetClipboardSequenceNumber() << L'\n';
    return 0;
}
}

int main(int argc, char** argv) {
    if (argc != 2 || !probe::private_station()) {
        std::cerr << "Use the Office fixture in the private test station.\n";
        return 2;
    }
    DWORD id{};
    std::string_view argument(argv[1]);
    auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() || id == 0) return 2;
    if (FAILED(OleInitialize(nullptr))) return 1;
    int result = measure(id);
    OleUninitialize();
    return result;
}
