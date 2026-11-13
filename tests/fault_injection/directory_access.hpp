#pragma once
#include "../integration/history_fixture.hpp"
#include <aclapi.h>

namespace history_fixture {
class DenyDirectoryAccess {
public:
    DenyDirectoryAccess(std::filesystem::path path, DWORD mask) : path_(std::move(path)) {
        require(GetNamedSecurityInfoW(const_cast<wchar_t *>(path_.c_str()), SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION, nullptr, nullptr, &original_, nullptr,
                                      &descriptor_) == ERROR_SUCCESS,
                "Could not read fixture permissions");
        BYTE world[SECURITY_MAX_SID_SIZE];
        DWORD n = sizeof(world);
        require(CreateWellKnownSid(WinWorldSid, nullptr, world, &n) != FALSE, "Could not create fixture SID");
        EXPLICIT_ACCESSW rule{};
        rule.grfAccessPermissions = mask;
        rule.grfAccessMode = DENY_ACCESS;
        BuildTrusteeWithSidW(&rule.Trustee, world);
        PACL denied{};
        require(SetEntriesInAclW(1, &rule, original_, &denied) == ERROR_SUCCESS,
                "Could not prepare fixture permissions");
        const auto status = SetNamedSecurityInfoW(const_cast<wchar_t *>(path_.c_str()), SE_FILE_OBJECT,
                                                  DACL_SECURITY_INFORMATION, nullptr, nullptr, denied, nullptr);
        LocalFree(denied);
        require(status == ERROR_SUCCESS, "Could not apply fixture directory restriction");
    }
    ~DenyDirectoryAccess() {
        SetNamedSecurityInfoW(const_cast<wchar_t *>(path_.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, original_, nullptr);
        LocalFree(descriptor_);
    }
    DenyDirectoryAccess(const DenyDirectoryAccess &) = delete;

private:
    std::filesystem::path path_;
    PACL original_{};
    PSECURITY_DESCRIPTOR descriptor_{};
};
} // namespace history_fixture
