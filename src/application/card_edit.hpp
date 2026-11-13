#pragma once
#include "application/browse.hpp"
#include "application/history_failure.hpp"
#include <stdexcept>

namespace cliprope::application {
struct TextDraft {
    domain::Id entry_id;
    std::int64_t revision{};
    std::wstring text;
    bool rich{};
};
inline constexpr std::size_t max_edit_units = 4 * 1024 * 1024 - 1;
class StaleEdit final : public HistoryFailure {
public:
    explicit StaleEdit(bool missing = false)
        : HistoryFailure(HistoryIssue::stale_binding, "The card changed or was deleted"), deleted(missing) {}
    bool deleted;
};
} // namespace cliprope::application
