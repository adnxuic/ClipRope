#pragma once
#include "domain/color.hpp"
#include <functional>

namespace cliprope::presentation {
enum class ColorCommit { saved, staged };
struct ColorPickerCallbacks {
    std::function<void(domain::ColorSelection)> preview;
    std::function<ColorCommit(domain::ColorSelection)> commit;
    std::function<void()> cancel;
    std::function<bool()> valid;
};
class ColorPickerController {
  public:
    ColorPickerController(domain::ColorSelection original, unsigned fallback, ColorPickerCallbacks callbacks)
        : original_(original), draft_(original), fallback_(fallback), callbacks_(std::move(callbacks)),
          hsv_(domain::ColorHSV::from(original.resolve(fallback))) {}
    ~ColorPickerController() { cancel(); }
    const domain::ColorSelection &original() const { return original_; }
    const domain::ColorSelection &draft() const { return draft_; }
    const std::optional<domain::ColorSelection> &committed() const { return committed_; }
    domain::ColorRGBA8 color() const { return draft_.resolve(fallback_); }
    domain::ColorHSV hsv() const { return hsv_; }
    bool valid() const { return !callbacks_.valid || callbacks_.valid(); }
    void preview(domain::ColorSelection value) {
        if (ended_ || value == draft_)
            return;
        draft_ = value;
        hsv_ = domain::ColorHSV::from(color(), hsv_.h);
        if (callbacks_.preview)
            callbacks_.preview(draft_);
    }
    void hsv(domain::ColorHSV value) {
        const auto rgba = value.rgba(color().a);
        preview(rgba);
        hsv_ = value;
    }
    ColorCommit commit() {
        if (ended_)
            throw std::logic_error("The color edit has already ended");
        if (!valid())
            throw std::runtime_error("Color target changed");
        auto result = callbacks_.commit ? callbacks_.commit(draft_) : ColorCommit::staged;
        if (result == ColorCommit::saved)
            committed_ = draft_;
        ended_ = true;
        return result;
    }
    void cancel() {
        if (ended_)
            return;
        ended_ = true;
        if (callbacks_.cancel)
            callbacks_.cancel();
    }

  private:
    domain::ColorSelection original_, draft_;
    std::optional<domain::ColorSelection> committed_;
    unsigned fallback_;
    ColorPickerCallbacks callbacks_;
    domain::ColorHSV hsv_;
    bool ended_{};
};
} // namespace cliprope::presentation
