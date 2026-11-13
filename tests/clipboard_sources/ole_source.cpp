#include "platform_win/clipboard_object.hpp"
#include "sample.hpp"
#include "virtual_sample.hpp"
#include "../integration/clipboard_process.hpp"

class SampleObject final : public cliprope::ClipboardObject {
public:
    SampleObject(std::wstring mode, HANDLE reading, HANDLE proceed)
        : ClipboardObject(make_formats(mode)), mode_(std::move(mode)), reading_(reading), proceed_(proceed) {}

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* data) override {
        if (format && format->cfFormat == cliprope::virtual_files::contents_format() && format->lindex == 1 &&
            (mode_ == L"virtual-broken" || mode_ == L"virtual-only-broken")) return STG_E_READFAULT;
        if (format && (format->cfFormat == CF_UNICODETEXT || format->cfFormat == RegisterClipboardFormatW(L"HTML Format"))) {
            if (mode_ == L"failed") return E_FAIL;
            if (mode_ == L"busy-once" && !was_busy_) {
                was_busy_ = true;
                return CLIPBRD_E_CANT_OPEN;
            }
            if (mode_ == L"exclude" || mode_ == L"no-history" || mode_ == L"paused") SetEvent(reading_);
        }
        if (format && format->cfFormat == RegisterClipboardFormatW(L"HTML Format")) {
            if (mode_ == L"partial") return E_FAIL;
            if (mode_ == L"blocked") {
                // Simulate a producer awaiting an unavailable attachment. Its owner
                // window still handles clipboard ownership changes from later copies.
                SetEvent(reading_);
                DWORD selected{};
                if (CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES,
                                             40000, 1, &proceed_, &selected) != S_OK) return E_ABORT;
            }
            if (mode_ == L"delayed" || mode_ == L"held") {
                SetEvent(reading_);
                if (WaitForSingleObject(proceed_, mode_ == L"held" ? 25000 : 8000) != WAIT_OBJECT_0) return E_ABORT;
            }
        }
        return ClipboardObject::GetData(format, data);
    }
private:
    static std::vector<cliprope::Representation> make_formats(const std::wstring& mode) {
        if (mode.starts_with(L"virtual")) {
            auto items = sample::virtual_group();
            if (mode == L"virtual-large") items.back().bytes.resize(cliprope::virtual_files::stream_limit + 1, 0x41);
            if (!mode.starts_with(L"virtual-only")) items.insert(items.begin(), sample::formats(false).front());
            return items;
        }
        auto formats = sample::formats(mode == L"second");
        formats.push_back({RegisterClipboardFormatW(L"ClipRope Synthetic Private"), {42}});
        if (mode == L"exclude") formats.push_back({RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing"), {1}});
        if (mode == L"no-history") formats.push_back({RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"), {0, 0, 0, 0}});
        if (mode == L"no-cloud") formats.push_back({RegisterClipboardFormatW(L"CanUploadToCloudClipboard"), {0, 0, 0, 0}});
        return formats;
    }
    std::wstring mode_;
    HANDLE reading_;
    HANDLE proceed_;
    bool was_busy_{};
};

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 || !probe::private_station()) return 2;
    if (FAILED(OleInitialize(nullptr))) return 1;
    const std::wstring key = argv[2];
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, (key + L"-ready").c_str());
    HANDLE reading = OpenEventW(EVENT_MODIFY_STATE, FALSE, (key + L"-reading").c_str());
    HANDLE proceed = OpenEventW(SYNCHRONIZE, FALSE, (key + L"-proceed").c_str());
    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, (key + L"-stop").c_str());
    if (!ready || !reading || !proceed || !stop) return 1;
    if (std::wstring_view(argv[1]) == L"reader-250" || std::wstring_view(argv[1]) == L"anonymous-reader" ||
        std::wstring_view(argv[1]) == L"held-reader") {
        const bool anonymous = std::wstring_view(argv[1]) == L"anonymous-reader";
        const HWND window = CreateWindowW(L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
        if (!window) return 1;
        SetEvent(ready);
        if (WaitForSingleObject(proceed, 8000) != WAIT_OBJECT_0) return 1;
        const auto deadline = GetTickCount64() + 1000;
        while (!OpenClipboard(anonymous ? nullptr : window)) {
            if (GetTickCount64() >= deadline) return 1;
            Sleep(1);
        }
        SetEvent(reading);
        if (std::wstring_view(argv[1]) == L"reader-250") Sleep(250);
        else WaitForSingleObject(stop, 8000);
        CloseClipboard();
        DestroyWindow(window);
        OleUninitialize();
        return 0;
    }
    auto* source = new SampleObject(argv[1], reading, proceed);
    const bool after_resume = std::wstring_view(argv[1]) == L"after-resume";
    const auto sequence = GetClipboardSequenceNumber();
    const auto started = GetTickCount64();
    unsigned attempts = 1;
    auto hr = OleSetClipboard(source);
    // A waking session can temporarily deny clipboard opens. Retry only the
    // unissued synthetic copy and keep the original source object throughout.
    while (after_resume && hr == CLIPBRD_E_CANT_OPEN && GetTickCount64() - started < 120000) {
        DWORD selected{};
        const auto wait = CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES,
                                                    100, 1, &stop, &selected);
        if (wait != RPC_S_CALLPENDING || (sequence && GetClipboardSequenceNumber() != sequence)) break;
        ++attempts;
        hr = OleSetClipboard(source);
    }
    if (after_resume)
        std::cout << "post_resume_publish_attempts=" << attempts << " elapsed_ms=" << GetTickCount64() - started
                  << " published=" << SUCCEEDED(hr) << std::endl;
    if (FAILED(hr)) {
        std::cerr << "External publication HRESULT=" << std::hex << hr << '\n';
        return 1;
    }
    SetEvent(ready);
    const auto deadline = GetTickCount64() + 45000;
    while (GetTickCount64() < deadline) {
        const auto wait = MsgWaitForMultipleObjects(1, &stop, FALSE, 100, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) break;
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (OleIsCurrentClipboard(source) == S_OK) OleSetClipboard(nullptr);
    source->Release();
    CloseHandle(stop);
    CloseHandle(proceed);
    CloseHandle(reading);
    CloseHandle(ready);
    OleUninitialize();
    return 0;
}
