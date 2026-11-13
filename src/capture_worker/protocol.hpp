#pragma once

#include "capture_worker/capture.hpp"
#include <cstdint>

namespace cliprope::worker {

// The section is anonymous. Only the writer's committed response consumes pages.
std::size_t section_size();
void write_response(HANDLE section, std::uint64_t serial, DWORD sequence, const CaptureResult& result);
CaptureResult read_response(HANDLE section, std::uint64_t serial, DWORD sequence);

} // namespace cliprope::worker
