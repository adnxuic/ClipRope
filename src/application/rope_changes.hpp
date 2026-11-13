#pragma once
#include "domain/history.hpp"
#include <stdexcept>

namespace cliprope::application {
inline constexpr int rope_name_units = 256;
struct InvalidRopeName : std::invalid_argument {
    InvalidRopeName() : std::invalid_argument("A rope name needs nonblank Unicode text of up to 256 UTF-16 units without NUL") {}
};
enum class RopeOp { create, properties, defaults, clear, clear_all, erase, delete_all };
struct RopeChange {
    RopeOp op{};
    domain::Id rope, target; // An erase with a target transfers the batch first.
    std::string name;
    domain::ColorSelection color;
    std::optional<int> capacity;
};
struct RopePreview {
    RopeChange change;
    std::size_t hanging{}, floating{}, moved{}, overflow{};
    bool shrink{};
    std::string stamp;
};
} // namespace cliprope::application
