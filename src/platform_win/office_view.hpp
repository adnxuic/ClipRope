#pragma once
#include <windows.h>
#include <iterator>

namespace cliprope::win {
enum class OfficeApp { other, word, excel, powerpoint };

inline OfficeApp office_app(const wchar_t* filename) {
    if (!_wcsicmp(filename, L"EXCEL.EXE")) return OfficeApp::excel;
    if (!_wcsicmp(filename, L"POWERPNT.EXE")) return OfficeApp::powerpoint;
    if (!_wcsicmp(filename, L"WINWORD.EXE")) return OfficeApp::word;
    return OfficeApp::other;
}

inline HWND office_document_view(HWND focus, OfficeApp app) {
    if (app == OfficeApp::other) return nullptr;
    const auto root = GetAncestor(focus, GA_ROOT);
    for (HWND view = focus; view; view = GetParent(view)) {
        wchar_t type[32]{};
        if (GetClassNameW(view, type, 32)) {
            if ((app == OfficeApp::excel && !_wcsicmp(type, L"EXCEL7")) ||
                (app == OfficeApp::word && !_wcsicmp(type, L"_WwG")) ||
                (app == OfficeApp::powerpoint &&
                    (!_wcsicmp(type, L"paneClassDC") || !_wcsicmp(type, L"mdiClass")))) return view;
        }
        if (view == root) break;
    }
    return nullptr;
}
} // namespace cliprope::win
