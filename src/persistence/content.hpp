#pragma once

#include "domain/history.hpp"
#include "persistence/database.hpp"
#include "application/card_search.hpp"
#include "application/screenshot_preview.hpp"

namespace cliprope::persistence {

std::filesystem::path managed_screenshot(Database&, const domain::Entry&, std::span<const unsigned char>);

class PreparedContent {
public:
    explicit PreparedContent(const domain::Snapshot&, const Database::Fault& probe = {});
private:
    friend class ContentRepository;
    const domain::Snapshot& snapshot;
    application::SearchFields fields;
};

struct CollectionResult {
    std::size_t removed{}, deferred{};
};

class ContentRepository final {
public:
    explicit ContentRepository(Database& db) : db_(db) {}
    // The caller owns the writer mutex and the entry's admission transaction.
    std::string install(const domain::Snapshot& snapshot);
    std::string install(const PreparedContent& content);
    domain::LeasedSnapshot load(std::string_view snapshot_id, std::stop_token stop = {});
    // Select and pin one saved screenshot representation without decoding or reading external bytes.
    application::ScreenshotSource screenshot(const domain::Entry&, std::stop_token, application::PreviewDeadline);
    CollectionResult collect();
    static constexpr std::size_t inline_limit = 4096;
private:
    Database& db_;
};

} // namespace cliprope::persistence
