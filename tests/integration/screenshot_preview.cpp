#include "history_fixture.hpp"
#include "../clipboard_sources/media_sample.hpp"
#include "persistence/content.hpp"
#include "persistence/history_store.hpp"
#include "preview_worker/process.hpp"
#include "platform_win/media.hpp"
#include "rendering/screenshot_preview_service.hpp"
#include "persistence/hotkey_settings.hpp"
#include "platform_win/screenshot_hover.hpp"
#include <thread>
#include <iostream>
#include <atomic>
#include <fstream>

using namespace cliprope;
using namespace history_fixture;
int main(int argc, char** argv) {
    try {
        const auto until = [] {
            return std::chrono::steady_clock::now() + std::chrono::seconds(2);
        };
        std::uint64_t serial = 0;
        for (auto sample :
             {media_sample::png(), media_sample::dib(false), media_sample::dib(true)}) {
            preview_worker::Request request;
            request.serial = ++serial;
            request.length = static_cast<unsigned>(sample.bytes.size());
            request.width = 1600;
            request.height = 1200;
            request.encoding = sample.format == CF_DIB     ? application::ScreenshotEncoding::dib
                               : sample.format == CF_DIBV5 ? application::ScreenshotEncoding::dibv5
                                                           : application::ScreenshotEncoding::png;
            const auto begin = std::chrono::steady_clock::now();
            auto image = preview_worker::decode(request, sample.bytes, {}, until());
            require(image.width == 2 && image.height == 2 && image.pbgra.size() == 16,
                    "Sandbox pixel size incorrect");
            require(image.pbgra[2] == 255 && image.pbgra[3] == 255, "Sandbox pixels incorrect");
            std::cout << "sandbox_ms="
                      << std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - begin)
                             .count()
                      << '\n';
            std::stop_source stop;
            stop.request_stop();
            rejects(
                [&] { preview_worker::decode(request, sample.bytes, stop.get_token(), until()); },
                "Cancellation ignored");
            preview_worker::Response bad;
            bad.serial = request.serial;
            bad.status = 0;
            bad.width = 0xffffffff;
            bad.height = 4;
            require(!preview_worker::valid(bad, request), "Invalid IPC response accepted");
            request.width = 2048;
            request.height = 2048;
            require(!preview_worker::valid(request), "Output pixel budget ignored");
        }
        auto bad_png = media_sample::png();
        bad_png.bytes[16] = 127;
        require(!media::screenshot_header(bad_png.format, bad_png.bytes), "Oversized PNG accepted");
        application::CardSummary card;
        card.menu = application::CardMenuKind::single_file;
        card.image_primary = true;
        require(!application::screenshot_preview_eligible(card), "File image was eligible");
        Directory directory;
        persistence::HistoryStore store(directory.path);
        persistence::ContentRepository repository(store.database());
        auto input = media_sample::dib(false);
        BITMAPINFOHEADER header{};
        std::memcpy(&header, input.bytes.data(), sizeof header);
        header.biWidth = 128;
        header.biHeight = 128;
        header.biSizeImage = 128 * 128 * 3;
        input.bytes.resize(sizeof header + header.biSizeImage, 255);
        std::memcpy(input.bytes.data(), &header, sizeof header);
        domain::Format f;
        f.standard_id = CF_DIB;
        f.bytes = input.bytes;
        const auto id = store.admit("main", domain::Snapshot{{f}});
        auto entry = store.entry(id);
        auto source = repository.screenshot(entry, {}, until());
        require(source.read({}, until()) == input.bytes, "Selective source changed bytes");
        auto stale = entry;
        ++stale.revision;
        rejects([&] { repository.screenshot(stale, {}, until()); }, "Stale source accepted");
        domain::Format text;
        text.standard_id = CF_UNICODETEXT;
        text.bytes = {'x', 0, 0, 0};
        auto mixed = store.admit("main", domain::Snapshot{{text, f}});
        rejects([&] { repository.screenshot(store.entry(mixed), {}, until()); },
                "Text companion accepted");
        const auto hash = source.hash;
        store.erase(id);
        store.erase(mixed);
        store.gc();
        require(source.read({}, until()) == input.bytes, "GC reclaimed leased screenshot");
        source = {};
        store.gc();
        require(!std::filesystem::exists(persistence::safe_blob_path(directory.path, hash)),
                "Screenshot lease leaked");
        const auto cached_id = store.admit("main", domain::Snapshot{{f}});
        rendering::ScreenshotPreviewService service(
            [&](const auto &e, auto s, auto d) { return repository.screenshot(e, s, d); });
        auto await = [&] {
            const auto end = until();
            while (std::chrono::steady_clock::now() < end) {
                if (auto result = service.take())
                    return *result;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            throw std::runtime_error("Preview service completion unavailable");
        };
        application::ScreenshotPreviewRequest request{store.entry(cached_id), 1, 100, 512, 512};
        service.request(request);
        require(bool(await().image), "Service decode failed");
        request.serial++;
        service.request(request);
        require(bool(await().image), "Service cache failed");
        require(service.counters().decodes == 1 && service.counters().hits == 1,
                "Warm preview decoded again");
        if (argc == 2 && std::string_view(argv[1]) == "--idle") {
            service.cancel();
            std::this_thread::sleep_for(std::chrono::milliseconds(31500));
            auto expired = service.counters();
            require(!expired.bytes, "Hover preview cache did not expire");
            std::this_thread::sleep_for(std::chrono::milliseconds(3200));
            require(service.counters().idle_wakes == expired.idle_wakes, "Expired hover worker kept waking");
            std::cout << "idle_wakes_at_expiry=" << expired.idle_wakes
                      << " post_expiry_wakes=" << service.counters().idle_wakes-expired.idle_wakes << '\n';
            return 0;
        }
        service.cancel(true);
        require(service.counters().bytes == 0 && !service.take(), "Purge retained pixels");
        std::atomic<unsigned> resolves{};
        std::atomic<bool> release{};
        rendering::ScreenshotPreviewService racing([&](const auto &e, auto s, auto d) {
            if (++resolves == 1)
                while (!release && std::chrono::steady_clock::now() < d)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return repository.screenshot(e, s, d);
        });
        request.serial = 401;
        racing.request(request);
        auto ready = until();
        while (!resolves && std::chrono::steady_clock::now() < ready)
            std::this_thread::yield();
        require(resolves == 1, "Delayed request did not start");
        request.serial = 402;
        racing.request(request);
        request.serial = 403;
        racing.request(request);
        release = true;
        bool latest{};
        ready = until();
        while (std::chrono::steady_clock::now() < ready) {
            if (auto response = racing.take()) {
                require(response->request.serial == 403 && response->image,
                        "Stale or intermediate preview escaped");
                latest = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(latest && resolves == 2, "Pending queue did not retain only the latest request");
        racing.cancel(true);
        auto damaged = repository.screenshot(store.entry(cached_id), {}, until());
        const auto blob = persistence::safe_blob_path(directory.path, damaged.hash);
        {
            std::fstream changed(blob, std::ios::binary | std::ios::in | std::ios::out);
            changed.put('!');
        }
        rejects([&] { damaged.read({}, until()); }, "Wrong blob hash was accepted");
        request.serial = 404;
        service.request(request);
        require(await().status == rendering::ScreenshotPreviewService::Status::content_failure,
                "Content failure status was not returned");
        const auto attempts = service.counters().decodes;
        request.serial = 405;
        service.request(request);
        require(!await().image && service.counters().decodes == attempts,
                "Failed content bypassed the negative cache");
        std::filesystem::remove(blob);
        rejects([&] { damaged.read({}, until()); }, "Missing blob was accepted");
        {
            std::ofstream repaired(blob, std::ios::binary);
            repaired.write(reinterpret_cast<const char *>(input.bytes.data()),
                           std::streamsize(input.bytes.size()));
        }
        auto settings = persistence::read_card_preferences(store.database());
        require(settings.screenshot_hover_preview, "Missing preference did not default on");
        settings.screenshot_hover_preview = false;
        persistence::save_card_preferences(store.database(), settings);
        require(!persistence::read_card_preferences(store.database()).screenshot_hover_preview,
                "Preference did not save");
        {
            Directory archive;
            store.database().backup_to(archive.path / "backup");
            persistence::Database::restore_backup(archive.path / "backup",
                                                  archive.path / "restored");
            persistence::HistoryStore restored(archive.path / "restored");
            require(
                !persistence::read_card_preferences(restored.database()).screenshot_hover_preview,
                "Backup/restore lost the optional preference");
        }
        persistence::exec(store.database().handle(),
                          "CREATE TRIGGER preview_fail BEFORE UPDATE ON "
                          "screenshot_preview_preferences BEGIN SELECT RAISE(ABORT,'test'); END");
        settings.screenshot_hover_preview = true;
        settings.font_size = 21;
        rejects([&] { persistence::save_card_preferences(store.database(), settings); },
                "Preference failure was accepted");
        require(!persistence::read_card_preferences(store.database()).screenshot_hover_preview &&
                    persistence::read_card_preferences(store.database()).font_size != 21,
                "Preference transaction did not roll back");
        persistence::exec(store.database().handle(), "DROP TRIGGER preview_fail");
        persistence::reset_global_preferences(store.database(), false);
        require(persistence::read_card_preferences(store.database()).screenshot_hover_preview,
                "Reset did not enable preview");
        for (UINT dpi : {96u, 144u, 192u})
            for (auto size : {SIZE{4096, 2048}, SIZE{100, 8192}, SIZE{8192, 100}, SIZE{2, 2}}) {
                RECT work{-1920, 0, 0, 1080}, card_rect{-1800, 100, -1600, 300};
                const auto layout =
                    win::screenshot_placement(card_rect, work, size.cx, size.cy, dpi);
                require(layout.has_value(), "Layout unavailable");
                RECT overlap{};
                require(!IntersectRect(&overlap, &layout->window, &card_rect) &&
                            layout->window.left >= work.left && layout->window.top >= work.top &&
                            layout->window.right <= work.right &&
                            layout->window.bottom <= work.bottom,
                        "Preview overlapped the card or escaped the work area");
                require(std::uint64_t(layout->width) * layout->height <= 2097152 &&
                            layout->width <= unsigned(size.cx) &&
                            layout->height <= unsigned(size.cy),
                        "Preview layout exceeded pixel bounds");
            }
        std::cout << "Screenshot sandbox formats, cancellation, IPC and header limits passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
