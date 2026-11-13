#include "presentation/color_picker.hpp"
#include <iostream>

using namespace cliprope;
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main() {
    try {
        using namespace domain;
        require(!parse_color("#12") && !parse_color("#GG0000") && !parse_color("#11223344", false),
                "Incomplete/invalid input accepted");
        require(parse_color("#11223344") == ColorRGBA8{17, 34, 51, 68}, "RGBA byte order");
        require(ColorSelection{}.encode() == "default" && ColorSelection(0).encode() == "#000000FF",
                "Default aliases black");
        require(ColorSelection(0xffffff).resolve(paper_default).rgb() == 0xffffff, "White aliases paper");
        for (unsigned r = 0; r < 256; r += 17)
            for (unsigned g = 0; g < 256; g += 17)
                for (unsigned b = 0; b < 256; b += 17) {
                    ColorRGBA8 c{std::uint8_t(r), std::uint8_t(g), std::uint8_t(b), 77};
                    require(ColorHSV::from(c).rgba(77) == c, "HSV round trip");
                }
        require(ColorHSV::from(ColorRGBA8::rgb(0), 240).h == 240, "Black lost hue");
        require(composite(ColorRGBA8::rgb(0xff0000, 0), paper_default) == paper_default,
                "Zero tint changes paper");
        require(contrast(0, 0xffffff) == 21, "Contrast endpoints");
        int previews = 0, writes = 0, cancels = 0;
        presentation::ColorPickerCallbacks cb;
        cb.preview = [&](auto) { ++previews; };
        cb.cancel = [&] { ++cancels; };
        cb.commit = [&](auto) {
            ++writes;
            return presentation::ColorCommit::saved;
        };
        {
            presentation::ColorPickerController c({}, paper_default, cb);
            for (int i = 0; i < 100; ++i)
                c.preview(unsigned(i));
            require(writes == 0, "Preview wrote");
            c.cancel();
        }
        require(previews == 100 && cancels == 1, "Cancel not exactly once");
        {
            presentation::ColorPickerController c({}, paper_default, cb);
            c.preview(0xffffff);
            c.commit();
            require(c.committed()==ColorSelection(0xffffff),"Committed color missing");
            try { c.commit(); } catch (const std::logic_error&) {}
            require(writes==1,"A completed edit committed twice");
        }
        require(writes == 1 && cancels == 1, "Commit reverted");
        cb.commit = [](auto) -> presentation::ColorCommit { throw std::runtime_error("Injected failure"); };
        {
            presentation::ColorPickerController c({}, paper_default, cb);
            c.preview(0x123456);
            try {
                c.commit();
            } catch (...) {
            }
            require(c.draft() == ColorSelection(0x123456), "Failure lost draft");
        }
        require(cancels == 2, "Failed draft not canceled");
        std::cout << "Color model and transactional controller passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
