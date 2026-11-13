#include "../clipboard_sources/image_file_sample.hpp"
#include "../clipboard_sources/media_sample.hpp"
#include "cache/thumbnail_name.hpp"
#include "preview_worker/thumbnail.hpp"
#include "history_fixture.hpp"
#include "clipboard_process.hpp"
#include <algorithm>
#include "persistence/history_store.hpp"
#include "rendering/card_browser.hpp"
#include "platform_win/clipboard_object.hpp"
#include "platform_win/file_group.hpp"
#include "platform_win/virtual_files.hpp"
#include <wrl/client.h>
#include "platform_win/history_clipboard.hpp"
#include "platform_win/media.hpp"
#include <fstream>
#include <iostream>
#include <source_location>

using namespace cliprope;
using history_fixture::require;
namespace fs = std::filesystem;
namespace {
void write(const fs::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream out(path, std::ios::binary);
    require(bool(out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size())), "Fixture write failed");
}
class Source final : public ClipboardObject {
public:
    using ClipboardObject::ClipboardObject;
    bool read_pixels{};
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f, STGMEDIUM* medium) override {
        if (f && f->cfFormat == RegisterClipboardFormatW(L"PNG")) read_pixels = true;
        return ClipboardObject::GetData(f, medium);
    }
};
domain::Snapshot capture(const std::vector<fs::path>& paths) {
    Source* source = new Source({media_sample::png(), image_file_sample::drop(paths)});
    struct Release { Source* p; ~Release(){p->Release();} } release{source};
    require(SUCCEEDED(OleSetClipboard(source)), "Could not offer file fixture");
    bool busy{};
    auto captured = capture_offer(*source, GetClipboardSequenceNumber(), GetClipboardOwner(),
                                  CaptureMode::local_history, busy);
    require(captured.saved.size() == 2 && !source->read_pixels, "Capture read a file companion or lost the group");
    return win::capture_snapshot(std::move(captured));
}
template<class F> application::CardSummary await_card(rendering::CardBrowser& browser, const domain::Id& id, F ready,
                                                      std::source_location where = std::source_location::current()) {
    const auto until = GetTickCount64() + 5000;
    do {
        auto card = browser.summary(id, true);
        if (ready(card)) return card;
        Sleep(15);
    } while (GetTickCount64() < until);
    throw std::runtime_error("File preview did not reach the expected state at fixture line " + std::to_string(where.line()));
}
std::vector<unsigned char> solid_bmp(unsigned char blue, int width = 1, int height = 1) {
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER dib{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof file + sizeof dib;
    const auto stride = (width * 3 + 3) & ~3;
    file.bfSize = file.bfOffBits + stride * height;
    dib.biSize = sizeof dib;
    dib.biWidth = width;
    dib.biHeight = height;
    dib.biPlanes = 1;
    dib.biBitCount = 24;
    std::vector<unsigned char> bytes(file.bfSize);
    std::memcpy(bytes.data(), &file, sizeof file);
    std::memcpy(bytes.data() + sizeof file, &dib, sizeof dib);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) bytes[file.bfOffBits + y * stride + 3 * x] = blue;
    return bytes;
}
void full_hd_previews(persistence::HistoryStore& store, rendering::CardBrowser& browser, const fs::path& path) {
    const auto bitmap = solid_bmp(27, 1920, 1080);
    write(path, bitmap);
    domain::Format pixels;
    pixels.standard_id = CF_DIB;
    pixels.bytes.assign(bitmap.begin() + sizeof(BITMAPFILEHEADER), bitmap.end());
    const auto screenshot = store.admit("main", domain::Snapshot{{std::move(pixels)}});
    const auto file = store.admit("main", capture({path}));
    browser.summary(file, true);  // Queue a file decode just before switching to the ordinary file face.
    persistence::exec(store.database().handle(), "UPDATE app_settings SET image_file_previews=0");
    const auto has_thumbnail = [](const auto& summary) { return bool(summary.thumbnail); };
    const auto shot = await_card(browser, screenshot, has_thumbnail);
    const auto plain = await_card(browser, file, [](const auto& c) { return c.availability == domain::FileAvailability::available; });
    require(shot.source.width == 1920 && shot.source.height == 1080 && !plain.thumbnail &&
            plain.file_preview == application::CardSummary::FilePreviewKind::none,
            "Disabling file previews lost the screenshot dimensions or displayed an old file decode");
    persistence::exec(store.database().handle(), "UPDATE app_settings SET image_file_previews=1");
    const auto picture = await_card(browser, file, has_thumbnail);
    require(picture.source.width == 1920 && picture.source.height == 1080 && picture.thumbnail->width < 1920,
            "File preview used thumbnail dimensions instead of original dimensions");
    store.erase(file);
    store.erase(screenshot);
}
void screenshot_publication() {
    using Microsoft::WRL::ComPtr;
    const auto png = media_sample::png();
    domain::LeasedSnapshot image;
    domain::Format pixel;
    pixel.name = "PNG";
    pixel.bytes = png.bytes;
    image.snapshot.formats.push_back(pixel);
    win::HistoryClipboard clipboard;
    require(clipboard.publish(image) != 0 && !IsClipboardFormatAvailable(virtual_files::descriptor_format()),
            "Normal pixel recall advertised a file");
    require(clipboard.publish(image, true) != 0 && IsClipboardFormatAvailable(png.format),
            "Folder recall lost the saved image format");
    ComPtr<IDataObject> object;
    require(SUCCEEDED(OleGetClipboard(&object)), "Could not inspect the screenshot publication");
    FORMATETC file{static_cast<CLIPFORMAT>(virtual_files::contents_format()), nullptr, DVASPECT_CONTENT, 0, TYMED_ISTREAM};
    for (int i = 0; i < 2; ++i) {
        STGMEDIUM medium{};
        require(SUCCEEDED(object->GetData(&file, &medium)), "Screenshot stream unavailable");
        std::vector<unsigned char> bytes(png.bytes.size());
        ULONG read{};
        auto hr = medium.pstm->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read);
        ReleaseStgMedium(&medium);
        require(SUCCEEDED(hr) && read == bytes.size() && bytes == png.bytes, "Original PNG bytes or stream position changed");
    }
    file.lindex = 1;
    require(FAILED(object->QueryGetData(&file)), "Screenshot advertised extra files");
    require(clipboard.flush_if_current() == clipboard_partial_flush && IsClipboardFormatAvailable(png.format) &&
            !IsClipboardFormatAvailable(virtual_files::descriptor_format()), "Exit did not retain the portable PNG");
    domain::Format text;
    text.standard_id = CF_UNICODETEXT;
    text.bytes = {'x', 0, 0, 0};
    image.snapshot.formats.push_back(text);
    require(clipboard.publish(image, true) != 0 && !IsClipboardFormatAvailable(virtual_files::descriptor_format()),
            "A rich snapshot with a picture companion was exported as a screenshot");
    if (clipboard.owns_current()) OleSetClipboard(nullptr);
}

void file_cache_format(const fs::path& profile, const fs::path& file, std::string_view hash) {
    preview_worker::ThumbnailRequest request;
    request.op = preview_worker::ThumbnailOp::file_picture;
    request.width = 320;
    request.generation = 1;
    std::copy(hash.begin(), hash.end(), request.hash.begin());
    auto fetch = [&] { return preview_worker::run_thumbnail(request, profile, {}, {}); };
    const auto unsupported = profile.parent_path() / "unsupported-preview";
    {
        persistence::Database db(unsupported);
        persistence::exec(db.handle(), "PRAGMA application_id=123");
    }
    const auto digest = persistence::file_sha256(unsupported / "history.db");
    try {
        preview_worker::run_thumbnail(request, unsupported, {}, {});
        throw std::logic_error("Thumbnail worker accepted a foreign database");
    } catch (const std::runtime_error& error) {
        require(std::string_view(error.what()).starts_with("Unsupported history database version"),
                "Wrong thumbnail diagnostic");
    }
    require(digest == persistence::file_sha256(unsupported / "history.db") &&
            !fs::exists(unsupported / "thumbnails"), "Thumbnail worker changed a foreign database");

    const auto first = fetch();
    require(first.decodes == 1 && !first.image.pbgra.empty(), "File cache was not populated");
    const auto cache_file = profile / "thumbnails" / (cache::thumbnail_key(hash, cache::files, 320) + ".thumb");
    std::ifstream stream(cache_file, std::ios::binary);
    std::vector<unsigned char> old(std::istreambuf_iterator<char>(stream), {});
    stream.close();
    require(old.size() >= 28 && old[3] == '2', "Missing current file thumbnail");
    old[3] = '1'; // CRF1 omitted the three source dimension/orientation words.
    old.erase(old.begin() + 16, old.begin() + 28);
    write(cache_file, old);
    const auto away = file.parent_path() / "temporarily-missing.png";
    fs::rename(file, away);
    const auto missing = fetch();
    require(missing.file.availability == domain::FileAvailability::missing && missing.image.pbgra.empty(),
            "Offline file exposed obsolete cache pixels");
    fs::rename(away, file);
    const auto rebuilt = fetch();
    require(rebuilt.decodes == 1 && rebuilt.image.pbgra == first.image.pbgra && fetch().decodes == 0,
            "Returning file did not regenerate and reuse its current cache");
}

void exercise(const fs::path& jpeg) {
    screenshot_publication();
    // The encoded 8 x 4 JPEG has a red left half, a blue right half and EXIF orientation 6.
    auto oriented = rendering::decode_file_preview(jpeg.wstring(), 4, 8);
    require(oriented.source.width == 4 && oriented.source.height == 8 && oriented.width == 4 && oriented.height == 8 && oriented.pbgra[2] > 240 &&
            oriented.pbgra[7 * 4 * 4] > 240, "JPEG preview lost EXIF orientation");
    history_fixture::Directory dir;
    const auto profile = dir.path / "profile";
    std::vector<fs::path> files;
    for (int i = 0; i < 6; ++i) {
        files.push_back(dir.path / (std::to_string(i) + (i == 0 ? ".PNG" : ".bmp")));
        write(files.back(), i ? solid_bmp(static_cast<unsigned char>(20 * i)) : media_sample::png().bytes);
    }
    {
        const auto path = files.front().wstring();
        const auto* data = reinterpret_cast<const BYTE*>(path.c_str());
        Representation filename{RegisterClipboardFormatW(L"FileNameW"), {data, data + (path.size() + 1) * sizeof(wchar_t)}};
        Source* source = new Source({media_sample::png(), filename});
        struct Release { Source* p; ~Release() { p->Release(); } } release{source};
        require(SUCCEEDED(OleSetClipboard(source)), "Could not offer path-bearing screenshot");
        bool busy{};
        auto received = capture_offer(*source, GetClipboardSequenceNumber(), GetClipboardOwner(), CaptureMode::local_history, busy);
        require(received.screenshot_path == path && received.saved.size() == 1 &&
                received.saved.front().format == RegisterClipboardFormatW(L"PNG"), "Screenshot origin became a file list or was lost");
    }
    domain::Id single, multiple;
    {
        persistence::HistoryStore store(profile);
        single = store.admit("main", capture({files.front()}));
        multiple = store.admit("main", capture(files));
        require(store.hanging("main").size() == 2, "A group occupied extra history slots");
    }
    bool reject_commit = false;
    persistence::HistoryStore store(profile, [&](std::string_view point) {
        if (reject_commit && point == "transaction") throw std::runtime_error("Injected history rollback");
    });
    const auto single_snapshot = store.load(single).snapshot;
    const auto paths = std::find_if(single_snapshot.formats.begin(), single_snapshot.formats.end(),
                                    [](const auto& f) { return f.standard_id == CF_HDROP; });
    require(paths != single_snapshot.formats.end(), "File sample lost its path representation");
    file_cache_format(profile, files.front(), persistence::sha256(paths->bytes));
    rendering::CardBrowser browser{persistence::Browser(store)};
    using Kind = application::CardSummary::FilePreviewKind;
    using enum domain::FileAvailability;
    auto remote = image_file_sample::drop({fs::path(LR"(\\cliprope.invalid\share\photo.png)")});
    domain::Format remote_paths;
    remote_paths.standard_id = CF_HDROP;
    remote_paths.bytes = std::move(remote.bytes);
    const auto remote_id = store.admit("main", domain::Snapshot{{std::move(remote_paths)}});
    await_card(browser, remote_id, [](const auto& c){return c.availability == on_request && !c.thumbnail;});
    store.erase(remote_id);
    require(!browser.summary(single).thumbnail && browser.thumbnail_bytes() == 0, "A summary-only read requested an image");
    auto valid = [](const auto& c){return c.availability == available && c.thumbnail;};
    auto card = await_card(browser, single, valid);
    require(card.kind == application::CardKind::files && card.file_preview == Kind::picture &&
            card.text == files.front().filename() && card.thumbnail->width == 2 && card.source.width == 2, "Single-file summary mismatch");
    // The status-only request has a separate cache key, including while an older decode completes.
    persistence::exec(store.database().handle(), "UPDATE app_settings SET image_file_previews=0,hover_resolution=0");
    auto plain = await_card(browser, single, [](const auto& c){return c.availability == available;});
    require(!plain.thumbnail && plain.file_preview == Kind::none && !plain.hover_resolution &&
            plain.menu == application::CardMenuKind::single_file, "Disabled file preview reused a cached picture");
    auto group = await_card(browser, multiple, valid);
    persistence::exec(store.database().handle(), "UPDATE app_settings SET image_file_previews=1");
    card = await_card(browser, single, valid);
    require(card.file_preview == Kind::picture && !card.hover_resolution, "File and resolution switches were coupled");
    require(group.items == 6 && group.file_preview == Kind::collage && group.thumbnail->width == 320,
            "Group did not produce one four-tile sheet");
    for (unsigned i = 1; i < 4; ++i) {
        const auto x = (i % 2) * 160 + 80, y = (i / 2) * 128 + 64;
        require(group.thumbnail->pbgra[(y * 320 + x) * 4] == 20 * i, "Collage reordered or duplicated an image");
    }
    full_hd_previews(store, browser, dir.path / "Full HD.bmp");
    auto snapshot = store.load(multiple).snapshot;
    const auto extra = media_sample::png();
    domain::Format companion;
    companion.name = "PNG";
    companion.bytes = extra.bytes;
    snapshot.formats.insert(snapshot.formats.begin(), companion); // Older, multi-format history.
    auto restored = win::restorable_formats(snapshot);
    auto refs = std::find_if(restored.begin(), restored.end(), [](const auto& f){return f.format == CF_HDROP;});
    require(restored.size() == 2 && refs != restored.end() && file_group_paths(*refs) ==
            std::vector<std::wstring>(files.begin(), files.end()), "Legacy restore exposed pixels or lost path order");

    ClipboardPublisher sentinel;
    require(SUCCEEDED(sentinel.publish({{CF_UNICODETEXT,{'X',0,0,0}}})), "Could not set newer copy");
    win::HistoryClipboard clipboard;
    fs::rename(files.back(), dir.path / "moved.png");
    browser.recheck_files();
    group = await_card(browser, multiple, [](const auto& c){return c.availability == missing;});
    require(group.thumbnail && !clipboard.publish(store.load(multiple)) && sentinel.is_current(),
            "Stale group thumbnail restored a missing file");
    fs::rename(dir.path / "moved.png", files.back());
    browser.recheck_files();
    await_card(browser, multiple, valid);
    require(clipboard.publish(store.load(multiple)) != 0, "Returning a member did not restore file copy");

    const auto attrs = GetFileAttributesW(files.front().c_str());
    require(SetFileAttributesW(files.front().c_str(), attrs | FILE_ATTRIBUTE_OFFLINE) != FALSE,
            "Could not simulate an offline original");
    browser.recheck_files();
    card = await_card(browser, single, [](const auto& c){return c.availability == inaccessible;});
    require(card.thumbnail && !clipboard.publish(store.load(single)), "Offline reference was offered as a file");
    require(SetFileAttributesW(files.front().c_str(), attrs) != FALSE, "Could not reconnect the fixture");
    browser.recheck_files();
    await_card(browser, single, valid);
    write(files.front(), {1, 2, 3});
    browser.recheck_files();
    await_card(browser, single, [](const auto& c){return c.availability == decode_failed && !c.thumbnail;});
    require(clipboard.publish(store.load(single)) != 0, "Decode failure blocked a valid file copy");
    write(files.front(), extra.bytes);
    browser.recheck_files();
    await_card(browser, single, valid);
    for (const auto& f : fs::directory_iterator(profile / "thumbnails")) write(f.path(), {0});
    browser.recheck_files();
    const auto previous = browser.summary(single, true).thumbnail;
    await_card(browser, single, [&](const auto& c) { return c.thumbnail && c.thumbnail != previous; });
    const auto target = store.create_rope("Destination", 1);
    const auto tail = store.copy(single, target);
    store.detach(multiple);
    auto invoke = [&] {
        auto cmd = application::bind_invocation(store, application::Route::return_button, multiple, target);
        return cmd.run(store, [&](const auto& content) { return clipboard.publish(content); });
    };
    reject_commit = true;
    auto result = invoke();
    require(result.publication == application::Publication::skipped && result.history == application::HistoryEffect::failed &&
            !store.entry(multiple).rope_id && store.hanging(target).front().id == tail && clipboard.owns_current(),
            "Failed return changed its Floating entry, clipboard or target tail");
    reject_commit = false;
    result = invoke();
    const auto members = store.hanging(target);
    require(result.history == application::HistoryEffect::committed && members.size() == 1 &&
            members.front().id == multiple && store.hanging("main").front().id == single,
            "File return did not evict exactly the bound rope tail");
    store.erase(single);
    store.erase(multiple);
    store.gc();
    require(fs::is_empty(profile / "thumbnails") && fs::exists(files.front()), "GC retained a thumbnail or removed its source");
    if (clipboard.owns_current()) OleSetClipboard(nullptr);
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || !probe::private_station() || FAILED(OleInitialize(nullptr))) return 2;
    int result = 0;
    try { exercise(argv[1]); std::cout << "Capture, legacy restore, collage, alpha, revalidation and GC passed\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; result = 1; }
    OleSetClipboard(nullptr); // The fixture owns this private test station, including on failure.
    OleUninitialize();
    return result;
}
