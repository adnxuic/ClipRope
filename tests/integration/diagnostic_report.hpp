#pragma once
#include "history_fixture.hpp"
#include <fstream>
#include <string>

namespace history_fixture {
inline std::string diagnostic_report(const std::filesystem::path& profile) {
    std::ifstream in(profile / "diagnostics.txt", std::ios::binary);
    require(in.good(), "Requested session diagnostics were not exported");
    const std::string report((std::istreambuf_iterator<char>(in)), {});
    require(report.starts_with("ClipRope diagnostics v2\r\n") && report.size() < 4096 &&
            report.find(profile.string()) == report.npos && report.find("AR3 isolated target") == report.npos &&
            report.find("PRIVATE-SEED") == report.npos && report.find("Synthetic\\Private") == report.npos,
            "Diagnostic export contains unsupported or private data");
    return report;
}
}
