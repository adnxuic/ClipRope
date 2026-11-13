#include "platform_win/color_picker.hpp"
#include "platform_win/card_menu_frame.hpp"
#include "platform_win/menu_bitmap.hpp"
#include "platform_win/menu_timing.hpp"
#include <dbghelp.h>
#include <dwmapi.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <ole2.h>
#include <uiautomation.h>
#include <wrl.h>

using namespace cliprope;
namespace {
int mode{}, step{}, previews{}, commits{}, cancels{};
bool failed{};
bool target_alive = true;
std::future<bool> accessibility;
std::ofstream performance;
int repetition{};
double ticks{};
bool inspect_accessibility(HWND picker) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool result = false;
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IUIAutomation> automation;
        if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&automation)))) {
            ComPtr<IUIAutomationElement> hue, sv;
            auto child = [&](int id) {
                HWND result{};
                struct Lookup {
                    int id;
                    HWND *result;
                } lookup{id, &result};
                EnumChildWindows(
                    picker,
                    [](HWND h, LPARAM p) -> BOOL {
                        auto &l = *reinterpret_cast<Lookup *>(p);
                        if (GetDlgCtrlID(h) == l.id)
                            *l.result = h;
                        return TRUE;
                    },
                    LPARAM(&lookup));
                return result;
            };
            if (SUCCEEDED(automation->ElementFromHandle(child(4141), &hue)) &&
                SUCCEEDED(automation->ElementFromHandle(child(4140), &sv))) {
                ComPtr<IUIAutomationRangeValuePattern> range;
                ComPtr<IUIAutomationValuePattern> pair;
                if (SUCCEEDED(hue->GetCurrentPatternAs(UIA_RangeValuePatternId, IID_PPV_ARGS(&range))) &&
                    SUCCEEDED(sv->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&pair)))) {
                    auto valid = SysAllocString(L"50, 60"), invalid = SysAllocString(L"101, 0");
                    double value{};
                    result = SUCCEEDED(range->SetValue(240)) && SUCCEEDED(range->get_CurrentValue(&value)) &&
                             value == 240 && FAILED(range->SetValue(361)) &&
                             SUCCEEDED(pair->SetValue(valid)) && FAILED(pair->SetValue(invalid));
                    SysFreeString(valid);
                    SysFreeString(invalid);
                }
            }
        }
    }
    CoUninitialize();
    return result;
}
std::filesystem::path captures;
void capture(HWND h, const char *name) {
    if (captures.empty())
        return;
    RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    RECT r;
    GetClientRect(h, &r);
    win::MenuBitmap bitmap;
    if (!bitmap.create(r.right, r.bottom)) {
        failed = true;
        return;
    }
    auto dc = GetDC(h);
    if (std::string_view(name) == "picker.bmp") {
        auto title = GetDlgItem(h, 4123);
        LOGFONTW font{};
        GetObjectW(HGDIOBJ(SendMessageW(title, WM_GETFONT, 0, 0)), sizeof(font), &font);
        std::wcout << L"Picker title font: " << font.lfFaceName << L", height=" << font.lfHeight << L"\n";
    }
    BitBlt(bitmap.dc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
    ReleaseDC(h, dc);
    GdiFlush();
    BITMAPFILEHEADER file{0x4d42, DWORD(54 + r.right * r.bottom * 4), 0, 0, 54};
    BITMAPINFOHEADER info{40, r.right, -r.bottom, 1, 32, BI_RGB};
    std::ofstream output(captures / name, std::ios::binary);
    output.write(reinterpret_cast<char *>(&file), sizeof(file));
    output.write(reinterpret_cast<char *>(&info), sizeof(info));
    output.write(reinterpret_cast<char *>(bitmap.pixels), r.right * r.bottom * 4);
}
HWND find(HWND root, int id) {
    struct Search {
        int id;
        HWND result;
    };
    Search search{id, {}};
    EnumChildWindows(
        root,
        [](HWND h, LPARAM data) -> BOOL {
            auto &s = *reinterpret_cast<Search *>(data);
            if (GetDlgCtrlID(h) == s.id) {
                s.result = h;
                return FALSE;
            }
            return TRUE;
        },
        LPARAM(&search));
    return search.result;
}
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void CALLBACK drive(HWND owner, UINT, UINT_PTR, DWORD) {
    auto picker = FindWindowW(L"ClipRope.ColorPicker", nullptr);
    if (!picker)
        return;
    RECT bounds{}, work{};
    GetWindowRect(picker, &bounds);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(picker, MONITOR_DEFAULTTONEAREST), &monitor);
    work = monitor.rcWork;
    if (bounds.left < work.left || bounds.top < work.top || bounds.right > work.right ||
        bounds.bottom > work.bottom)
        failed = true;
    if (mode == 10) {
        const auto &times = win::menu_times;
        const double painted = (times[size_t(win::MenuStage::painted)].QuadPart -
                                times[size_t(win::MenuStage::input)].QuadPart) /
                               ticks;
        LARGE_INTEGER begin{}, end{};
        QueryPerformanceCounter(&begin);
        RedrawWindow(picker, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        DwmFlush();
        QueryPerformanceCounter(&end);
        const double fenced = (end.QuadPart - times[size_t(win::MenuStage::input)].QuadPart) / ticks;
        QueryPerformanceCounter(&begin);
        auto axis = find(picker, 4140);
        RECT r{};
        GetClientRect(axis, &r);
        SendMessageW(axis, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(0, 0));
        for (int i = 0; i < 60; ++i) {
            SendMessageW(axis, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(r.right * i / 60, r.bottom * i / 60));
            RedrawWindow(picker, nullptr, nullptr, RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
        SendMessageW(axis, WM_LBUTTONUP, 0, MAKELPARAM(r.right - 1, r.bottom - 1));
        QueryPerformanceCounter(&end);
        performance << repetition << ',' << painted << ',' << fenced << ','
                    << (end.QuadPart - begin.QuadPart) / ticks / 60 << ','
                    << GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) << ','
                    << win::CardMenuFrame::generations() << ',' << win::CardMenuFrame::cache_bytes() << '\n';
        SendMessageW(picker, WM_CLOSE, 0, 0);
        return;
    }
    if (mode == 5) {
        if (++step == 1) {
            accessibility =
                std::async(std::launch::async, [picker] { return inspect_accessibility(picker); });
        } else if (accessibility.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            if (!accessibility.get())
                failed = true;
            SendMessageW(picker, WM_CLOSE, 0, 0);
        }
        return;
    }
    if (mode == 6) {
        if (++step == 1)
            SendMessageW(find(picker, 4103), BM_CLICK, 0, 0);
        else
            target_alive = false;
        return;
    }
    if (mode >= 3) {
        ++step;
        if (step == 1) {
            if (mode == 3) {
                SendMessageW(find(picker, 4102), BM_CLICK, 0, 0);
                capture(picker, "picker.bmp");
            }
        } else if (step == 2) {
            if (!IsWindowVisible(find(picker, 4140)) || !IsWindowVisible(find(picker, 4150)))
                failed = true;
            SetWindowTextW(find(picker, 4150), L"#12");
            if (IsWindowEnabled(find(picker, IDOK)))
                failed = true;
            SendMessageW(picker, WM_SETTINGCHANGE, 0, 0);
            if (win::fluent::caption(find(picker, 4150)) != L"#12" || IsWindowEnabled(find(picker, IDOK)))
                failed = true;
            for (const auto invalid : {L"12, 34", L"256, 34, 56", L"-1, 34, 56", L"1, 2, 3, 4"}) {
                SetWindowTextW(find(picker, 4151), invalid);
                if (IsWindowEnabled(find(picker, IDOK)))
                    failed = true;
            }
            SetWindowTextW(find(picker, 4151), L"17, 34, 51");
            if (!IsWindowEnabled(find(picker, IDOK)) ||
                win::fluent::caption(find(picker, 4150)) != L"#112233")
                failed = true;
            SetWindowTextW(find(picker, 4150), L"#11223344");
            if (!IsWindowEnabled(find(picker, IDOK)))
                failed = true;
            PostMessageW(find(picker, 4150), WM_KEYDOWN, VK_RETURN, 0);
        } else if (step == 3) {
            if (mode == 3)
                capture(picker, "validation.bmp");
            if (mode == 3)
                SendMessageW(find(picker, IDOK), BM_CLICK, 0, 0);
            else {
                SendMessageW(find(picker, 4141), WM_KEYDOWN, VK_RIGHT, 0);
                SendMessageW(find(picker, 4142), WM_KEYDOWN, VK_HOME, 0);
                SendMessageW(picker, WM_CLOSE, 0, 0);
            }
        }
        return;
    }
    if (++step == 1) {
        if (!find(picker, 4100) || !find(picker, 4107) || find(picker, 4161) || find(picker, 4120) ||
            !find(picker, 4140) || !find(picker, 4151))
            failed = true;
        if (mode == 0) {
            SetWindowTextW(find(picker, 4150), L"#FBF7EF");
            if (SendMessageW(find(picker, 4100), BM_GETCHECK, 0, 0) != BST_UNCHECKED)
                failed = true;
            SendMessageW(find(picker, 4100), BM_CLICK, 0, 0);
            if (SendMessageW(find(picker, 4100), BM_GETCHECK, 0, 0) != BST_CHECKED)
                failed = true;
        }
        SendMessageW(find(picker, 4102), BM_CLICK, 0, 0);
    } else {
        if (step == 2 && mode)
            SendMessageW(GetDlgItem(picker, IDOK), BM_CLICK, 0, 0);
        else {
            SendMessageW(picker, WM_CLOSE, 0, 0);
            KillTimer(owner, 1);
        }
    }
}
} // namespace
int main(int argc, char **argv) {
    try {
        SetUnhandledExceptionFilter([](EXCEPTION_POINTERS *fault) -> LONG {
            auto process = GetCurrentProcess();
            SymInitialize(process, nullptr, TRUE);
            STACKFRAME64 frame{};
            auto context = *fault->ContextRecord;
            frame.AddrPC = {context.Rip, 0, AddrModeFlat};
            frame.AddrStack = {context.Rsp, 0, AddrModeFlat};
            frame.AddrFrame = {context.Rbp, 0, AddrModeFlat};
            for (int i = 0; i < 24; ++i) {
                char buffer[sizeof(SYMBOL_INFO) + 512]{};
                auto symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 511;
                DWORD64 displacement{};
                if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
                    std::cerr << symbol->Name << " + " << displacement << '\n';
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context,
                                 nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                    break;
            }
            return EXCEPTION_EXECUTE_HANDLER;
        });
        OleInitialize(nullptr);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        localization::LocalizationManager language;
        language.set_language(localization::Language::automatic, win::preferred_ui_locale());
        win::UiLocalization binding(language);
        const bool benchmark = argc > 2 && std::string_view(argv[1]) == "--benchmark";
        if (argc > 1 && !benchmark) {
            captures = argv[1];
            std::filesystem::create_directories(captures);
        }
        auto owner = CreateWindowExW(0, L"STATIC", L"Color fixture", WS_OVERLAPPEDWINDOW, 100, 100, 400, 200,
                                     nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        auto anchor = CreateWindowW(L"BUTTON", L"Color", WS_CHILD | WS_VISIBLE, 20, 20, 140, 36, owner,
                                    nullptr, GetModuleHandleW(nullptr), nullptr);
        win::fluent::Surface owner_theme;
        owner_theme.attach(owner);
        win::color_picker_button(owner_theme, anchor, {}, domain::paper_default);
        ShowWindow(owner, SW_SHOW);
        RedrawWindow(owner, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        if (benchmark) {
            performance.open(argv[2]);
            performance << "trial,open_paint_ms,open_fence_ms,drag_frame_ms,gdi,"
                           "frame_generations,"
                           "frame_bytes\n";
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            ticks = frequency.QuadPart / 1000.;
            const int count = argc > 3 ? std::stoi(argv[3]) : 101;
            mode = 10;
            for (repetition = 0; repetition < count; ++repetition) {
                win::ColorPickerOptions options;
                options.allow_alpha = true;
                SetTimer(owner, 1, 1, drive);
                win::ColorPickerPopup::track(owner, anchor, std::move(options));
                KillTimer(owner, 1);
            }
            DestroyWindow(owner);
            OleUninitialize();
            return failed ? 1 : 0;
        }
        for (mode = 0; mode < 7; ++mode) {
            if (mode == 4) {
                MONITORINFO monitor{sizeof(monitor)};
                GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
                SetWindowPos(owner, nullptr, monitor.rcWork.right - 100, monitor.rcWork.bottom - 100, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER);
            }
            if (mode == 5)
                SetWindowPos(owner, nullptr, -50, -50, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            step = 0;
            win::ColorPickerOptions options;
            options.default_color = domain::paper_default;
            options.callbacks.preview = [](auto) { ++previews; };
            options.callbacks.cancel = [] { ++cancels; };
            options.allow_alpha = true;
            options.callbacks.valid = [] { return target_alive; };
            if (mode == 3)
                options.value = 0x433a31;
            else {
                options.contrast_ink = 0x666666;
                options.translucent = true;
            }
            options.callbacks.commit = [](auto color) {
                if (mode == 2)
                    throw std::runtime_error("Injected save failure");
                if (mode == 3 && color != domain::ColorSelection(domain::ColorRGBA8{17, 34, 51, 68}))
                    failed = true;
                ++commits;
                return presentation::ColorCommit::saved;
            };
            SetTimer(owner, 1, 70, drive);
            bool saved = win::ColorPickerPopup::track(owner, anchor, std::move(options));
            KillTimer(owner, 1);
            require(saved == (mode == 1 || mode == 3), "Incorrect native completion");
        }
        require(!failed && previews >= 5 && commits == 2 && cancels == 5,
                "Native preview/commit/cancel boundary");
        DestroyWindow(owner);
        OleUninitialize();
        std::cout << "Native full picker, combined RGB, UI Automation, theme/input "
                     "and cancellation passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
