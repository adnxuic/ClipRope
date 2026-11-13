#include "persistence/history_store.hpp"
#include "../integration/history_fixture.hpp"
#include "../integration/history_observer.hpp"
#include "../clipboard_sources/sample.hpp"
#include <algorithm>
#include <future>
#include <iostream>
#include <thread>

namespace {
using namespace cliprope;
using namespace std::chrono_literals;
using history_fixture::require;
using Clock = std::chrono::steady_clock;
double ms(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

void measure(const std::filesystem::path& profile, bool sibling) {
    const auto input = sample::formats(false);
    domain::Format text, html;
    text.standard_id = CF_UNICODETEXT;
    text.bytes = input[0].bytes;
    html.name = "HTML Format";
    html.bytes = input[1].bytes;
    domain::Snapshot sample{{text, html}};
    persistence::HistoryStore store(profile);
    store.set_capacity("main", 2);
    const auto tail = store.admit("main", sample), survivor = store.admit("main", sample);
    auto db = history_fixture::observe(profile, SQLITE_OPEN_READWRITE);
    persistence::Transaction hold(db.get());
    std::promise<void> starting;
    auto started = starting.get_future();
    auto admission = std::async(std::launch::async, [&] {
        starting.set_value();
        return sibling ? store.admit_capture("main", sample) : store.admit("main", sample);
    });
    started.get();
    const auto begin = Clock::now();
    std::optional<Clock::time_point> unavailable;
    Clock::duration longest{}, max_gap{};
    unsigned total{}, misses{}, held_misses{};
    auto previous = begin;
    auto sample_mutex = [&] {
        const auto now = Clock::now();
        max_gap = std::max(max_gap, now - previous);
        previous = now;
        ++total;
        std::unique_lock probe(store.database().mutex(), std::try_to_lock);
        if (!probe.owns_lock()) {
            ++misses;
            if (!unavailable) unavailable = now;
            longest = std::max(longest, now - *unavailable);
        } else unavailable.reset();
    };
    while (Clock::now() - begin < 400ms) {
        sample_mutex();
        std::this_thread::sleep_for(2ms);
    }
    held_misses = misses;
    const auto release = Clock::now();
    hold.commit();
    while (admission.wait_for(0ms) != std::future_status::ready) {
        sample_mutex();
        std::this_thread::sleep_for(2ms);
    }
    const auto id = admission.get();
    const auto finish = Clock::now();
    const auto entries = store.hanging("main");
    require(entries.size() == 2 && entries[0].id == id && entries[1].id == survivor &&
            store.load(id).snapshot.formats.back().bytes == html.bytes, "Mutex comparison changed the admission");
    require(sibling ? misses == 0 : held_misses > 0, "Expected primary mutex availability was not sampled");
    std::cout << "executor=" << (sibling ? "sibling" : "primary") << " writer_hold_ms=" << ms(release - begin)
              << " observed_commit_ms=" << ms(finish - begin) << " sampling_requested_ms=2 max_sample_gap_ms=" << ms(max_gap)
              << " total_samples=" << total << " unavailable_samples=" << misses << " longest_sampled_busy_ms=" << ms(longest)
              << " admitted=" << id << " victim=" << tail << " survivor=" << survivor << '\n';
}
}
int main() {
    try {
        history_fixture::Directory dir;
        for (int i = 0; i < 3; ++i) {
            measure(dir.path / ("primary-" + std::to_string(i)), false);
            measure(dir.path / ("sibling-" + std::to_string(i)), true);
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
