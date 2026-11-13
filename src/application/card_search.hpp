#pragma once
#include "domain/history.hpp"
#include <string_view>

namespace cliprope::application {
enum class SearchField { body, link, path, metadata };
using SearchFields = std::vector<std::pair<SearchField, std::wstring>>;
struct FindScope {
    domain::Id first_rope;
    bool overview{}, include_rebin{};
    bool operator==(const FindScope&) const = default;
};
struct MatchRange {
    std::size_t start{}, length{};
    bool operator==(const MatchRange&) const = default;
};
struct SearchHit {
    domain::Id entry, snapshot, rope;
    int ordinal{};
    SearchField field{};
    MatchRange range;
    std::wstring context;
    std::size_t context_start{}, occurrence{};
};
inline wchar_t find_fold(wchar_t c) {
    return c >= L'A' && c <= L'Z' ? c + 32 : c;
}
inline void fold_find(std::wstring& value) {
    for (auto& c : value)
        c = find_fold(c);
}
} // namespace cliprope::application
