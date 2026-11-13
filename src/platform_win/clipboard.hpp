#pragma once

#include <windows.h>
#include <ole2.h>
#include <string>
#include <memory>
#include <optional>
#include <vector>

namespace cliprope {

struct Representation {
    UINT format;
    std::vector<unsigned char> bytes;
    LONG index = -1;
    DWORD tymed = TYMED_HGLOBAL;
    DWORD aspect = DVASPECT_CONTENT;
    int codec_version = 1;
    bool operator==(const Representation&) const = default;
};

std::wstring format_name(UINT format);
bool supports_hglobal(UINT format);
std::size_t representation_byte_limit(UINT format);
inline constexpr std::size_t snapshot_byte_limit = 64u * 1024u * 1024u;
// Windows cannot flatten a family of indexed streams into one clipboard handle.
inline constexpr HRESULT clipboard_partial_flush = MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_ITF, 0x201);
enum class FlushEffect { untouched, ole_flush, cleared, partial, flattened };
struct FlushReceipt {
    HRESULT result = S_FALSE;
    FlushEffect effect = FlushEffect::untouched;
    unsigned installed{}, companions{};
    DWORD native_error{};
    std::string summary() const;
};
// Trim allocation padding and validate the versioned representation.
bool validate_representation(Representation& representation);
HRESULT prepare_publication(std::vector<Representation>& formats);

// A validated, byte-only offer. It can be prepared before the publication STA
// constructs the IDataObject and changes the clipboard.
class ClipboardOffer {
public:
    static HRESULT prepare(std::vector<Representation> saved, bool screenshot_file, ClipboardOffer& result);
private:
    friend class ClipboardPublisher;
    friend class ClipboardObject;
    std::vector<Representation> formats_;
    std::optional<std::size_t> png_alias_;
};

// Use on the same pumping OLE STA for its entire lifetime.
class ClipboardObject;
class ClipboardPublisher {
public:
    ClipboardPublisher() = default;
    ~ClipboardPublisher();
    ClipboardPublisher(const ClipboardPublisher&) = delete;
    ClipboardPublisher& operator=(const ClipboardPublisher&) = delete;
    HRESULT publish(const std::vector<Representation>& saved, std::shared_ptr<void> lease = {});
    HRESULT publish(std::vector<Representation>&& saved, std::shared_ptr<void> lease = {},
                    std::optional<DWORD> expected_sequence = {}, bool screenshot_file = false);
    HRESULT publish_prepared(ClipboardOffer offer, std::shared_ptr<void> lease, std::optional<DWORD> expected_sequence);
    bool is_current() const;
    bool has_settled_publication() const { return !publishing_ && is_current() && !publishing_; }
    void release_if_replaced();
    FlushReceipt flush_current();
    HRESULT flush_if_current() { return flush_current().result; }
private:
    ClipboardObject* current_ = nullptr;
    bool publishing_{};
};

} // namespace cliprope
