#include "platform_win/clipboard_object.hpp"
#include "platform_win/clipboard_wait.hpp"
#include "platform_win/file_group.hpp"
#include "platform_win/media.hpp"
#include "platform_win/virtual_files.hpp"
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <new>
#include <set>
#include <sstream>
#include <string_view>

namespace cliprope {

std::wstring format_name(UINT format) {
    switch (format) {
    case CF_TEXT: return L"CF_TEXT";
    case CF_BITMAP: return L"CF_BITMAP";
    case CF_METAFILEPICT: return L"CF_METAFILEPICT";
    case CF_SYLK: return L"CF_SYLK";
    case CF_DIF: return L"CF_DIF";
    case CF_TIFF: return L"CF_TIFF";
    case CF_OEMTEXT: return L"CF_OEMTEXT";
    case CF_DIB: return L"CF_DIB";
    case CF_PALETTE: return L"CF_PALETTE";
    case CF_PENDATA: return L"CF_PENDATA";
    case CF_RIFF: return L"CF_RIFF";
    case CF_WAVE: return L"CF_WAVE";
    case CF_UNICODETEXT: return L"CF_UNICODETEXT";
    case CF_ENHMETAFILE: return L"CF_ENHMETAFILE";
    case CF_HDROP: return L"CF_HDROP";
    case CF_LOCALE: return L"CF_LOCALE";
    case CF_DIBV5: return L"CF_DIBV5";
    default:
        wchar_t name[256]{};
        const auto count = GetClipboardFormatNameW(format, name, 256);
        return count > 0 ? std::wstring(name, count) : L"format-" + std::to_wstring(format);
    }
}

static bool html_offset(std::string_view header, std::string_view field, size_t& value) {
    const auto start = header.find(field);
    if (start == header.npos || (start && header[start - 1] != '\n' && header[start - 1] != '\r')) return false;
    const auto* begin = header.data() + start + field.size();
    const auto* end = header.data() + header.size();
    auto [next, error] = std::from_chars(begin, end, value);
    return error == std::errc{} && next != end && (*next == '\r' || *next == '\n');
}

bool supports_hglobal(UINT format) {
    return format == CF_UNICODETEXT || format == RegisterClipboardFormatW(L"HTML Format") ||
        media::supports(format) || is_file_group_format(format) || format == virtual_files::descriptor_format();
}

std::size_t representation_byte_limit(UINT format) {
    if (media::supports(format)) return media::byte_limit(format);
    if (format == virtual_files::descriptor_format())
        return offsetof(FILEGROUPDESCRIPTORW, fgd) + virtual_files::file_limit * sizeof(FILEDESCRIPTORW) + 16;
    return 8u * 1024u * 1024u;
}

bool validate_representation(Representation& item) {
    auto& bytes = item.bytes;
    if (!item.format || item.format > 0xffff || item.codec_version != 1 || item.aspect != DVASPECT_CONTENT) return false;
    if (item.format == virtual_files::contents_format())
        return item.tymed == TYMED_ISTREAM && item.index >= 0 && static_cast<size_t>(item.index) < virtual_files::file_limit &&
               bytes.size() <= virtual_files::stream_limit;
    const bool stream_png = item.tymed == TYMED_ISTREAM && item.format == RegisterClipboardFormatW(L"PNG");
    if ((item.tymed != TYMED_HGLOBAL && !stream_png) || item.index != -1) return false;
    const auto byte_limit = representation_byte_limit(item.format);
    if (bytes.empty() || bytes.size() > byte_limit) return false;
    if (media::supports(item.format)) return media::validate(item.format, bytes);
    if (is_file_group_format(item.format)) return validate_file_group(item);
    if (item.format == virtual_files::descriptor_format()) return virtual_files::validate_descriptors(item);
    if (item.format == CF_UNICODETEXT) {
        for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
            const auto unit = static_cast<unsigned>(bytes[i]) | (static_cast<unsigned>(bytes[i + 1]) << 8);
            if (!unit) {
                bytes.resize(i + 2);
                return true;
            }
            if (unit >= 0xd800 && unit <= 0xdfff) {
                if (unit >= 0xdc00 || i + 3 >= bytes.size() || (bytes[i + 3] & 0xfc) != 0xdc) return false;
                i += 2;
            }
        }
        return false;
    }
    if (item.format != RegisterClipboardFormatW(L"HTML Format")) return false;
    const std::string_view content(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    auto header = content.substr(0, 8192);
    size_t start{}, end{}, fragment{}, fragment_end{};
    if (!header.starts_with("Version:") || !html_offset(header, "StartHTML:", start) || start > header.size()) return false;
    header = header.substr(0, start);
    if (!html_offset(header, "StartHTML:", start) || !html_offset(header, "EndHTML:", end) || !html_offset(header, "StartFragment:", fragment) ||
        !html_offset(header, "EndFragment:", fragment_end) || start > fragment ||
        fragment > fragment_end || fragment_end > end || end > bytes.size() || end >= byte_limit || !end) return false;
    if (content.substr(0, end).find('\0') != content.npos ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, content.data(), static_cast<int>(end), nullptr, 0)) return false;
    bytes.resize(end);
    bytes.push_back(0);
    return true;
}

ClipboardObject::ClipboardObject(std::vector<Representation> saved, std::shared_ptr<void> lease, bool screenshot_file)
    : saved_(std::move(saved)), lease_(std::move(lease)) {
    if (screenshot_file) file_contents_png_ = virtual_files::append_screenshot(saved_);
}
ClipboardObject::ClipboardObject(ClipboardOffer offer, std::shared_ptr<void> lease)
    : saved_(std::move(offer.formats_)), lease_(std::move(lease)), file_contents_png_(offer.png_alias_) {}

HRESULT ClipboardObject::QueryInterface(REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_IDataObject) return E_NOINTERFACE;
    *out = static_cast<IDataObject*>(this);
    AddRef();
    return S_OK;
}
ULONG ClipboardObject::AddRef() { return ++refs_; }
ULONG ClipboardObject::Release() {
    const ULONG remaining = --refs_;
    if (!remaining) delete this;
    return remaining;
}

HRESULT ClipboardObject::QueryGetData(FORMATETC* format) {
    if (!format) return E_POINTER;
    if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
    if (format->ptd) return DV_E_DVTARGETDEVICE;
    HRESULT mismatch = DV_E_FORMATETC;
    for (const auto& item : saved_) {
        if (item.format != format->cfFormat) continue;
        if (item.index != format->lindex) { if (mismatch == DV_E_FORMATETC) mismatch = DV_E_LINDEX; continue; }
        if (item.tymed & format->tymed) return S_OK;
        mismatch = DV_E_TYMED;
    }
    return mismatch;
}

HRESULT ClipboardObject::GetData(FORMATETC* format, STGMEDIUM* medium) {
    if (!medium) return E_POINTER;
    *medium = {};
    if (auto hr = QueryGetData(format); FAILED(hr)) return hr;
    for (const auto& item : saved_) {
        if (item.format != format->cfFormat || item.index != format->lindex || !(item.tymed & format->tymed)) continue;
        if ((item.tymed & format->tymed & TYMED_ISTREAM) != 0) {
            const auto& bytes = file_contents_png_ && item.format == virtual_files::contents_format()
                ? saved_[*file_contents_png_].bytes : item.bytes;
            auto* stream = SHCreateMemStream(bytes.empty() ? nullptr : bytes.data(), static_cast<UINT>(bytes.size()));
            if (!stream) return E_OUTOFMEMORY;
            medium->tymed = TYMED_ISTREAM;
            medium->pstm = stream;
            return S_OK;
        }
        auto memory = GlobalAlloc(GHND, item.bytes.size());
        if (!memory) return E_OUTOFMEMORY;
        void* destination = GlobalLock(memory);
        if (!destination) { GlobalFree(memory); return E_OUTOFMEMORY; }
        std::memcpy(destination, item.bytes.data(), item.bytes.size());
        GlobalUnlock(memory);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        return S_OK;
    }
    return DV_E_FORMATETC;
}

HRESULT ClipboardObject::GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) {
    if (!out) return E_POINTER;
    out->ptd = nullptr;
    return E_NOTIMPL;
}

HRESULT ClipboardObject::EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (direction != DATADIR_GET) return E_NOTIMPL;
    try {
        std::vector<FORMATETC> formats;
        for (const auto& item : saved_)
            formats.push_back({static_cast<CLIPFORMAT>(item.format), nullptr, item.aspect, item.index, item.tymed});
        return SHCreateStdEnumFmtEtc(static_cast<UINT>(formats.size()), formats.data(), out);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

ClipboardPublisher::~ClipboardPublisher() { if (current_) current_->Release(); }

std::string FlushReceipt::summary() const {
    constexpr const char* stages[]{"untouched", "ole", "cleared", "partial", "portable-formats"};
    std::ostringstream out;
    out << "flush=" << stages[static_cast<unsigned>(effect)] << " installed=" << installed << "/" << companions
        << " HRESULT=0x" << std::hex << static_cast<unsigned long>(result) << std::dec << " win32=";
    if (FAILED(result) && !native_error) out << "unavailable";
    else out << native_error;
    if (effect == FlushEffect::cleared || effect == FlushEffect::partial || effect == FlushEffect::flattened)
        out << " virtual-files=restorable-from-history";
    return out.str();
}

#ifdef CLIPROPE_FLUSH_PROBES
// Private station only. Child processes inherit this setting from the fixture.
static unsigned failed_installation() {
    wchar_t requested[4]{}, station[256]{}, actual[256]{};
    if (const auto size = GetEnvironmentVariableW(L"CLIPROPE_PROBE_FLUSH_SET", requested, 4); !size || size >= 4)
        return 0;
    const auto size = GetEnvironmentVariableW(L"CLIPROPE_PROBE_STATION", station, 256);
    DWORD copied{};
    if (!size || size >= 256 || !_wcsicmp(station, L"WinSta0") ||
        !GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, actual, sizeof(actual), &copied) ||
        _wcsicmp(actual, station)) return 0;
    return static_cast<unsigned>(_wtoi(requested));
}
#endif

// The native clipboard can keep only one handle per format. Flushing an OLE
// FileContents family would leave a descriptor advertising unreadable indices.
static HRESULT flush_without_virtual_files(ClipboardObject& source, FlushReceipt& receipt) {
    struct Global { void operator()(void* h) const { GlobalFree(h); } };
    struct Native { UINT format; std::unique_ptr<void, Global> bytes; };
    std::vector<Native> portable;
    const auto descriptor = virtual_files::descriptor_format(), contents = virtual_files::contents_format();
    for (const auto& item : source.formats())
        receipt.companions += item.format != descriptor && item.format != contents;
    portable.reserve(receipt.companions);
    for (const auto& item : source.formats()) {
        if (item.format == descriptor || item.format == contents) continue;
        // Other supported media have a portable HGLOBAL spelling. PNG uses
        // its exact validated encoded bytes even when served via IStream.
        Native native{item.format, std::unique_ptr<void, Global>(GlobalAlloc(GHND, item.bytes.size()))};
        if (!native.bytes) return E_OUTOFMEMORY;
        auto* destination = GlobalLock(native.bytes.get());
        if (!destination) return E_OUTOFMEMORY;
        std::memcpy(destination, item.bytes.data(), item.bytes.size());
        GlobalUnlock(native.bytes.get());
        portable.push_back(std::move(native));
    }
#ifdef CLIPROPE_FLUSH_PROBES
    const auto fail_at = failed_installation();
#endif
    const auto deadline = GetTickCount64() + 500;
    DWORD open_error{};
    do {
        const DWORD sequence = GetClipboardSequenceNumber();
        const auto owned = OleIsCurrentClipboard(&source);
        if (owned != S_OK && owned != CLIPBRD_E_CANT_OPEN) return owned;
        HWND owner = GetClipboardOwner();
        DWORD pid{};
        bool opened{};
        if (owned == S_OK && owner && GetWindowThreadProcessId(owner, &pid) && pid == GetCurrentProcessId()) {
            SetLastError(ERROR_SUCCESS);
            opened = OpenClipboard(owner) != FALSE;
            open_error = opened ? 0 : GetLastError();
        }
        if (opened) {
            struct Close { ~Close() { CloseClipboard(); } } close;
            // Identity was checked above. The version and owner must still
            // match under the exclusive native lock before any mutation.
            if (GetClipboardOwner() != owner) return S_FALSE;
            if (!sequence || GetClipboardSequenceNumber() != sequence) continue;
            SetLastError(ERROR_SUCCESS);
            if (!EmptyClipboard()) {
                receipt.native_error = GetLastError();
                return CLIPBRD_E_CANT_EMPTY;
            }
            receipt.effect = FlushEffect::cleared;
            for (auto& item : portable) {
#ifdef CLIPROPE_FLUSH_PROBES
                if (fail_at == receipt.installed + 1) {
                    receipt.native_error = ERROR_ACCESS_DENIED;
                    return CLIPBRD_E_CANT_SET;
                }
#endif
                SetLastError(ERROR_SUCCESS);
                if (!SetClipboardData(item.format, item.bytes.get())) {
                    receipt.native_error = GetLastError();
                    return CLIPBRD_E_CANT_SET;
                }
                item.bytes.release();
                ++receipt.installed;
                receipt.effect = FlushEffect::partial;
            }
            receipt.effect = FlushEffect::flattened;
            return clipboard_partial_flush;
        }
        pump_clipboard_reader(12);
    } while (GetTickCount64() < deadline);
    receipt.native_error = open_error;
    return CLIPBRD_E_CANT_OPEN;
}

HRESULT prepare_publication(std::vector<Representation>& formats) {
    if (formats.empty() || formats.size() > 256) return E_INVALIDARG;
    std::size_t bytes{};
    std::set<std::pair<UINT, LONG>> identities;
    for (auto& item : formats) {
        if (item.bytes.size() > snapshot_byte_limit - bytes) return E_INVALIDARG;
        if (!validate_representation(item)) return DV_E_FORMATETC;
        if (item.bytes.size() > snapshot_byte_limit - bytes) return E_INVALIDARG;
        bytes += item.bytes.size();
        if (!identities.emplace(item.format, item.index).second) return E_INVALIDARG;
    }
    if (!virtual_files::complete(formats) || !prepare_file_group(formats) || formats.size() > 256) return DV_E_FORMATETC;
    bytes = 0;
    for (const auto& item : formats) {
        if (item.bytes.size() > snapshot_byte_limit - bytes) return E_INVALIDARG;
        bytes += item.bytes.size();
    }
    return S_OK;
}

HRESULT ClipboardPublisher::publish(const std::vector<Representation>& saved, std::shared_ptr<void> lease) {
    if (publishing_) return RPC_E_CALL_REJECTED;
    if (saved.empty() || saved.size() > 256) return E_INVALIDARG;
    std::size_t bytes{};
    for (const auto& item : saved) {
        if (item.bytes.size() > snapshot_byte_limit - bytes) return E_INVALIDARG;
        bytes += item.bytes.size();
    }
    try { return publish(std::vector<Representation>(saved), std::move(lease)); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT ClipboardPublisher::publish(std::vector<Representation>&& saved, std::shared_ptr<void> lease,
                                    std::optional<DWORD> expected_sequence, bool screenshot_file) {
    if (publishing_) return RPC_E_CALL_REJECTED;
    ClipboardOffer offer;
    const auto hr = ClipboardOffer::prepare(std::move(saved), screenshot_file, offer);
    if (FAILED(hr)) return hr;
    return publish_prepared(std::move(offer), std::move(lease), expected_sequence);
}

HRESULT ClipboardOffer::prepare(std::vector<Representation> saved, bool screenshot_file, ClipboardOffer& result) {
    result = {};
    try {
        if (const auto hr = prepare_publication(saved); FAILED(hr)) return hr;
        if (screenshot_file) result.png_alias_ = virtual_files::append_screenshot(saved);
        result.formats_ = std::move(saved);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (const std::exception&) { return DV_E_FORMATETC; }
    return S_OK;
}

HRESULT ClipboardPublisher::publish_prepared(ClipboardOffer offer, std::shared_ptr<void> lease,
                                             std::optional<DWORD> expected_sequence) {
    if (publishing_) return RPC_E_CALL_REJECTED;
    if (offer.formats_.empty()) return DV_E_FORMATETC;
    auto* next = new(std::nothrow) ClipboardObject(std::move(offer), std::move(lease));
    if (!next) return E_OUTOFMEMORY;
    // COM may dispatch messages while publishing. Expose the candidate identity
    // before calling into OLE, and restore the previous identity on failure.
    auto* previous = current_;
    current_ = next;
    publishing_ = true;
    const DWORD before = GetClipboardSequenceNumber();
    auto hr = expected_sequence && before != *expected_sequence ? HRESULT_FROM_WIN32(ERROR_CANCELLED)
                                                               : OleSetClipboard(next);
    // A listener can briefly hold OpenClipboard even for a read. Retry only a
    // definite failure to open, while the original clipboard version survives.
    for (int attempt = 0; hr == CLIPBRD_E_CANT_OPEN && before && attempt < 5; ++attempt) {
        current_ = previous; // Its delayed render is still a self-owned event.
        pump_clipboard_reader(8);
        if (GetClipboardSequenceNumber() != before) break;
        current_ = next;
        hr = OleSetClipboard(next);
    }
    if (FAILED(hr)) { current_ = previous; next->Release(); }
    else if (previous) previous->Release();
    publishing_ = false;
    return hr;
}

bool ClipboardPublisher::is_current() const {
    Microsoft::WRL::ComPtr<IDataObject> queried(current_);
    return queried.Get() && OleIsCurrentClipboard(queried.Get()) == S_OK && queried.Get() == current_;
}
void ClipboardPublisher::release_if_replaced() {
    if (publishing_ || !current_) return;
    // OLE may dispatch another publication or retirement while querying the old object.
    Microsoft::WRL::ComPtr<IDataObject> queried(current_);
    if (OleIsCurrentClipboard(queried.Get()) == S_FALSE && current_ == queried.Get()) {
        auto* retired = current_;
        current_ = nullptr;
        retired->Release();
    }
}
FlushReceipt ClipboardPublisher::flush_current() {
    FlushReceipt receipt;
    if (!current_) return receipt;
    if (publishing_) return {RPC_E_CALL_REJECTED};
    auto* identity = current_;
    Microsoft::WRL::ComPtr<IDataObject> keep(identity);
    publishing_ = true;
    auto& hr = receipt.result;
    if (std::any_of(identity->formats().begin(), identity->formats().end(),
                    [](const auto& item) { return item.format == virtual_files::descriptor_format(); })) {
        try { hr = flush_without_virtual_files(*identity, receipt); }
        catch (const std::bad_alloc&) { hr = E_OUTOFMEMORY; }
        publishing_ = false;
        if (SUCCEEDED(hr)) release_if_replaced();
        return receipt;
    }
    const auto deadline = GetTickCount64() + 500;
    do {
        hr = OleIsCurrentClipboard(identity);
        if (hr == S_OK) {
            receipt.effect = FlushEffect::ole_flush;
            hr = OleFlushClipboard();
        }
        if (hr != CLIPBRD_E_CANT_OPEN) break;
        pump_clipboard_reader(12);
    } while (GetTickCount64() < deadline);
    publishing_ = false;
    if (SUCCEEDED(hr)) {
        release_if_replaced();
    }
    return receipt;
}

} // namespace cliprope
