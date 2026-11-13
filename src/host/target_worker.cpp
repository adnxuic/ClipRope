#include "platform_win/target_protocol.hpp"
#include "platform_win/clipboard_wait.hpp"
#include "platform_win/focus_field.hpp"
#include "platform_win/office_selection.hpp"
#include "platform_win/office_view.hpp"
#include "platform_win/process_image.hpp"
#include <exdisp.h>
#include <shlobj.h>
#include <shlguid.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <future>
#include <stdexcept>
#ifdef CLIPROPE_DESKTOP_PROBES
#include "target_gate.hpp"
#endif
#include <cwchar>

namespace cliprope::win::target {
namespace {
bool query_paste_button(HWND foreground) {
    // Explorer's command bar uses the localized Windows.paste resource, including
    // directional marks. The folder background extension menu can omit this verb.
    wchar_t resource[256]{}, label[256]{};
    DWORD length = sizeof resource;
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
             L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell\\Windows.paste",
             L"MUIVerb", RRF_RT_REG_SZ, nullptr, resource, &length) != ERROR_SUCCESS ||
        FAILED(SHLoadIndirectString(resource, label, static_cast<UINT>(std::size(label)), nullptr))) return false;
    using Microsoft::WRL::ComPtr;
    FocusClient client;
    const auto pid = window_process(foreground);
    auto* automation = client.for_window(foreground, pid);
    ComPtr<IUIAutomationElement> root, button;
    ComPtr<IUIAutomationCondition> name, type, both;
    VARIANT match{};
    match.vt = VT_BSTR;
    match.bstrVal = SysAllocString(label);
    if (!match.bstrVal) return false;
    auto hr = automation ? automation->CreatePropertyCondition(UIA_NamePropertyId, match, &name) : E_FAIL;
    VariantClear(&match);
    if (FAILED(hr)) return false;
    match.vt = VT_I4;
    match.lVal = UIA_ButtonControlTypeId;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, match, &type)) ||
        FAILED(automation->CreateAndCondition(name.Get(), type.Get(), &both)) ||
        FAILED(automation->ElementFromHandle(foreground, &root)) || !root ||
        FAILED(root->FindFirst(TreeScope_Descendants, both.Get(), &button)) || !button) return false;
    BOOL enabled{};
    int owner{};
    BSTR cls{};
    if (FAILED(button->get_CurrentClassName(&cls))) return false;
    const bool command = cls && wcscmp(cls, L"AppBarButton") == 0;
    SysFreeString(cls);
    return command && SUCCEEDED(button->get_CurrentProcessId(&owner)) && static_cast<DWORD>(owner) == pid &&
           SUCCEEDED(button->get_CurrentIsEnabled(&enabled)) && enabled;
}

// Explorer can leave the toolbar disabled after replacing a non-file clipboard.
// Ask the directory's transfer handler about this exact data object. DragLeave
// ends the capability check. Only the eventual native Ctrl+V performs a copy.
bool folder_accepts_copy(const std::wstring& path, const std::function<bool()>& current) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IShellItem> item;
    ComPtr<IDropTarget> target;
    ComPtr<IDataObject> data;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) ||
        FAILED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&target))) ||
        !current() || FAILED(OleGetClipboard(&data)) || !data || !current()) return false;
    DWORD effect = DROPEFFECT_COPY;
    const auto hr = target->DragEnter(data.Get(), MK_CONTROL, POINTL{}, &effect);
    if (FAILED(hr)) return false;
    target->DragLeave();
    return (effect & DROPEFFECT_COPY) != 0;
}

// Match the focused file pane to its actual tab's Shell view. The address bar,
// file rename editor and sibling tabs can all share the same top-level HWND.
std::wstring explorer_folder(HWND foreground, HWND focus, const char*& status,
                             Microsoft::WRL::ComPtr<IUnknown>& identity) {
    using Microsoft::WRL::ComPtr;
    wchar_t cls[64]{};
    status = "outside-file-pane";
    GetClassNameW(focus, cls, static_cast<int>(std::size(cls)));
    if (_wcsicmp(cls, L"DirectUIHWND") && _wcsicmp(cls, L"SysListView32")) return {};
    status = "no-shell-view";
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))) return {};
    long count{};
    if (FAILED(windows->get_Count(&count))) return {};
    for (long i = 0; i < count; ++i) {
        VARIANT index{};
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> disp;
        ComPtr<IWebBrowserApp> app;
        SHANDLE_PTR handle{};
        if (FAILED(windows->Item(index, &disp)) || !disp || FAILED(disp.As(&app)) ||
            FAILED(app->get_HWND(&handle)) || reinterpret_cast<HWND>(handle) != foreground) continue;
        status = "different-tab";
        ComPtr<IServiceProvider> services;
        ComPtr<IShellBrowser> browser;
        ComPtr<IShellView> view;
        HWND pane{};
        if (FAILED(disp.As(&services)) ||
            FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
            FAILED(browser->QueryActiveShellView(&view)) || FAILED(view->GetWindow(&pane)) ||
            (pane != focus && !IsChild(pane, focus))) continue;
        status = "unidentified-folder";
        ComPtr<IFolderView> folder_view;
        ComPtr<IPersistFolder2> folder;
        PIDLIST_ABSOLUTE id{};
        if (FAILED(view.As(&folder_view)) || FAILED(folder_view->GetFolder(IID_PPV_ARGS(&folder))) ||
            FAILED(folder->GetCurFolder(&id))) continue;
        PWSTR name{};
        const auto named = SHGetNameFromIDList(id, SIGDN_FILESYSPATH, &name);
        CoTaskMemFree(id);
        std::wstring path;
        if (SUCCEEDED(named) && name) path = name;
        CoTaskMemFree(name);
        if (!path.empty() && SUCCEEDED(view.As(&identity))) {
            status = "folder";
            return path;
        }
    }
    return {};
}


template<class F> auto in_mta(F work) {
    Handle done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!done.value) throw std::runtime_error("UIA completion event unavailable");
    auto result = std::async(std::launch::async, [work = std::move(work), event = done.value]() mutable {
        struct Signal { HANDLE event; ~Signal() { SetEvent(event); } } signal{event};
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) throw std::runtime_error("UIA apartment unavailable");
        struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
        return work();
    });
    // UIA can call back into a Shell extension while this STA retains the original view.
    DWORD index{};
    if (FAILED(CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES,
                                        INFINITE, 1, &done.value, &index)))
        throw std::runtime_error("UIA completion wait failed");
    return result.get();
}
bool stayed_at_binding(const Channel& c, bool witnessed_press) {
    LASTINPUTINFO input{sizeof(input)};
    bool same_focus;
    bool have_input;
#ifdef CLIPROPE_DESKTOP_PROBES
    if (auto receiver = target_fixture::receiver()) {
        same_focus = reinterpret_cast<UINT_PTR>(receiver) == c.field;
        input.dwTime = c.input_tick + (GetPropW(receiver, target_fixture::changed_input) ? 1 : 0);
        have_input = true;
    } else
#endif
    {
        same_focus = valid_focus(c);
        have_input = GetLastInputInfo(&input) != FALSE;
    }
    // The desktop checks all observed input before accepting a witnessed binding.
    return same_focus && (witnessed_press || (have_input && input.dwTime == c.input_tick)) &&
           GetTickCount64() <= c.bind_deadline;
}
void hold_probe([[maybe_unused]] const wchar_t* point) {
#ifdef CLIPROPE_DESKTOP_PROBES
    target_fixture::signal_hold(point);
#endif
}
// The apartment retaining the original selection performs both checks.
template<class Check>
void exchange(Channel& c, HANDLE bound, HANDLE request, HANDLE reply, Kind kind, Check check) {
    c.kind = stayed_at_binding(c, guarded_editor(kind) && c.guarded_card_press) ? kind : Kind::none;
    c.bound_serial = c.serial;
    c.bound_at = GetTickCount64();
    if (!SetEvent(bound) || c.kind == Kind::none || c.kind == Kind::warm) return;
    for (;;) {
        const auto wake = MsgWaitForMultipleObjects(1, &request, FALSE, INFINITE, QS_ALLINPUT);
        if (wake == WAIT_OBJECT_0) break;
        if (wake != WAIT_OBJECT_0 + 1) return;
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    }
    const auto focused = [&] {
#ifdef CLIPROPE_DESKTOP_PROBES
        if (auto receiver = target_fixture::receiver()) return reinterpret_cast<UINT_PTR>(receiver) == c.field;
#endif
        return valid_focus(c);
    };
    const auto current = [&] { return c.sequence && c.sequence == GetClipboardSequenceNumber() &&
                                     focused() && GetTickCount64() < c.verify_deadline; };
    auto result = Verdict::lost;
    if (c.payload <= DWORD(PastePayload::image_files) && current())
        result = check(PastePayload(c.payload), current);
    if (!current()) result = c.sequence == GetClipboardSequenceNumber() ? Verdict::lost : Verdict::replaced;
    c.verdict = result;
    c.replied_at = GetTickCount64();
    c.reply_serial = c.serial;
#ifdef CLIPROPE_DESKTOP_PROBES
    if (target_fixture::gate() == L"uia-stale-reply") ++c.reply_serial;
#endif
    SetEvent(reply);
}
int run(Channel& c, HANDLE bound, HANDLE request, HANDLE reply) {
#ifdef CLIPROPE_DESKTOP_PROBES
    if (auto receiver = target_fixture::receiver())
        if (!target_fixture::release_at(receiver, L"uia-before", c.input_tick)) return 2;
#endif
    if (c.magic != signature || c.size != sizeof(c) || !c.serial || c.bind_deadline > GetTickCount64()+bind_ms ||
        !stayed_at_binding(c, c.guarded_card_press != 0)) return 2;
    const HWND root = reinterpret_cast<HWND>(c.foreground), field = reinterpret_cast<HWND>(c.field);
#ifdef CLIPROPE_DESKTOP_PROBES
    if (target_fixture::receiver() == field) {
        // The noninteractive station cannot supply a foreground Office or Explorer selection.
        // Exercise the same two-phase exchange and deadlines with a held provider boundary.
        const auto stage = target_fixture::gate();
        const auto dash = stage.find(L'-');
        const auto adapter = stage.substr(0, dash);
        const auto kind = adapter == L"uia" ? Kind::uia : adapter == L"office" ? Kind::office : Kind::folder;
        auto probe = [&] {
            hold_probe((adapter + L"-bind").c_str());
            const auto identity = GetPropW(field, target_fixture::selection);
            if (!target_fixture::release_at(field, L"uia-reading", c.input_tick)) return 2;
            exchange(c, bound, request, reply, kind, [&](auto, const auto&) {
                hold_probe((adapter + L"-verify").c_str());
                if (!identity || GetPropW(field, target_fixture::selection) != identity) return Verdict::lost;
                return Verdict::accepted;
            });
            return 0;
        };
        return kind == Kind::uia ? in_mta(probe) : probe();
    }
#endif
    const auto app = process_basename(c.pid);
    const bool explorer = !_wcsicmp(app.c_str(), L"explorer.exe");
    const auto office = office_app(app.c_str());
    if (explorer) {
        hold_probe(L"shell-bind");
        const char* status{};
        Microsoft::WRL::ComPtr<IUnknown> original_view;
        auto folder = explorer_folder(root, field, status, original_view);
        if (!folder.empty()) {
            strcpy_s(c.description, "shell-folder");
            exchange(c, bound, request, reply, Kind::folder, [&](PastePayload payload, const auto& current) {
                if (payload != PastePayload::files && payload != PastePayload::image_files) return Verdict::refused;
                hold_probe(L"shell-verify");
                const auto until = GetTickCount64() + 750;
                const auto same_tab = [&] {
                    Microsoft::WRL::ComPtr<IUnknown> active_view;
                    return explorer_folder(root, field, status, active_view) == folder &&
                           active_view.Get() == original_view.Get() && current();
                };
                do {
                    if (!same_tab()) return Verdict::lost;
                    if (in_mta([root] { return query_paste_button(root); }))
                        return same_tab() ? Verdict::accepted : Verdict::lost;
                    pump_clipboard_reader(20);
                } while (GetTickCount64() < until && current());
                return same_tab() && folder_accepts_copy(folder, current) && same_tab() ? Verdict::accepted : Verdict::refused;
            });
            return 0;
        }
    }
    if (HWND view = office_document_view(field, office)) {
        hold_probe(L"office-bind");
        OfficeSelection selection;
        const bool accepted = selection.bind(view, office);
        strcpy_s(c.description, "office-selection");
        exchange(c, bound, request, reply, accepted ? Kind::office : Kind::none, [&](PastePayload, const auto&) {
            hold_probe(L"office-verify");
            return selection.matches() ? Verdict::accepted : Verdict::lost;
        });
        return 0;
    }
    // UIA objects never migrate to the Shell STA. The worker process retains their MTA until verification ends.
    return in_mta([&] {
        FocusClient client;
        if (c.warm) {
            if (auto* automation = client.for_window(root, c.pid)) {
                Microsoft::WRL::ComPtr<IUIAutomationElement> element;
                automation->ElementFromHandle(root, &element);
            }
            exchange(c, bound, request, reply, Kind::warm, [](auto, const auto&) { return Verdict::refused; });
            return 0;
        }
        hold_probe(L"uia-bind");
        FocusField selection;
        const bool accepted = selection.bind(root, c.pid, client) && (!explorer || selection.accepts(PastePayload::other));
        sprintf_s(c.description, "uia %s type=%ld", selection.reason(), static_cast<long>(selection.control_type()));
        exchange(c, bound, request, reply, accepted ? Kind::uia : Kind::none, [&](PastePayload payload, const auto&) {
            hold_probe(L"uia-verify");
            if (!selection.accepts(payload) || (payload == PastePayload::image_files &&
                (app.empty() || explorer || !_wcsicmp(app.c_str(), L"Notepad.exe") || !selection.allows_file_paste())))
                return Verdict::refused;
            return selection.revalidate(payload) ? Verdict::accepted : Verdict::lost;
        });
        return 0;
    });
}
} // namespace
} // namespace cliprope::win::target
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    using namespace cliprope::win::target;
    BOOL supervised{};
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &supervised) || !supervised) return 2;
    int argc{};
    std::unique_ptr<wchar_t*, decltype(&LocalFree)> argv(CommandLineToArgvW(GetCommandLineW(), &argc), LocalFree);
    if (!argv || argc != 5 || FAILED(OleInitialize(nullptr))) return 2;
    struct Apartment { ~Apartment() { OleUninitialize(); } } apartment;
    try {
        auto handle = [&](int i) {
            const std::wstring arg(argv.get()[i]);
            if (arg.empty() || arg.find_first_not_of(L"0123456789") != arg.npos) throw std::invalid_argument("handle");
            return reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(std::stoull(arg)));
        };
        Handle section(handle(1)), bound(handle(2)), request(handle(3)), reply(handle(4));
        auto* address = MapViewOfFile(section.value, FILE_MAP_WRITE, 0, 0, sizeof(Channel));
        if (!address) return 2;
        std::unique_ptr<void, decltype(&UnmapViewOfFile)> view(address, UnmapViewOfFile);
        return run(*static_cast<Channel*>(address), bound.value, request.value, reply.value);
    } catch (...) { return 1; }
}
