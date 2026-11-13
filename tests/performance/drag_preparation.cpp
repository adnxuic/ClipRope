#include "../integration/history_fixture.hpp"
#include "../integration/clipboard_process.hpp"
#include "../clipboard_sources/media_sample.hpp"
#include "platform_win/external_drag.hpp"
#include "platform_win/history_clipboard.hpp"
#include "rendering/image_preview.hpp"
#include <shlobj.h>
#include <wrl.h>
#include <chrono>
#include <fstream>
#include <iostream>

namespace {
using namespace cliprope;
using history_fixture::require;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
bool held{};
unsigned drops{};
std::filesystem::path destination;
std::vector<unsigned char> expected_png;
double ms(Clock::time_point start) {
    return std::chrono::duration<double,std::milli>(Clock::now()-start).count();
}
// Read through a real Shell folder drop target, on the private station.
void shell_copy(IDataObject* object) {
    ComPtr<IShellItem> folder;
    require(SUCCEEDED(SHCreateItemFromParsingName(destination.c_str(), nullptr, IID_PPV_ARGS(&folder))),
            "Disposable Shell folder unavailable");
    ComPtr<IDropTarget> receiver;
    require(SUCCEEDED(folder->BindToHandler(nullptr,BHID_SFUIObject,IID_PPV_ARGS(&receiver))),
            "Shell folder has no drop target");
    DWORD effect = DROPEFFECT_COPY;
    require(SUCCEEDED(receiver->DragEnter(object,MK_LBUTTON,{},&effect)) && (effect & DROPEFFECT_COPY),
            "Shell folder declined virtual PNG");
    require(SUCCEEDED(receiver->Drop(object,0,{},&effect)) && (effect & DROPEFFECT_COPY),
            "Shell folder failed to copy virtual PNG");
}
void scenario(std::string_view name) {
    history_fixture::Directory dir;
    destination = dir.path / "receiver";
    std::filesystem::create_directory(destination);
    persistence::HistoryStore store(dir.path / "profile");
    BITMAPINFOHEADER header{sizeof header,1920,1080,1,32,BI_RGB};
    domain::Format dib;
    dib.standard_id = CF_DIB;
    dib.bytes.resize(sizeof header + size_t(header.biWidth)*header.biHeight*4);
    std::memcpy(dib.bytes.data(),&header,sizeof header);
    for (size_t i=sizeof header;i<dib.bytes.size();++i) dib.bytes[i]=static_cast<unsigned char>(i/7);
    expected_png = rendering::encode_dib_png(CF_DIB,dib.bytes);
    application::CardSummary card;
    card.entry = store.entry(store.admit("main",{{std::move(dib)}}));
    HWND window = CreateWindowExW(0,L"STATIC",L"",WS_POPUP,0,0,20,20,nullptr,nullptr,nullptr,nullptr);
    require(window!=nullptr,"Missing source HWND");
    HWND surface = CreateWindowExW(0,L"STATIC",L"",WS_POPUP,0,0,20,20,nullptr,nullptr,nullptr,nullptr);
    require(surface!=nullptr,"Missing card HWND");
    auto session=std::make_unique<win::ExternalDragSession>();
    const auto serial=session->replace();
    held=true;
    drops=0;
    const auto sequence=GetClipboardSequenceNumber();
    const auto begin=Clock::now();
    const auto start=session->run(window,surface,store,card.entry,card,serial,sequence);
    const auto dispatch=ms(begin);
    require(start=="preparing","DIB drag did not stage");
    if (name=="cancel" || name=="quit" || name=="replaced") {
        if (name=="replaced") session->replace();
        else if (name=="quit") session.reset();
        else require(session->cancel(),"Cancel lost the ticket");
        if (session) require(session->finish(store,serial).empty(),"Canceled ticket returned an offer");
    } else {
        if (name=="released") held=false;
        if (name=="entry-changed") store.erase(card.entry.id);
        if (name=="owner-replaced") { DestroyWindow(surface); surface=nullptr; }
        if (name=="clipboard-changed") {
            require(OpenClipboard(window)!=FALSE,"Private clipboard unavailable");
            const auto changed=EmptyClipboard();
            CloseClipboard();
            require(changed && sequence!=GetClipboardSequenceNumber(),"Private clipboard sequence unchanged");
        }
        std::string_view result;
        while (ms(begin)<5000 && result.empty()) {
            MSG msg{};
            if (PeekMessageW(&msg,nullptr,win::ExternalDragSession::ready_message,win::ExternalDragSession::ready_message,PM_REMOVE))
                result=session->finish(store,msg.wParam);
            else Sleep(1);
        }
        require(result==name,"Drag completion did not preserve its bound receipt");
        require(session->finish(store,serial).empty(),"Versioned result dispatched twice");
    }
    require(drops==(name=="copied" ? 1u : 0u),"Unexpected receiver dispatch count");
    if (name!="clipboard-changed") require(GetClipboardSequenceNumber()==sequence,"Drag replaced clipboard");
    if (drops) {
        unsigned files{};
        for (const auto& entry : std::filesystem::directory_iterator(destination)) {
            std::ifstream file(entry.path(),std::ios::binary);
            std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file),{}};
            require(bytes==expected_png,"Shell receiver changed PNG bytes");
            ++files;
        }
        require(files==1,"Shell receiver did not install exactly one PNG");
    }
    const auto terminal=ms(begin);
    if (surface) DestroyWindow(surface);
    DestroyWindow(window);
    // Replay the pre-change synchronous preparation on the same immutable 1080p offer.
    // Run it after the candidate so its disk pages and allocator may already be warm.
    domain::Snapshot comparison{{{CF_DIB,{},std::vector<unsigned char>(sizeof header + size_t(header.biWidth)*header.biHeight*4)}}};
    auto& bytes=comparison.formats.front().bytes;
    std::memcpy(bytes.data(),&header,sizeof header);
    for (size_t i=sizeof header;i<bytes.size();++i) bytes[i]=static_cast<unsigned char>(i/7);
    auto comparison_id=store.admit("main",comparison);
    auto baseline_begin=Clock::now();
    auto loaded=store.load(comparison_id);
    auto formats=win::restorable_formats(loaded.snapshot);
    require(formats.size()==1 && rendering::encode_dib_png(formats.front().format,formats.front().bytes)==expected_png,
            "Synchronous replay changed receiver bytes");
    const auto baseline=ms(baseline_begin);
    std::cout<<"drag case="<<name<<" foreground_prepare_ms="<<dispatch<<" sync_prepare_ms="<<baseline<<" terminal_ms="<<terminal
             <<" shell_files="<<drops<<'\n';
}
}
namespace cliprope::win {
bool drag_button_down() { return held; }
HRESULT drag_target(IDataObject* object, IDropSource*, DWORD allowed, DWORD* effect) {
    require(allowed==DROPEFFECT_COPY,"Changed permitted drop effect");
    shell_copy(object);
    ++drops;
    *effect=DROPEFFECT_COPY;
    return DRAGDROP_S_DROP;
}
}
int main() {
    if (!probe::private_station() || FAILED(OleInitialize(nullptr))) return 2;
    int result{};
    try {
        for (auto name:{"copied","released","entry-changed","owner-replaced","clipboard-changed","cancel","quit","replaced"})
            scenario(name);
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; result=1; }
    OleUninitialize();
    return result;
}
