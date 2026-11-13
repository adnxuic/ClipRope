#pragma once
#include <windows.h>
#include <string>

namespace cliprope::persistence {
// Consume a freshly opened, read-locked handle from offset zero to EOF.
std::string file_sha256(HANDLE file);
}
