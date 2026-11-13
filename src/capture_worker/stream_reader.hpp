#pragma once
#include "platform_win/clipboard.hpp"
#include <cstdint>
#include <functional>
#include <optional>

namespace cliprope {
std::string read_bounded_stream(IDataObject& source, Representation& item, std::size_t limit,
                                std::optional<std::uint64_t> declared_size, HRESULT& error,
                                const std::function<bool()>& superseded);
}
