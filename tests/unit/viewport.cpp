#include "presentation/viewport.hpp"
#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace cliprope;
namespace {
void expect(bool pass, const char *problem) {
    if (!pass)
        throw std::runtime_error(problem);
}
void exercise() {
    std::vector<domain::Id> order;
    for (std::size_t i = 0; i < 1000; ++i)
        order.push_back(std::to_string(i));
    application::Anchor remembered;
    std::map<std::string, std::pair<bool, application::Anchor>> saved;
    bool fail_density = false;
    std::size_t max_read = 0;
    std::size_t fetched = 0;
    bool mixed = false;
    unsigned portrait_width = 1080;
    presentation::Viewport view({[&](auto) { return order.size(); },
                                 [&](auto, std::size_t pos, std::size_t count) {
                                     max_read = std::max(max_read, count);
                                     fetched += count;
                                     std::vector<application::CardSummary> page;
                                     for (std::size_t i = pos; i < pos + count; ++i) {
                                         application::CardSummary summary;
                                         summary.entry.id = order.at(i);
                                         if (mixed && (summary.entry.id == "0" || summary.entry.id == "1")) {
                                             summary.kind = application::CardKind::image;
                                             summary.source = summary.entry.id == "0" ? domain::ImageMetadata{1920,1080}
                                                 : domain::ImageMetadata{portrait_width,1920};
                                         }
                                         page.push_back(std::move(summary));
                                     }
                                     return page;
                                 },
                                 [&](auto, std::string_view id) -> std::optional<std::size_t> {
                                     const auto it = std::find(order.begin(), order.end(), id);
                                     if (it == order.end())
                                         return {};
                                     return static_cast<std::size_t>(it - order.begin());
                                 },
                                 [&](auto id) { return saved[std::string(id)].second; },
                                 [&](auto id, const auto &anchor) { saved[std::string(id)].second = remembered = anchor; },
                                 {}, {}, {},
                                 [&](auto id) { return saved[std::string(id)].first; },
                                 [&](auto id, bool compact, const auto& anchor) {
                                     if (fail_density) throw std::runtime_error("Simulated density failure");
                                     saved[std::string(id)] = {compact, anchor};
                                     remembered = anchor;
                                 }});
    view.open("main", 1180);
    expect(view.cards().size() == 7 && view.at_head(), "Unexpected initial viewport");
    view.pin_entry("2");
    const auto pinned_x = view.left(2);
    order.insert(order.begin(), "capture");
    view.refresh(1180);
    expect(view.left(3) == pinned_x, "Background capture displaced the edited sheet");
    view.pin_entry({});
    order.erase(order.begin());
    view.latest();
    view.scroll(10 * view.step() + 75);
    expect(view.anchor().entry == "10" && view.anchor().offset == 75, "Incorrect scrolled anchor");
    const float old_x = view.left(10);
    presentation::InsertionPreview gap{10, 12};
    expect(!gap.position(10, 1000) && gap.position(11, 1000) == 10 && gap.position(12, 1000) == 11 &&
           gap.position(13, 1000) == 13, "Preview failed to close the source and reserve the target slot");
    expect(view.insertion(view.left(12) + 32, gap, 1000) == 12, "Cursor inside a preview gap changed its index");
    presentation::InsertionPreview external{{}, 1};
    expect(external.position(0, 3) == 0 && external.position(1, 3) == 2 && !external.position(2, 3) &&
           view.insertion(10000, external, 3) == 2, "Full target preview advertised an evicted position");
    order.insert(order.begin(), "new");
    view.refresh(1180);
    expect(view.anchor().entry == "10" && view.left(11) == old_x && view.new_heads() == 1,
           "Head displaced anchor");
    order.erase(order.begin() + 11);
    view.refresh(1180);
    expect(view.anchor().entry == "11" && view.left(11) == old_x, "Deleted anchor lost its adjacent card");
    for (int i = 0; i < 80; ++i)
        view.scroll(1300);
    expect(max_read <= 9 && view.cards().front().entry.id == order.at(view.first_realized()),
           "Slot reuse or realization error");
    const auto prior = view.anchor();
    view.release();
    expect(view.cards().empty(), "Hidden viewport retained card summaries");
    view.open("main", 1180);
    expect(view.anchor().entry == prior.entry && view.anchor().offset == prior.offset,
           "Reopen lost scroll anchor");
    view.latest();
    expect(view.at_head() && remembered.entry.empty(), "Latest failed to reset anchor");
    view.set_compact(true);
    expect(view.compact() && view.paper()==86 && view.step()==110 && view.cards().size()==12,
           "Compact page must use the paper's measured width and shared gap");
    fetched=0;
    view.scroll(view.step());
    expect(fetched==1 && view.cards()[1].entry.id==order[1], "One wheel step should fetch only the newly exposed card");
    view.scroll(37);
    const auto compact_anchor = view.anchor();
    const auto compact_left = view.left(1);
    fail_density = true;
    bool rejected = false;
    try { view.set_compact(false); }
    catch (const std::runtime_error&) { rejected = true; }
    fail_density = false;
    expect(rejected && view.compact() && view.anchor().entry == compact_anchor.entry && view.anchor().offset == compact_anchor.offset &&
           view.left(1) == compact_left && saved["main"].first, "Density failure changed the visible or saved layout");
    view.open("other", 1180);
    expect(!view.compact(), "Density leaked to another rope");
    view.open("main", 1180);
    expect(view.compact() && view.anchor().entry == compact_anchor.entry && view.anchor().offset == compact_anchor.offset,
           "Reopening lost the rope's density or anchor");
    view.set_compact(false);
    expect(view.anchor().entry == compact_anchor.entry &&
           std::abs(view.anchor().offset - compact_anchor.offset * view.step() / 110) < .001,
           "Density changed the anchor card or its fractional offset");
    mixed = true;
    view.latest();
    // Entry "new" precedes landscape "0" and portrait "1".
    expect(view.geometry(1).body_width == 192 && view.geometry(2).body_width == 81 &&
           view.left(2) - view.left(1) == view.advance(1), "Mixed widths did not drive slot placement");
    const auto regular_image = view.geometry(1);
    view.set_compact(true);
    expect(view.geometry(1).body_width == 96 && view.geometry(1).width == regular_image.width / 2 &&
           view.geometry(1).height() == regular_image.height() / 2 && view.geometry(1).spacing == 12 &&
           view.left(2) - view.left(1) == view.advance(1), "Compact images or their slot spacing did not shrink together");
    view.set_compact(false);
    view.wheel(1);
    view.wheel(1);
    const auto anchor_x = view.left(2);
    expect(view.anchor().entry == "1", "Variable wheel missed the portrait");
    portrait_width = 3840;
    view.refresh(1180);
    expect(view.geometry(2).body_width == 192 && view.left(2) == anchor_x, "Late metadata displaced the anchor");
    const presentation::InsertionPreview mixed_gap{1,3,view.geometry(1).width,view.advance(1),view.geometry(1).inset()};
    expect(view.projected_left(1,mixed_gap) == view.left(2) - view.advance(1) &&
           view.projected_left(4,mixed_gap) == view.left(4), "Moving a wide card lost its slot width");
    std::cout << "1000 cards, 1180 DIP, maximum " << max_read
              << " realized; anchor and slot identity stable\n";
}
} // namespace
int main() {
    try {
        exercise();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
