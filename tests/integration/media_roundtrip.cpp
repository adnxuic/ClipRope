#include "presentation/card_labels.hpp"
#include "../clipboard_sources/media_sample.hpp"
#include "../clipboard_sources/external_copy.hpp"
#include "history_fixture.hpp"
#include "persistence/history_store.hpp"
#include "rendering/card_browser.hpp"
#include "platform_win/clipboard_object.hpp"
#include "platform_win/restore_details.hpp"
#include "rendering/image_preview.hpp"
#include "rendering/workspace_paint.hpp"
#include <algorithm>
#include <set>

using namespace cliprope;
namespace {
namespace fs = std::filesystem;
using probe::require;
struct ReleaseOwn {
    std::function<bool()> current;
    ~ReleaseOwn() {
        if (current())
            OleSetClipboard(nullptr);
    }
};
void clear_fixture_envelope() {
    // Some service stations retain OLE bookkeeping after the final fixture's
    // publisher exits. Only reclaim the exact ownerless, payload-free shape.
    if (!probe::private_station() || !OpenClipboard(nullptr))
        return;
    if (!GetClipboardOwner()) {
        std::set<std::wstring> names;
        for (UINT id = 0; (id = EnumClipboardFormats(id)) != 0;)
            names.insert(format_name(id));
        if (names == std::set<std::wstring>{L"DataObject", L"Ole Private Data", L"CF_LOCALE"})
            EmptyClipboard();
    }
    CloseClipboard();
}
int source(std::wstring_view mode, const std::wstring& key) {
    auto ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, (key + L"-ready").c_str());
    auto stop = OpenEventW(SYNCHRONIZE, FALSE, (key + L"-stop").c_str());
    auto* obj = new ClipboardObject(media_sample::formats(mode));
    require(ready && stop && SUCCEEDED(OleSetClipboard(obj)), "Synthetic media source failed");
    SetEvent(ready);
    probe::pump_until([&] { return WaitForSingleObject(stop, 0) == WAIT_OBJECT_0; }, "Source not released");
    if (OleIsCurrentClipboard(obj) == S_OK)
        OleSetClipboard(nullptr);
    obj->Release();
    CloseHandle(ready);
    CloseHandle(stop);
    return 0;
}
void pixels(const rendering::ImagePreview& read) {
    require(read.width == 2 && read.height == 2, "Image dimensions changed");
    const std::vector<unsigned char> expected{0,   0, 255, 255, 0,   255, 0,   255,
                                              255, 0, 0,   255, 255, 255, 255, 255};
    require(read.pbgra == expected, "Decoded RGBA samples differ from the four known pixels");
}
void seed(const fs::path& profile, const wchar_t* exe) {
    persistence::HistoryStore store(profile);
    win::HistoryClipboard clipboard;
    win::HistoryCapture reader(store, clipboard);
    for (const auto mode : {L"rich", L"image", L"partial", L"bad"}) {
        probe::ExternalCopy copy(exe, mode);
        auto outcome = reader.observe();
        probe::pump_until(
            [&] {
                for (auto& item : reader.poll())
                    outcome = std::move(item);
                return !reader.busy();
            },
            "Media capture stalled");
        std::wcout << mode << L": ";
        std::cout << outcome.status << " " << outcome.detail << std::endl;
        if (std::wstring_view(mode) == L"bad")
            require(outcome.entry.empty(), "All-invalid object admitted to history");
        else
            require(!outcome.entry.empty(), "A usable media object was dropped");
        ClipboardPublisher overwrite;
        ReleaseOwn release{[&] { return overwrite.is_current(); }};
        require(SUCCEEDED(overwrite.publish(sample::formats(true))), "Clipboard overwrite failed");
    }
    require(store.hanging("main").size() == 3, "Incorrect total admissions");
}
void validate_form(const CaptureResult& read, const std::vector<Representation>& expected) {
    for (const auto& wanted : expected) {
        auto found = std::find_if(read.saved.begin(), read.saved.end(),
                                  [&](const auto& f) { return f.format == wanted.format; });
        require(found != read.saved.end() && *found == wanted, "Published bytes or format media changed");
        if (wanted.format == CF_DIB || wanted.format == CF_DIBV5 ||
            wanted.format == RegisterClipboardFormatW(L"PNG"))
            pixels(rendering::decode_preview(found->format, found->bytes));
    }
}
void child(const wchar_t* exe, const wchar_t* mode, const fs::path& path);
void inspect(int index) {
    auto read = capture_clipboard(GetClipboardSequenceNumber(), CaptureMode::all_formats_probe);
    const std::vector<Representation> expected = index == 0   ? std::vector{sample::formats(false).front()}
                                                 : index == 1 ? std::vector{media_sample::stream_png()}
                                                              : media_sample::rich();
    validate_form(read, expected);
    require(read.saved.size() == expected.size(), "Advertisement contains an unexpected supported format");
    if (index == 1) {
        FORMATETC format{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"PNG")), nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM};
        IDataObject* object{};
        STGMEDIUM medium{};
        require(SUCCEEDED(OleGetClipboard(&object)) && object, "OLE PNG object unavailable");
        const auto hr = object->GetData(&format, &medium);
        object->Release();
        require(SUCCEEDED(hr) && medium.tymed == TYMED_ISTREAM, "PNG stream transport unavailable");
        ReleaseStgMedium(&medium);
    } else if (index == 2) {
        require(OpenClipboard(nullptr), "Native bitmap reader could not open clipboard");
        const auto handle = GetClipboardData(CF_DIB);
        const auto* bytes = handle ? static_cast<const unsigned char*>(GlobalLock(handle)) : nullptr;
        const auto dib = media_sample::dib(false);
        const bool matches = bytes && GlobalSize(handle) >= dib.bytes.size() &&
                             std::equal(dib.bytes.begin(), dib.bytes.end(), bytes);
        if (bytes) GlobalUnlock(handle);
        CloseClipboard();
        require(matches, "Native bitmap reader did not receive the saved DIB bytes");
    }
}
void restore(const fs::path& profile, const wchar_t* exe) {
    persistence::HistoryStore store(profile);
    rendering::CardBrowser browser{persistence::Browser(store)};
    win::HistoryClipboard clipboard;
    ReleaseOwn release{[&] { return clipboard.owns_current(); }};
    const auto entries = store.hanging("main");
    require(entries.size() == 3, "Independent process failed to restore entries");
    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto lease = store.load(entries[i].id);
        auto summary = cliprope::presentation::english_card(browser.summary(entries[i].id));
        require(!summary.thumbnail && browser.thumbnail_bytes() == 0, "Summary-only query loaded pixels");
        auto plan = win::restorable_formats(lease.snapshot);
        std::cout << "restore index=" << i << " manifest=" << lease.snapshot.formats.size()
                  << " plan=" << plan.size() << std::endl;
        for (const auto& f : lease.snapshot.formats)
            std::cout << "  f=" << f.standard_id << " registered=" << f.name << " status=" << f.outcome
                      << " bytes=" << f.bytes.size() << " tymed=" << f.tymed << " aspect=" << f.aspect
                      << " index=" << f.index << " codec=" << f.codec_version << std::endl;
        const auto details = win::restore_details(lease.snapshot, browser.manifest(entries[i].id));
        if (!clipboard.publish(lease)) {
            std::cerr << "Publication failed at index " << i << " plan size=" << plan.size()
                      << " owner=" << bool(GetClipboardOwner()) << " open=" << bool(GetOpenClipboardWindow())
                      << " sequence=" << GetClipboardSequenceNumber() << '\n';
            throw std::runtime_error("Actual clipboard publication failed");
        }
        child(exe, L"--inspect", std::to_wstring(i));
        if (i == 0) {
            require(lease.snapshot.capture_status == "partial" && plan.size() == 1 &&
                        plan[0].format == CF_UNICODETEXT &&
                        summary.capabilities.find(L"partial") != std::wstring::npos &&
                        details.find(L"HTML Format — failed") != std::wstring::npos &&
                        details.find(L"CF_DIB — failed") != std::wstring::npos,
                    "Malformed markup/oversized DIB advertised");
        } else if (i == 1) {
            require(plan.size() == 1 && summary.capabilities == L"Image" &&
                        details.find(L"PNG · image pixels · IStream") != details.npos,
                    "Image-only capability did not match readable formats");
        } else {
            const auto saved = media_sample::rich();
            require(plan.size() == saved.size(), "Unexpected rich format count");
            require(summary.capabilities.find(L"HTML") != summary.capabilities.npos &&
                        summary.capabilities.find(L"RTF") != summary.capabilities.npos &&
                        details.find(L"Office-private fixture — skipped") != details.npos,
                    "Rich capabilities inaccurate");
        }
    }
    std::shared_ptr<const domain::ImagePreview> image;
    probe::pump_until(
        [&] {
            image = cliprope::presentation::english_card(browser.summary(entries[1].id, true)).thumbnail;
            return bool(image);
        },
        "Thumbnail unavailable");
    pixels(*image);
    require(browser.thumbnail_bytes() == 16, "Unexpected thumbnail cache charge");
    require(fs::exists(profile / "thumbnails") && !fs::is_empty(profile / "thumbnails"),
            "Disk thumbnail was not populated");
    std::cout << "Three persisted entries; six rich media readable; PNG/DIB/DIBV5 2x2 red/green/blue/white "
                 "pixels exact; cache 16 bytes\n";
}
void child(const wchar_t* exe, const wchar_t* mode, const fs::path& path) {
    auto process = probe::launch({exe, mode, path.wstring()}, probe::desktop_path());
    require(process, "Could not start fresh host");
    probe::pump_until([&] { return WaitForSingleObject(process, 0) == WAIT_OBJECT_0; },
                      "Media worker timed out");
    DWORD code = 1;
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    require(code == 0, "Media test child failed");
}
void cache_gc(const fs::path& root) {
    persistence::HistoryStore store(root);
    auto entries = store.hanging("main");
    require(entries.size() == 3, "Missing cache GC fixture");
    const auto dir = root / "thumbnails";
    std::size_t before = 0;
    for (const auto& item : fs::directory_iterator(dir))
        if (item.path().extension() == ".thumb")
            ++before;
    require(before == 1, "Unexpected initial derivative count");
    store.detach(entries[2].id);
    store.erase(entries[1].id);
    store.gc();
    require(!fs::is_empty(dir), "Shared Floating-owned thumbnail collected early");
    store.erase(entries[2].id);
    store.gc();
    require(fs::is_empty(dir), "Last-reference thumbnail not reclaimed");
}
void offline_cache() {
    history_fixture::Directory dir;
    {
        persistence::HistoryStore store(dir.path);
        CaptureResult original;
        original.outcome = "captured";
        original.saved = media_sample::rich();
        for (const auto& f : original.saved)
            original.formats.push_back({f.format, f.tymed, f.aspect, f.index, "saved"});
        const auto id = store.admit("main", win::capture_snapshot(std::move(original)));
        auto copy = store.copy(id, "main");
        store.detach(copy);
        rendering::CardBrowser browser{persistence::Browser(store)};
        auto recovered = win::restorable_formats(store.load(id).snapshot);
        require(recovered.size() == 6, "Offline restore plan rejected saved rich data");
        std::shared_ptr<const domain::ImagePreview> thumb;
        probe::pump_until(
            [&] {
                thumb = cliprope::presentation::english_card(browser.summary(id, true)).thumbnail;
                return bool(thumb);
            },
            "Disk thumbnail did not materialize");
        pixels(*thumb);
        application::CardSummary card = cliprope::presentation::english_card(browser.summary(id, true));
        card.text = card.capabilities = L"Image";
        card.image_primary = true;
        card.source = thumb->source;
        require(card.source.width == 2 && card.source.height == 2, "Cached source dimensions missing");
        // At 400% DPI the 3-DIP-wide body occupies twelve pixels. Sample
        // inside each corner to avoid the sub-DIP outline around the image.
        constexpr int dpi = 384, width = 144 * 4, height = 41 * 4;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        HDC dc = CreateCompatibleDC(nullptr);
        void* bits{};
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        require(dc && bitmap && bits, "No thumbnail test surface");
        auto previous = SelectObject(dc, bitmap);
        const bool painted =
            rendering::WorkspacePaint::acquire()->draw_floating(dc, {0, 0, width, height}, dpi, card);
        GdiFlush();
        const int x = (width - 12) / 2, y = 38 * 4;
        const COLORREF red = GetPixel(dc, x + 2, y + 2), green = GetPixel(dc, x + 9, y + 2),
                       blue = GetPixel(dc, x + 2, y + 9);
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
        require(painted && GetRValue(red) > 240 && GetGValue(red) < 16 && GetGValue(green) > 240 &&
                    GetBValue(blue) > 240,
                "Native card did not draw decoded preview pixels");
        auto descendants = dir.path / "thumbnails";
        require(!fs::is_empty(descendants), "No disk thumbnail");
        store.erase(id);
        store.gc();
        require(!fs::is_empty(descendants), "Floating-only image lost its preview");
        store.erase(copy);
        store.gc();
        require(fs::is_empty(descendants), "Orphan thumbnail remained");
    }
    std::cout << "Offline snapshot plan, WIC thumbnail, shared and Floating-only GC checks passed\n";
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    const bool offline = argc == 2 && std::wstring_view(argv[1]) == L"--cache-only";
    const bool desktop = argc == 2 && std::wstring_view(argv[1]) == L"--desktop-roundtrip";
    const auto surface = probe::desktop_path();
    if (desktop && !SetEnvironmentVariableW(L"CLIPROPE_MEDIA_TEST_DESKTOP", surface.c_str()))
        return 2;
    wchar_t allowed[512]{};
    GetEnvironmentVariableW(L"CLIPROPE_MEDIA_TEST_DESKTOP", allowed, static_cast<DWORD>(std::size(allowed)));
    if ((!offline && !probe::private_station() && !(surface == allowed && *allowed)) ||
        FAILED(OleInitialize(nullptr)))
        return 2;
    int result = 0;
    try {
        if (offline)
            offline_cache();
        else if (argc == 1 || desktop) {
            history_fixture::Directory dir;
            child(argv[0], L"--capture", dir.path);
            child(argv[0], L"--restore", dir.path);
            fs::remove_all(dir.path / "thumbnails");
            child(argv[0], L"--restore", dir.path);
            child(argv[0], L"--gc", dir.path);
        } else if (argc == 3 && std::wstring_view(argv[1]) == L"--capture")
            seed(argv[2], argv[0]);
        else if (argc == 3 && std::wstring_view(argv[1]) == L"--restore")
            restore(argv[2], argv[0]);
        else if (argc == 3 && std::wstring_view(argv[1]) == L"--inspect")
            inspect(_wtoi(argv[2]));
        else if (argc == 3 && std::wstring_view(argv[1]) == L"--gc")
            cache_gc(argv[2]);
        else if (argc == 3)
            result = source(argv[1], argv[2]);
        else
            result = 2;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        result = 1;
    }
    if (argc == 1)
        clear_fixture_envelope();
    OleUninitialize();
    return result;
}
