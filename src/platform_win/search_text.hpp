#pragma once
#include "application/card_search.hpp"
#include <functional>

namespace cliprope::win {
using SearchTextProbe = std::function<void(std::string_view)>;
application::SearchFields extract_search_fields(const domain::Snapshot& snapshot, const SearchTextProbe& probe = {});
void append_search_metadata(application::SearchFields&, std::string_view screenshot_path, std::string_view source_hint);
}
