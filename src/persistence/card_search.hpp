#pragma once
#include "application/card_search.hpp"
#include "persistence/database.hpp"
#include <memory>
#include <stop_token>

namespace cliprope::persistence {
struct MatchGroup {
    domain::Id entry, snapshot, rope;
    int ordinal{};
    application::SearchField field{};
    std::size_t begin{}, count{};
};
struct SearchResults {
    std::vector<MatchGroup> groups;
    std::size_t total{};
    std::wstring folded;
};
class CardSearch final {
public:
    explicit CardSearch(const std::filesystem::path& root);
    SearchResults query(const application::FindScope&, std::wstring term, std::stop_token stop = {});
    std::optional<application::SearchHit> locate(const SearchResults&, std::size_t index, std::stop_token stop = {});

private:
    struct PositionPage {
        domain::Id snapshot;
        int ordinal{};
        std::wstring term;
        size_t first{}, next_offset{};
        struct Context {
            application::MatchRange range;
            std::wstring text;
            size_t start{};
        };
        std::vector<Context> hits;
    };
    PositionPage page_;
    std::stop_token query_stop_;
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db_;
};
} // namespace cliprope::persistence
