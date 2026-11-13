#pragma once
#include "capture_worker/capture.hpp"
#include <functional>

namespace cliprope {
void capture_virtual_streams(IDataObject& source, CaptureResult& result, const std::function<bool()>& superseded);
}
