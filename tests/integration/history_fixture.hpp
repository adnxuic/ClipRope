#pragma once
#include <windows.h>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace history_fixture {
inline void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
template<class F> void rejects(F action, const char* message) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto parent = std::filesystem::temp_directory_path();
        if (parent.filename().empty()) parent = parent.parent_path();
        path = parent / ("cliprope-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        require(path.parent_path() == parent && std::filesystem::create_directory(path), "Could not create an exclusive test directory");
    }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;
};
}
