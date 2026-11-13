#pragma once
#include <windows.h>
#include <oaidl.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include "platform_win/office_view.hpp"

namespace cliprope::win {
// Query only the document view under the bound HWND. No global active Office instance.
class OfficeSelection {
public:
    bool bind(HWND view, OfficeApp app);
    bool matches() const;
private:
    struct Position {
        Microsoft::WRL::ComPtr<IUnknown> document, sheet;
        std::wstring address, active_cell;
        long start{}, end{}, text_length{};
        long pane{}, selection_type{}, slide{}, text_shape{};
        bool child_shapes{};
        std::vector<long> selected_ids;
        bool operator==(const Position& other) const = default;
    };
    bool read_powerpoint(Position& position, IDispatch* window, IDispatch* selection) const;
    bool read(Position& position, IDispatch* window) const;
    HWND view_{};
    Position bound_;
    OfficeApp app_{};
};
}
