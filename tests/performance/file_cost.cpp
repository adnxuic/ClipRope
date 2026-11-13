#include "../integration/history_fixture.hpp"
#include "../clipboard_sources/media_sample.hpp"
#include "persistence/history_store.hpp"
#include "rendering/card_browser.hpp"
#include "platform_win/file_group.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace cliprope;
using history_fixture::require;
using Clock = std::chrono::steady_clock;

void measure(unsigned count, bool picture, unsigned trial) {
    history_fixture::Directory dir;
    persistence::HistoryStore store(dir.path);
    std::vector<std::wstring> names;
    const auto png = media_sample::png();
    for (unsigned part = 0; part < count; ++part) {
        auto file = dir.path / (std::to_string(part) + (picture ? ".png" : ".txt"));
        std::ofstream stream(file, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(png.bytes.data()), png.bytes.size());
        require(stream.good(), "Cannot seed file");
        names.push_back(file.wstring());
    }
    domain::Snapshot snapshot{{{CF_HDROP, {}, encode_file_group(names)}}};
    const auto entry = store.admit("main", snapshot);
    for (bool warm : {false,true}) {
        rendering::CardBrowser browser{persistence::Browser(store)};
        const auto start = Clock::now();
        for (;;) {
            const auto card = browser.summary(entry, true);
            if (card.availability == domain::FileAvailability::available && (!picture || card.thumbnail)) break;
            require(Clock::now() - start < std::chrono::seconds(5), "File request failed to settle");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::cout << "file_read trial=" << trial << " count=" << count << " picture=" << picture << " warm=" << warm
                  << " elapsed_ms=" << std::chrono::duration<double,std::milli>(Clock::now()-start).count()
                  << " resident_pixels=" << browser.thumbnail_bytes() << '\n';
    }
}
}
int main() {
    try {
        for (unsigned trial = 1; trial <= 5; ++trial)
            for (unsigned count : {1u,4u}) for (bool picture : {false,true}) measure(count,picture,trial);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
