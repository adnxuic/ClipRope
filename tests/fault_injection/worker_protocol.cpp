#include "capture_worker/protocol.hpp"
#include "../integration/history_fixture.hpp"
#include <iostream>

int main() {
    using namespace cliprope;
    using history_fixture::rejects;
    using history_fixture::require;
    auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE, 0,
                                      static_cast<DWORD>(worker::section_size()), nullptr);
    if (!section) return 1;
    try {
        rejects([&] { worker::read_response(section, 1, 17); }, "Uncommitted response was accepted");
        CaptureResult data{{{CF_UNICODETEXT, {65, 0, 0, 0}}},
                           {{CF_UNICODETEXT, TYMED_HGLOBAL, DVASPECT_CONTENT, -1, "saved"}}, "captured"};
        worker::write_response(section, 1, 17, data);
        auto decoded = worker::read_response(section, 1, 17);
        require(decoded.saved == data.saved && decoded.formats.size() == 1 && decoded.outcome == "captured",
                "Valid response changed");
        rejects([&] { worker::read_response(section, 2, 17); }, "Foreign ticket was accepted");
        rejects([&] { worker::read_response(section, 1, 18); }, "Foreign clipboard version was accepted");
        data.saved.front().codec_version = 99;
        worker::write_response(section, 2, 18, data);
        rejects([&] { worker::read_response(section, 2, 18); }, "Unknown saved codec was accepted");
        data.formats.resize(257);
        rejects([&] { worker::write_response(section, 1, 17, data); }, "Unbounded inventory was accepted");
        std::cout << "uncommitted/stale-ticket/stale-version/unknown-codec/descriptor-overflow=rejected\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; CloseHandle(section); return 1; }
    CloseHandle(section);
    return 0;
}
