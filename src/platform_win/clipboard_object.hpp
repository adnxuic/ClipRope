#pragma once

#include "platform_win/clipboard.hpp"
#include <atomic>

namespace cliprope {

// Immutable OLE data source. A synthetic external producer can override GetData
// to exercise delayed rendering and COM failure independently of publication.
class ClipboardObject : public IDataObject {
public:
    explicit ClipboardObject(std::vector<Representation> saved, std::shared_ptr<void> lease = {}, bool screenshot_file = false);
    ClipboardObject(ClipboardOffer offer, std::shared_ptr<void> lease);
    const std::vector<Representation>& formats() const { return saved_; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC*, STGMEDIUM*) override;
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC*) override;
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override;
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override;
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
protected:
    virtual ~ClipboardObject() = default;
private:
    std::atomic<ULONG> refs_{1};
    std::vector<Representation> saved_;
    std::shared_ptr<void> lease_;
    std::optional<std::size_t> file_contents_png_; // An indexed stream over an existing saved PNG.
};

} // namespace cliprope
