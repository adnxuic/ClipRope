#include "../clipboard_sources/media_sample.hpp"
#include "platform_win/media.hpp"
#include "rendering/image_preview.hpp"
#include <iostream>
#include <stdexcept>
using namespace cliprope;
int main() {
    if (FAILED(OleInitialize(nullptr)))
        return 1;
    int rc = 0;
    try {
        auto require = [](bool condition) {
            if (!condition)
                throw std::runtime_error("Media boundary validation failed");
        };
        for (auto f : media_sample::rich()) {
            if (!media::supports(f.format))
                continue;
            require(media::validate(f.format, f.bytes));
            if (f.format != RegisterClipboardFormatW(L"Rich Text Format")) {
                const auto image = rendering::decode_preview(f.format, f.bytes);
                require(image.width == 2 && image.height == 2 &&
                        image.pbgra == std::vector<unsigned char>{0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0,
                                                                  255, 255, 255, 255, 255});
            }
        }
        auto png = media_sample::png();
        png.bytes.resize(40);
        require(!media::validate(png.format, png.bytes));
        png = media_sample::png();
        png.bytes[16] = 127;
        require(!media::validate(png.format, png.bytes));
        auto dib = media_sample::dib(true);
        dib.bytes[112] = 1;
        require(!media::validate(dib.format, dib.bytes));
        dib = media_sample::dib(false);
        dib.bytes.pop_back();
        require(!media::validate(dib.format, dib.bytes));
        const auto rtf = RegisterClipboardFormatW(L"Rich Text Format");
        for (const auto text :
             {"{\\rtf1 unclosed", "{\\rtf1 \\bin99 }", "{\\rtf1 \\'xq}", "{\\rtf1 ok}junk"}) {
            std::vector<unsigned char> bytes(text, text + strlen(text));
            require(!media::validate(rtf, bytes));
        }
        const std::string valid = "{\\rtf1 escaped \\{ \\} \\bin2 {} done}";
        std::vector<unsigned char> bytes(valid.begin(), valid.end());
        require(media::validate(rtf, bytes));
        require(bytes.back() == 0 && bytes.size() == valid.size() + 1);
        std::cout << "RTF groups, binary and escapes; valid PNG/BGR samples; bounded DIB/DIBV5 dimensions, "
                     "lengths and profiles passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        rc = 1;
    }
    OleUninitialize();
    return rc;
}
