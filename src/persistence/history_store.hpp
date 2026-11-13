#pragma once
#include "application/browse.hpp"
#include "application/collector_head.hpp"

#include "application/history.hpp"
#include "application/expand_file_group.hpp"
#include "application/card_edit.hpp"
#include "application/rope_changes.hpp"
#include "persistence/content.hpp"
#include <atomic>

namespace cliprope::persistence {

class HistoryStore final : public application::History {
public:
    explicit HistoryStore(const std::filesystem::path& root, Database::Fault fault = {});
    Database& database() { return db_; }
    CollectionResult gc();
    CollectionResult startup_collection() const { return startup_collection_; }

    domain::Id active_rope() override;
    application::CollectorHead collector_head();
    // Called on the writer's thread, after commit and outside the database lock.
    std::function<void(const application::HeadChange&)> on_head_change;
    std::uint64_t failed_head_notifications() const noexcept { return failed_head_notifications_.load(); }
    domain::Id create_rope(std::string_view name, int capacity);
    void set_active_rope(std::string_view id, bool hide_rebin = false);
    void set_rebin_visible(bool visible);
    bool rebin_visible();
    // Insert in the circular order beginning at expected_head and select its new head atomically.
    domain::Id reorder_rope(std::string_view source, std::string_view target, std::string_view expected_head);
    double rope_top(std::string_view id);
    void save_rope_top(std::string_view id, double dip);
    void erase_rope(std::string_view id);
    void set_capacity(std::string_view id, int capacity);
    int default_capacity();
    bool rope_compact(std::string_view id);
    void save_rope_compact(std::string_view id, bool compact, const application::Anchor& anchor);
    void save_anchor(std::string_view rope_id, const application::Anchor& anchor);
    void save_path_kind(const domain::Entry&, domain::PathKind);
    domain::PathKind confirm_menu_kind(const application::CardMenuSnapshot&);
    application::RopePreview preview_rope_change(const application::RopeChange& change);
    std::optional<domain::Id> apply_rope_change(const application::RopePreview& preview);

    domain::Entry entry(std::string_view id) override;
    std::vector<domain::Entry> hanging(std::string_view rope_id);
    std::vector<domain::Entry> floating();
    domain::Id create_floating_text(const domain::Layout& layout);
    domain::Id prepare_note_id();
    void create_floating_text(const domain::Id& id, const domain::Layout& layout);
    domain::Layout layout(std::string_view entry_id);
    void resize(const domain::Entry& expected, domain::CardScale scale, const domain::Layout* placement = nullptr);
    void save_layout(std::string_view entry_id, const domain::Layout& layout);
    void reveal_floating(const domain::Entry& expected, const domain::Layout& saved, const domain::Layout& placed);
    domain::LeasedSnapshot load(std::string_view entry_id) override;
    domain::LeasedSnapshot load_snapshot(std::string_view snapshot_id) override;
    bool has_external_payload(std::string_view snapshot_id);
    application::PreparedSnapshot read_for_display(std::string_view snapshot_id, std::stop_token stop);
    std::filesystem::path materialize_screenshot(const domain::Entry& entry, std::span<const unsigned char> png);
    // A missing time window denotes an overlay-tracked snip. The untracked route also
    // requires a unique PNG among captures from the same application and time window.
    std::string snip_original(const domain::Id& snapshot, std::string_view candidate,
                              std::optional<std::pair<std::int64_t, std::int64_t>> window, bool remember);
    std::function<void()> on_snip_bound;
    domain::Id admit(std::string_view rope_id, const domain::Snapshot& snapshot) override;
    domain::Id admit_capture(std::string_view bound_rope, const domain::Snapshot& snapshot);
    void promote(std::string_view entry_id, const domain::Entry* expected = nullptr) override;
    void reorder(std::string_view entry_id, std::size_t zero_based_position,
                 const domain::Entry* expected = nullptr);
    void detach(std::string_view entry_id, const domain::Layout& layout = {},
                const domain::Entry* expected = nullptr);
    void rehang(std::string_view entry_id, std::string_view bound_rope_id,
                const domain::Entry* expected = nullptr) override;
    void rehang_at(std::string_view entry_id, std::string_view target_rope, std::size_t position,
                   const domain::Entry* expected);
    void move(std::string_view entry_id, std::string_view target_rope, const domain::Entry* expected = nullptr,
              std::size_t position = 0);
    domain::Id copy(std::string_view entry_id, std::string_view target_rope, const domain::Entry* expected = nullptr,
                    std::size_t position = 0);
    void erase(std::string_view entry_id, const domain::Entry* expected = nullptr);
    application::ExpandFileGroup prepare_expansion(const domain::Entry& expected, bool keep_source = true);
    application::ExpansionImpact preview_expansion(const application::ExpandFileGroup& group);
    // False requests a fresh preview. No entries have been changed in that case.
    bool expand_file_group(const application::ExpandFileGroup& group, const application::ExpansionImpact& approved,
                           const std::vector<domain::Layout>& layouts = {});
    application::TextDraft begin_text_edit(std::string_view entry_id);
    domain::Id save_text(std::string_view entry_id, std::int64_t expected_revision, std::wstring_view text);
    void save_style(std::string_view entry_id, const application::CardStyle& style, bool set_font = true);
    void save_color(const domain::Entry &expected, bool paper, domain::ColorSelection original,
                    domain::ColorSelection color);
    void save_rope_color(std::string_view id, domain::ColorSelection original, domain::ColorSelection color);

  private:
    struct NormalizedHead {
        domain::Id rope, head;
        std::int64_t data_version{}, schema_version{}, changes{};
    };
    void select_collector(std::string_view id);
    void mutate(const std::function<void()>& change, bool yield_while_busy = false);
    void mutate_head(const std::function<void()>& change, bool yield_while_busy = false);
    std::optional<NormalizedHead> reorder_in_transaction(std::string_view id, std::size_t position,
                                                        const domain::Entry* expected, bool promotion);
    void remember_normalization(std::optional<NormalizedHead> result);
    domain::Id insert_rope(std::string_view name, int capacity, domain::ColorSelection color);
    void write_floating_layout(std::string_view id, const domain::Layout& layout);
    void transfer(std::string_view id, std::string_view target, bool require_floating,
                  const domain::Entry* expected, std::size_t position);
    Database db_;
    ContentRepository content_;
    CollectionResult startup_collection_;
    // GC scans staged files after its SQL transaction. Exclude a concurrent capture install.
    std::mutex admission_files_;
    std::uint64_t head_serial_{};
    std::atomic<std::uint64_t> failed_head_notifications_{};
    std::optional<NormalizedHead> normalized_;
};

} // namespace cliprope::persistence
