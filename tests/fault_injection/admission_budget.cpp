#include "platform_win/capture_admissions.hpp"
#include "platform_win/clipboard.hpp"
#include "../integration/history_fixture.hpp"
#include <condition_variable>
#include <iostream>

namespace {
using namespace cliprope;
using history_fixture::require;
application::Admission sample(unsigned serial, size_t bytes) {
    domain::Format text;
    text.standard_id = CF_UNICODETEXT;
    text.bytes.resize(bytes, 1);
    application::Admission request;
    request.ticket.serial = serial;
    request.ticket.version = serial + 10;
    request.ticket.rope = "bound";
    request.snapshot.formats.push_back(std::move(text));
    return request;
}

void bounds() {
    std::mutex mutex;
    std::condition_variable changed;
    bool release = false;
    win::CaptureAdmissions writer([&](const application::Admission& item) {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, std::chrono::seconds(3), [&] { return release; }), "Writer gate timeout");
        if (item.ticket.serial == 1) throw std::runtime_error("Injected write failure");
        return application::CaptureOutcome{"captured", item.ticket.rope};
    }, {});
    require(writer.submit(sample(1, snapshot_byte_limit)).admission_serial == 1, "Full-size copy was not accepted");
    const auto rejected = writer.submit(sample(2, 2));
    require(rejected.status == "history-failed" && !rejected.admission_serial && writer.busy() && !writer.next(),
            "Overflow displaced the accepted copy or produced an early receipt");
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    writer.drain_writes(); // Keep the failed receipt pending while another valid request is accepted.
    require(writer.submit(sample(3, 2)).admission_serial == 3, "Existing failure blocked a new admission");
    writer.drain_writes();
    require(writer.submit(sample(4, 2)).status == "history-failed", "Completion backlog exceeded two slots");
    const auto failed = writer.next();
    const auto saved = writer.next();
    require(failed && failed->serial == 1 && failed->outcome.status == "history-failed" &&
            saved && saved->serial == 3 && saved->sequence == 13 && saved->outcome.entry == "bound" &&
            !writer.busy() && !writer.next() && writer.peak_bytes() == snapshot_byte_limit,
            "Out-of-order, lost or repeated durable completion");
    std::cout << "pending-payload-peak=" << writer.peak_bytes()
              << " capacity-failures=2 accepted-failures=1 committed=1 undelivered-limit=2\n";
}
}

int main() {
    try { bounds(); return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
