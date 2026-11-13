#include "presentation/image_card_layout.hpp"
#include "presentation/card_content.hpp"
#include "presentation/hanging_layout.hpp"
#include "presentation/swing.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

using namespace cliprope;
using presentation::Hanging;
namespace {
void expect(bool pass, const char *problem) {
    if (!pass)
        throw std::runtime_error(problem);
}
bool close_to(double a, double b, double tolerance = 1e-3) { return std::abs(a - b) <= tolerance; }

void curve() {
    for (float width : {320.f, 800.f, 1536.f, 3840.f}) {
        const presentation::RopeCurve rope(width);
        const presentation::RopePlaque plaque(rope);
        expect(plaque.top >= 0 && close_to(plaque.left + plaque.width / 2, width / 2),
               "Plaque clipped above the row or lost its center");
        expect(close_to(plaque.top + plaque.height + plaque.gap, rope.lowest()), "Plaque lost the sag anchor");
        expect(plaque.contains(width / 2, plaque.top + 25) && !plaque.contains(plaque.left, plaque.top) &&
               !plaque.contains(width / 2, rope.lowest()), "Plaque silhouette hit test captures transparent pixels");
        for (float x : {1.f, plaque.width - 1}) {
            expect(plaque.contains(plaque.left + x, plaque.top + plaque.radius) &&
                   !plaque.contains(plaque.left + x, plaque.top + 0.5f) &&
                   !plaque.contains(plaque.left + x, plaque.top + plaque.height - 0.5f),
                   "Plaque corners and straight edges disagree with its rounded face");
        }
        const presentation::RopePlaque moved(rope, 123);
        expect(moved.contains(width / 2, plaque.top + 148) && close_to(moved.top - plaque.top, 123),
               "Plaque did not follow its dragged row");
    }
    // 1920 pixels at 125% is 1536 DIPs, where the sag reaches its cap.
    const presentation::RopeCurve wide(1536);
    expect(close_to(wide.y(0), Hanging::rope_end) && close_to(wide.y(1536), Hanging::rope_end),
           "Rope ends left the screen edges");
    expect(close_to(wide.sag(), Hanging::max_sag) && close_to(wide.y(768), wide.lowest()) &&
               close_to(wide.lowest(), Hanging::rope_end + Hanging::max_sag),
           "Rope sag is not centered at its cap");
    expect(wide.sag() >= 20 && wide.sag() <= 45, "Sag left the perceptible but restrained range");
    for (float x : {50.f, 300.f, 700.f})
        expect(close_to(wide.y(x), wide.y(1536 - x)), "Rope is not symmetric");
    for (float x = 0; x + 64 <= 768; x += 64)
        expect(wide.y(x + 64) > wide.y(x), "Rope does not descend toward the middle");
    for (float x = 32; x + 32 <= 1536; x += 128)
        expect(wide.y(x) >= (wide.y(x - 32) + wide.y(x + 32)) / 2 - 1e-4, "Rope is not a hanging curve");
    expect(close_to(wide.y(-50), Hanging::rope_end) && close_to(wide.y(2000), Hanging::rope_end),
           "Rope extends past its anchors");
    for (float x = 0; x <= 1536; x += 37) {
        const float r = wide.contact(x);
        expect(r == std::round(r) && std::abs(r - wide.y(x)) <= 0.5f, "Hook contact is not a whole DIP");
    }
    const presentation::RopeCurve narrow(800);
    expect(close_to(narrow.sag(), 800.0 / 64) && close_to(narrow.y(400), narrow.lowest()),
           "Narrow rope did not scale its sag");
    for (float width : {0.f, -5.f, std::numeric_limits<float>::quiet_NaN()}) {
        const presentation::RopeCurve flat(width);
        expect(flat.sag() == 0 && close_to(flat.y(10), Hanging::rope_end), "Degenerate rope is not straight");
    }
}

void frame() {
    for (float degrees : {0.f, 3.f, -4.5f}) {
        const presentation::CardFrame f{420, 52, degrees};
        const double radians = degrees * std::numbers::pi / 180;
        const std::pair<float, float> points[] = {{0.f, 0.f}, {-86.f, 42.f}, {73.f, 57.f}, {40.f, 190.f}};
        for (const auto &[cx, cy] : points) {
            // Direct2D's clockwise rotation about the pivot, then the hook translation.
            const double dx = cx, dy = cy - Hanging::pivot;
            const float sx = float(420 + dx * std::cos(radians) - dy * std::sin(radians));
            const float sy = float(52 + Hanging::pivot + dx * std::sin(radians) + dy * std::cos(radians));
            float ox{}, oy{};
            f.to_card(sx, sy, ox, oy);
            expect(close_to(ox, cx, 1e-3) && close_to(oy, cy, 1e-3), "Hit test does not invert the card rotation");
        }
    }
}

void swing() {
    presentation::Swing pinned;
    pinned.release("card", 4., 0.);
    pinned.pin("card");
    pinned.advance(.090);
    expect(std::abs(pinned.angle("card") - 2.) < .001, "Edit settling did not reach its midpoint");
    pinned.advance(.090);
    pinned.kick("card", 30.);
    expect(!pinned.moving() && pinned.angle("card") == 0, "Pinned card accepted a kick");
    pinned.unpin();
    expect(!pinned.moving(), "Ending an edit introduced motion");
    pinned.kick("card", 30.);
    expect(pinned.moving(), "Released card remained pinned");

    presentation::Swing s, twin;
    expect(!s.moving() && s.angle("absent") == 0, "Idle swing has motion");
    s.kick("a", 30);
    twin.kick("a", 30);
    expect(s.moving(), "Kick did not start motion");
    double peak = 0, lowest = 0, elapsed = 0;
    while (s.moving() && elapsed < 10) {
        s.advance(0.016);
        twin.advance(0.016);
        elapsed += 0.016;
        peak = std::max(peak, s.angle("a"));
        lowest = std::min(lowest, s.angle("a"));
        expect(s.angle("a") == twin.angle("a"), "Swing is not deterministic");
    }
    expect(!s.moving() && elapsed < 3 && s.angle("a") == 0, "Swing did not settle upright");
    expect(peak > 1.5 && peak <= Hanging::swing_limit && lowest < -0.5, "Return swing was imperceptible");
    s.kick("b", 1e6);
    for (int i = 0; i < 400 && s.moving(); ++i) {
        s.advance(0.016);
        expect(std::abs(s.angle("b")) <= Hanging::swing_limit, "Swing exceeded its angle limit");
    }
    s.kick("c", std::numeric_limits<double>::quiet_NaN());
    expect(!s.moving() || s.angle("c") == 0, "Invalid kick moved a card");
    s.kick("d", 10);
    s.advance(std::numeric_limits<double>::quiet_NaN());
    s.advance(-1);
    s.clear();
    expect(!s.moving() && s.angle("d") == 0, "Clear kept motion");
}

void pinned_grip() {
    using presentation::PointF;
    const PointF grip{-63, 168};
    double worst = 0;
    for (int i = 0; i < 240; ++i) {
        // Move rapidly between monitors above and to the left of the primary.
        const float scale = (i / 30 % 4 + 4) * 0.25f;
        const PointF cursor{float(-2100 + i * 21), float(-800 + i * 7)};
        const float angle = Hanging::swing_limit * std::sin(i / 15.f);
        const auto hook = presentation::dragged_hook(cursor, scale, grip, angle);
        // Desktop windows have integer pixel origins. Recover the grabbed point
        // from the presented window origin using the independent inverse frame.
        const float x = std::round(hook.x - 118 * scale), y = std::round(hook.y - 24 * scale);
        const presentation::CardFrame frame{118, 24, angle};
        float gx{}, gy{};
        frame.to_card((cursor.x - x) / scale, (cursor.y - y) / scale, gx, gy);
        worst = std::max(worst, double(std::hypot(gx - grip.x, gy - grip.y) * scale));
    }
    expect(worst < 0.71, "Rounded drag grip drifted from the pointer");
    std::cout << "Maximum grip error over 240 mixed-scale frames: " << worst << " px\n";
}

std::vector<unsigned char> drop(std::wstring_view names, bool wide = true) {
    std::vector<unsigned char> bytes(20);
    const std::uint32_t offset = 20, unicode = wide;
    std::memcpy(bytes.data(), &offset, 4);
    std::memcpy(bytes.data() + 16, &unicode, 4);
    for (const auto c : names) {
        bytes.push_back(static_cast<unsigned char>(c & 0xff));
        bytes.push_back(static_cast<unsigned char>(c >> 8));
    }
    return bytes;
}

void adaptive_images() {
    application::CardSummary card;
    card.kind = application::CardKind::image;
    for (auto mode : {domain::ImageSizeMode::compact, domain::ImageSizeMode::standard, domain::ImageSizeMode::large}) {
        card.image_preferences.size = mode;
        const auto limit = presentation::image_limit(mode);
        expect(presentation::image_row_height(mode) >= presentation::Hanging::row,
               "Mixed image row clips paper cards");
        for (auto source : {domain::ImageMetadata{1920,1080}, {1000,1000}, {1080,1920}, {8192,1}, {1,8192}, {2,3}}) {
            card.source = source;
            const auto g = presentation::ImageCardLayout::measure(card);
            const auto ratio = std::min({limit.width / source.width, limit.height / source.height, 1.5f});
            expect(std::abs(g.body_width - ratio * source.width) < 0.001f &&
                   std::abs(g.height() - ratio * source.height) < 0.001f, "Image aspect or enlargement changed");
            expect(g.width == std::max(160.f, g.body_width + 12) && g.bar_bottom - g.top == 2,
                   "Image escaped the centered hanger");
        }
    }
}
void content() {
    using namespace std::string_view_literals;
    const auto pair = drop(L"D:\\Archive\\report.pdf\0D:\\Archive\\notes.txt\0\0"sv);
    const auto files = presentation::drop_files(pair);
    expect(files && files->first == L"D:\\Archive\\report.pdf" && files->count == 2 && !files->truncated &&
               !files->folder,
           "File drop list was misread");
    const auto cut = presentation::drop_files(std::span(pair).first(pair.size() - 4));
    expect(cut && cut->count == 1 && cut->truncated, "Bounded prefix did not mark the list as truncated");
    const auto folder = presentation::drop_files(drop(L"D:\\Archive\\Atlas\0\0"sv));
    expect(folder && folder->count == 1 && folder->folder, "Folder drop was not recognized");
    expect(!presentation::drop_files(drop(L"D:\\a.txt\0\0"sv, false)) && !presentation::drop_files(drop(L"\0"sv)),
           "Unsupported drop list was accepted");

    std::vector<unsigned char> group(4 + 592);
    const std::uint32_t count = 3, flags = 0x4, attributes = 0x10;
    std::memcpy(group.data(), &count, 4);
    std::memcpy(group.data() + 4, &flags, 4);
    std::memcpy(group.data() + 4 + 36, &attributes, 4);
    const std::wstring_view name = L"Assets";
    for (std::size_t i = 0; i < name.size(); ++i)
        group[4 + 72 + 2 * i] = static_cast<unsigned char>(name[i]);
    const auto described = presentation::file_descriptors(group);
    expect(described && described->first == L"Assets" && described->count == 3 && described->folder,
           "Virtual file descriptor was misread");
    expect(!presentation::file_descriptors(std::span(group).first(100)), "Short descriptor was accepted");

    expect(presentation::is_link(L"https://example.com/a") && presentation::is_link(L"WWW.example.com") &&
               !presentation::is_link(L"see https://example.com") && !presentation::is_link(L"http://"),
           "Link detection failed");
    expect(presentation::link_host(L"https://example.com/a?b") == L"example.com" &&
               presentation::link_host(L"example.org") == L"example.org",
           "Link host was misread");
    expect(presentation::describe_file(L"report.PDF", false) == L"PDF document" &&
               presentation::describe_file(L"model.xyz", false) == L"XYZ file" &&
               presentation::describe_file(L"Atlas", true) == L"Folder" &&
               presentation::describe_file(L"README", false) == L"File",
           "File kind label failed");
    expect(presentation::parent(L"D:\\a.txt") == L"D:\\" && presentation::parent(L"D:\\x\\a.txt") == L"D:\\x" &&
               presentation::leaf(L"D:\\x\\") == L"x",
           "Path split failed");
    const auto elided = presentation::elide_path(L"D:\\Archive\\Projects\\Atlas\\2026\\Q3", 24);
    expect(elided.size() <= 24 && elided.starts_with(L"D:\\…\\") && elided.ends_with(L"\\Q3"),
           "Location summary lost its root or leaf");
    expect(presentation::elide_path(L"D:\\Archive", 24) == L"D:\\Archive", "Short location was shortened");
}
} // namespace

int main() {
    try {
        curve();
        frame();
        pinned_grip();
        swing();
        adaptive_images();
        cliprope::application::CardSummary sized;
        for (const float scale : {.75f, 1.f, 2.f}) {
            sized.scale = {scale, scale};
            for (const bool compact : {false, true}) {
                const auto g = cliprope::presentation::ImageCardLayout::measure(sized, compact ? 86.f : 172.f, compact);
                const auto toolbar = g.floating_size(sized.scale);
                expect(g.width == (compact ? 86.f : 172.f) * scale && g.height() == 148 * scale &&
                       g.hanger_width() - g.width == 8 && g.bar_bottom == cliprope::presentation::Hanging::bar_bottom &&
                       toolbar.width == 240 * scale && toolbar.height == 160 * scale, "Paper scale or hardware changed");
            }
        }
        sized.kind = cliprope::application::CardKind::image;
        for (const auto source : {cliprope::domain::ImageMetadata{1080, 1920}, cliprope::domain::ImageMetadata{1920,1080}}) {
            sized.source = source;
            const auto g = cliprope::presentation::ImageCardLayout::measure(sized);
            expect(std::abs(g.body_width / g.height() - float(source.width)/source.height) < .001f &&
                   g.hanger_width() >= 160 && g.float_width() >= 144, "Image resize distorted its source or tools");
        }
        content();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "Catenary, rotated hit frame, bounded swing and card content parsers verified\n";
    return 0;
}
