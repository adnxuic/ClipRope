#include "platform_win/external_drag.hpp"
#include "platform_win/card_files.hpp"
#include "platform_win/clipboard_object.hpp"
#include "platform_win/file_group.hpp"
#include "platform_win/history_clipboard.hpp"
#include "platform_win/image_file_access.hpp"
#include "rendering/image_preview.hpp"
#include "rendering/workspace_paint.hpp"
#include <shlobj.h>
#include <wrl.h>
#include <algorithm>
#include <mutex>
#include <utility>
#include "preview_worker/thumbnail.hpp"

namespace cliprope::win {
#ifdef CLIPROPE_DRAG_FIXTURE
bool drag_button_down();
static bool reserve_escape(HWND) { return true; } // The private input fixture supplies Escape explicitly.
HRESULT drag_target(IDataObject*, IDropSource*, DWORD, DWORD*);
#else
static bool drag_button_down() { return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0; }
static bool reserve_escape(HWND listener) {
    return RegisterHotKey(listener, ExternalDragSession::escape_hotkey, MOD_NOREPEAT, VK_ESCAPE) != FALSE;
}
static constexpr auto drag_target = DoDragDrop;
#endif
namespace {
using Microsoft::WRL::ComPtr;
class DropSource final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDropSource> {
public:
    explicit DropSource(const bool& canceled) : canceled_(canceled) {}
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape || canceled_ || (GetAsyncKeyState(VK_ESCAPE) & 0x8000) || (keys & MK_RBUTTON))
            return DRAGDROP_S_CANCEL;
        return (keys & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
private:
    const bool& canceled_;
};

// Pin the verified native file as well as the snapshot for deferred Shell reads.
bool add_original(const std::filesystem::path& path, const Representation& pixels,
                  std::vector<Representation>& formats, std::shared_ptr<void>& lease) {
    const auto name = path.wstring();
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!disk_path(name) || !image_file_group({name}) ||
        inspect_image_file(name, info) != domain::FileAvailability::available) return false;
    auto handle = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_NO_RECALL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    std::shared_ptr<void> pinned(handle, [lease](void* file) { CloseHandle(file); });
    if (!rendering::matches_image_file(name, pixels.format, pixels.bytes)) return false;
    formats.push_back({CF_HDROP, encode_file_group({name})});
    formats.push_back(copy_drop_effect());
    lease = std::move(pinned);
    return true;
}

void set_preview(IDataObject* object, const application::CardSummary& card) {
    ComPtr<IDragSourceHelper> helper;
    if (FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper)))) return;
    const RECT bounds{0, 0, 144, 100};
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), bounds.right, -bounds.bottom, 1, 32, BI_RGB};
    void* pixels{};
    const HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap) return;
    const auto dc = CreateCompatibleDC(nullptr);
    if (dc) {
        const auto previous = SelectObject(dc, bitmap);
        bool drawn = false;
        try { drawn = rendering::WorkspacePaint::acquire()->draw_floating(dc, bounds, 72, card); }
        catch (const std::exception&) { /* The system cursor also indicates copy or rejection. */ }
        SelectObject(dc, previous);
        DeleteDC(dc);
        SHDRAGIMAGE preview{{bounds.right, bounds.bottom}, {12, 12}, bitmap, CLR_NONE};
        if (drawn && SUCCEEDED(helper->InitializeFromBitmap(&preview, object))) return;
    }
    DeleteObject(bitmap);
}
}

// The pool callback owns bytes and a guarded notification address. The store, snapshot lease,
// drop source and COM interfaces stay on the STA even when a canceled encoder exits late.
struct ExternalDragSession::Encoder {
    std::mutex mutex;
    HWND notify{};
    UINT format{};
    std::uint64_t serial{};
    std::vector<unsigned char> dib, png;
    std::stop_source stop;
    bool done{};
    static void CALLBACK execute(PTP_CALLBACK_INSTANCE, void* context) noexcept {
        std::unique_ptr<std::shared_ptr<Encoder>> owner(static_cast<std::shared_ptr<Encoder>*>(context));
        auto& job = **owner;
        std::vector<unsigned char> result;
        try { result = preview_worker::encode_drag_png(job.format, job.dib, job.serial, job.stop.get_token()); }
        catch (...) { /* An empty result is the failed preparation receipt. */ }
        std::lock_guard lock(job.mutex);
        job.png = std::move(result);
        job.done = true;
        if (job.notify) PostMessageW(job.notify, ready_message, WPARAM(job.serial), 0);
    }
};

struct ExternalDragSession::Pending {
    domain::Entry entry;
    application::CardSummary card;
    domain::LeasedSnapshot content;
    std::vector<Representation> formats;
    HWND surface{};
    DWORD sequence{};
    bool pixels{};
    size_t dib_index{};
    std::shared_ptr<Encoder> encoder;
};

ExternalDragSession::ExternalDragSession() = default;
ExternalDragSession::~ExternalDragSession() { cancel(); }
void ExternalDragSession::release_input() {
    if (!listener_) return;
    const auto listener = std::exchange(listener_, nullptr);
    UnregisterHotKey(listener, escape_hotkey);
    KillTimer(listener, ready_message);
    if (GetCapture() == listener) ReleaseCapture();
}
bool ExternalDragSession::cancel() {
    if (modal_) {
        canceled_ = true;
        if (const auto capture = GetCapture()) {
            POINT p{};
            if (GetCursorPos(&p) && ScreenToClient(capture, &p))
                PostMessageW(capture, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(p.x, p.y));
        }
    }
    if (!pending_) { if (listener_) canceled_ = true; return false; }
    if (const auto& encoder = pending_->encoder) {
        std::lock_guard lock(encoder->mutex);
        encoder->notify = nullptr;
        encoder->stop.request_stop();
    }
    pending_.reset();
    release_input();
    return true;
}
std::uint64_t ExternalDragSession::replace() {
    cancel();
    return ++serial_;
}
std::string_view ExternalDragSession::run(HWND listener, HWND surface, persistence::HistoryStore& store,
        const domain::Entry& expected, const application::CardSummary& card, std::uint64_t serial, DWORD sequence) {
    if (serial != serial_ || !IsWindow(surface)) return "owner-replaced";
    if (listener_ || modal_ || pending_) return "busy";
    if (GetClipboardSequenceNumber() != sequence) return "clipboard-changed";
    if (!drag_button_down()) return "released";
    if (!reserve_escape(listener))
        throw std::runtime_error("Escape is unavailable for this drag");
    listener_ = listener;
    canceled_ = false;
    try {
        domain::Entry current;
        try { current = store.entry(expected.id); }
        catch (const std::exception&) { release_input(); return "entry-unavailable"; }
        if (current.snapshot_id != expected.snapshot_id || current.revision != expected.revision ||
            current.rope_id != expected.rope_id) { release_input(); return "entry-changed"; }
        auto data = std::make_unique<Pending>();
        auto& p = *data;
        p.entry = expected;
        p.card = card;
        p.surface = surface;
        p.sequence = sequence;
        p.content = store.load_snapshot(expected.snapshot_id);
        p.formats = restorable_formats(p.content.snapshot);
        if (p.formats.empty()) throw std::runtime_error("No available formats to drag");
        const UINT png = RegisterClipboardFormatW(L"PNG");
        p.pixels = std::ranges::all_of(p.formats, [png](const auto& f) {
            return f.format == png || f.format == CF_DIBV5 || f.format == CF_DIB;
        });
        if (serial != serial_ || canceled_) { release_input(); return "owner-replaced"; }
        pending_ = std::move(data);
        if (!p.pixels || std::ranges::find(p.formats, png, &Representation::format) != p.formats.end())
            return deliver(store);
        if (!last_encoder_.expired()) { cancel(); return "encoder-busy"; }
        auto dib = std::ranges::find(p.formats, UINT(CF_DIBV5), &Representation::format);
        if (dib == p.formats.end()) dib = p.formats.begin();
        p.dib_index = static_cast<size_t>(dib - p.formats.begin());
        p.encoder = std::make_shared<Encoder>();
        last_encoder_ = p.encoder;
        auto& job = *p.encoder;
        job.notify = listener;
        job.format = dib->format;
        job.serial = serial;
        job.dib = std::move(dib->bytes);
        auto owner = std::make_unique<std::shared_ptr<Encoder>>(p.encoder);
        SetCapture(listener);
        if (!pending_ || GetCapture() != listener || !SetTimer(listener, ready_message, 3500, nullptr)) {
            cancel(); return "owner-replaced";
        }
        if (!TrySubmitThreadpoolCallback(Encoder::execute, owner.get(), nullptr))
            throw std::runtime_error("Could not schedule external drag preparation");
        owner.release();
        return "preparing";
    } catch (...) { cancel(); release_input(); throw; }
}

std::string_view ExternalDragSession::finish(persistence::HistoryStore& store, std::uint64_t serial) {
    if (serial != serial_ || !pending_ || !pending_->encoder) return {};
    const auto encoder = pending_->encoder;
    {
        std::lock_guard lock(encoder->mutex);
        if (!encoder->done) return {};
        pending_->formats[pending_->dib_index].bytes = std::move(encoder->dib);
    }
    if (encoder->png.empty()) { cancel(); return "preparation-failed"; }
    pending_->formats.insert(pending_->formats.begin(), {RegisterClipboardFormatW(L"PNG"), std::move(encoder->png)});
    return deliver(store);
}

std::string_view ExternalDragSession::deliver(persistence::HistoryStore& store) {
    auto data = std::move(pending_);
    struct Reset {
        ExternalDragSession& session;
        ~Reset() { session.modal_ = false; session.release_input(); }
    } reset{*this};
    auto& [expected, card, content, formats, surface, sequence, virtual_png, index, encoder] = *data;
    auto valid = [&]() -> std::string_view {
        if (canceled_ || !IsWindow(surface)) return "owner-replaced";
        if (GetClipboardSequenceNumber() != sequence) return "clipboard-changed";
        if (!drag_button_down()) return "released";
        try {
            const auto entry = store.entry(expected.id);
            if (entry.snapshot_id == expected.snapshot_id && entry.revision == expected.revision &&
                entry.rope_id == expected.rope_id) return {};
        } catch (const std::exception&) {}
        return "entry-changed";
    };
    if (auto stale = valid(); !stale.empty()) return stale;
    modal_ = true;
    if (GetCapture() == listener_) ReleaseCapture();
    const UINT png = RegisterClipboardFormatW(L"PNG");
    if (virtual_png) {
        const auto image = std::ranges::find(formats, png, &Representation::format);
        const auto path = find_snip_original(store, expected, content.snapshot);
        virtual_png = !add_original(path, *image, formats, content.lease);
    }
    // Native editors negotiate different media for the same registered PNG format.
    for (auto& item : formats)
        if (item.format == png) item.tymed = TYMED_HGLOBAL | TYMED_ISTREAM;
    ComPtr<IDataObject> saved;
    saved.Attach(new ClipboardObject(std::move(formats), std::move(content.lease), virtual_png));
    // The Shell wrapper owns the drag helper's private formats; the snapshot stays immutable.
    ComPtr<IDataObject> object;
    if (SUCCEEDED(SHCreateDataObject(nullptr, 0, nullptr, saved.Get(), IID_PPV_ARGS(&object))))
        set_preview(object.Get(), card);
    else object = saved;
    auto source = Microsoft::WRL::Make<DropSource>(canceled_);
    if (!source) throw std::bad_alloc();
    if (auto stale = valid(); !stale.empty()) return stale;
    DWORD effect{};
    const auto result = drag_target(object.Get(), source.Get(), DROPEFFECT_COPY, &effect);
    if (FAILED(result)) throw std::runtime_error("Could not start external drag");
    return result == DRAGDROP_S_DROP && effect == DROPEFFECT_COPY ? "copied" : "no-drop";
}
}
