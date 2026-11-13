#include "platform_win/office_selection.hpp"
#include <oleacc.h>
#include <oleauto.h>

namespace cliprope::win {
using Microsoft::WRL::ComPtr;
namespace {
struct Value : VARIANT {
    Value() { VariantInit(this); }
    ~Value() { VariantClear(this); }
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
};

bool property(IDispatch* object, const wchar_t* name, Value& result, VARIANT* argument = nullptr,
              WORD flags = DISPATCH_PROPERTYGET) {
    if (!object) return false;
    DISPID member{};
    auto key = const_cast<wchar_t*>(name);
    DISPPARAMS args{argument, nullptr, argument ? 1u : 0u, 0};
    return SUCCEEDED(object->GetIDsOfNames(IID_NULL, &key, 1, LOCALE_USER_DEFAULT, &member)) &&
           SUCCEEDED(object->Invoke(member, IID_NULL, LOCALE_USER_DEFAULT, flags,
                                    &args, &result, nullptr, nullptr));
}

ComPtr<IDispatch> object(IDispatch* parent, const wchar_t* name, VARIANT* argument = nullptr) {
    Value value;
    // PowerPoint exposes collection Item as a method in its dispatch type library.
    const WORD flags = argument ? DISPATCH_METHOD | DISPATCH_PROPERTYGET : DISPATCH_PROPERTYGET;
    if (!property(parent, name, value, argument, flags) || value.vt != VT_DISPATCH) return {};
    return ComPtr<IDispatch>(value.pdispVal);
}

bool number(IDispatch* parent, const wchar_t* name, long& result) {
    Value value, converted;
    if (!property(parent, name, value) || value.vt == VT_EMPTY || value.vt == VT_NULL ||
        FAILED(VariantChangeType(&converted, &value, 0, VT_I4))) return false;
    result = converted.lVal;
    return true;
}

bool equals(IDispatch* parent, const wchar_t* name, long expected) {
    long result{};
    return number(parent, name, result) && result == expected;
}
bool address(IDispatch* range, std::wstring& result) {
    Value value;
    if (!property(range, L"Address", value) || value.vt != VT_BSTR || !value.bstrVal) return false;
    result.assign(value.bstrVal, SysStringLen(value.bstrVal));
    return !result.empty();
}
ComPtr<IDispatch> native_window(HWND view) {
    ComPtr<IDispatch> window;
    if (!view || FAILED(AccessibleObjectFromWindow(view, static_cast<DWORD>(OBJID_NATIVEOM), IID_PPV_ARGS(&window))))
        return {};
    return window;
}
}

bool OfficeSelection::bind(HWND view, OfficeApp app) {
    app_ = app;
    bound_ = {};
    view_ = nullptr;
    for (HWND hwnd = view; hwnd; hwnd = office_document_view(GetParent(hwnd), app)) {
        if (auto window = native_window(hwnd)) {
            if (!read(bound_, window.Get())) return false;
            view_ = hwnd;
            return true;
        }
        if (app != OfficeApp::powerpoint || hwnd == GetAncestor(view, GA_ROOT)) break;
    }
    return false;
}

bool OfficeSelection::read_powerpoint(Position& p, IDispatch* window, IDispatch* selection) const {
    auto document = object(window, L"Presentation");
    auto pane = object(window, L"ActivePane");
    auto view = object(window, L"View");
    if (!equals(document.Get(), L"ReadOnly", 0) || FAILED(document.As(&p.document)) ||
        !number(pane.Get(), L"ViewType", p.pane) || !number(selection, L"Type", p.selection_type)) return false;
    // Editing canvas, slide sorter, or thumbnail pane in Normal view.
    if (p.pane != 1 && p.pane != 7 && p.pane != 11) return false;
    if (p.pane == 1 && !number(object(view.Get(), L"Slide").Get(), L"SlideID", p.slide)) return false;
    if (p.selection_type == 0) return p.pane == 1;
    if (p.selection_type == 3) {
        auto text = object(selection, L"TextRange");
        auto frame = object(text.Get(), L"Parent");
        auto shape = object(frame.Get(), L"Parent");
        return p.pane == 1 && number(shape.Get(), L"Id", p.text_shape) &&
               number(text.Get(), L"Start", p.start) && number(text.Get(), L"Length", p.text_length);
    }
    const bool slides = p.selection_type == 1;
    if (!slides && (p.selection_type != 2 || p.pane != 1)) return false;
    const wchar_t* range_name = L"SlideRange";
    if (!slides) {
        long children{};
        if (!number(selection, L"HasChildShapeRange", children)) return false;
        p.child_shapes = children != 0;
        range_name = p.child_shapes ? L"ChildShapeRange" : L"ShapeRange";
    }
    auto range = object(selection, range_name);
    long count{};
    if (!number(range.Get(), L"Count", count) || count <= 0) return false;
    for (long i = 0; i < count; ++i) {
        Value index;
        index.vt = VT_I4;
        index.lVal = i + 1;
        auto item = object(range.Get(), L"Item", &index);
        long id{};
        if (!number(item.Get(), slides ? L"SlideID" : L"Id", id)) return false;
        p.selected_ids.push_back(id);
    }
    return true;
}

bool OfficeSelection::read(Position& position, IDispatch* window) const {
    auto selection = object(window, L"Selection");
    if (!selection) return false;
    if (app_ == OfficeApp::powerpoint) return read_powerpoint(position, window, selection.Get());
    if (app_ == OfficeApp::excel) {
        auto cell = object(window, L"ActiveCell");
        auto sheet = object(selection.Get(), L"Worksheet");
        auto book = object(sheet.Get(), L"Parent");
        long protected_sheet{};
        if (!sheet || !book ||
            !equals(book.Get(), L"ReadOnly", 0) || !number(sheet.Get(), L"ProtectContents", protected_sheet) ||
            (protected_sheet && !equals(selection.Get(), L"Locked", 0)) ||
            FAILED(sheet.As(&position.sheet)) || FAILED(book.As(&position.document))) return false;
        return address(selection.Get(), position.address) && address(cell.Get(), position.active_cell);
    }
    auto document = object(window, L"Document");
    auto range = object(selection.Get(), L"Range");
    long type{};
    Value in_control, flag, info;
    info.vt = VT_I4;
    info.lVal = 46; // wdInContentControl; this adapter qualifies ordinary document text.
    auto controls = object(range.Get(), L"ContentControls");
    return property(range.Get(), L"Information", in_control, &info) &&
           in_control.vt != VT_EMPTY && SUCCEEDED(VariantChangeType(&flag, &in_control, 0, VT_BOOL)) &&
           !flag.boolVal && equals(controls.Get(), L"Count", 0) &&
           document && range && SUCCEEDED(document.As(&position.document)) &&
           equals(document.Get(), L"ReadOnly", 0) && equals(document.Get(), L"ProtectionType", -1) &&
           equals(range.Get(), L"StoryType", 1) && number(selection.Get(), L"Type", type) &&
           (type == 1 || type == 2) && number(range.Get(), L"Start", position.start) &&
           number(range.Get(), L"End", position.end);
}

bool OfficeSelection::matches() const {
    Position now;
    // The same native pane may now expose a different document window.
    return read(now, native_window(view_).Get()) && now == bound_;
}
} // namespace cliprope::win
