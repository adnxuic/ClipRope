#include "platform_win/login_startup.hpp"
#include <optional>
#include <stdexcept>

namespace cliprope::win {
namespace {
constexpr auto run = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr auto startup = L"Software\\ClipRope\\Startup";
constexpr auto product = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ClipRope";

void check(LSTATUS result) {
    if (result != ERROR_SUCCESS)
        throw std::runtime_error("Could not access the sign-in registration (Windows error " +
                                 std::to_string(result) + ").");
}
std::optional<std::wstring> get(HKEY user, const wchar_t* key, const wchar_t* name) {
    DWORD bytes{};
    constexpr auto flags = RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY;
    auto result = RegGetValueW(user, key, name, flags, nullptr, nullptr, &bytes);
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) return {};
    check(result);
    if (!bytes || bytes > 65536 || bytes % sizeof(wchar_t))
        throw std::runtime_error("Invalid sign-in registration.");
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    check(RegGetValueW(user, key, name, flags, nullptr, value.data(), &bytes));
    value.resize(bytes / sizeof(wchar_t) - 1);
    return value;
}
void put(HKEY user, const wchar_t* path, const wchar_t* name, const std::wstring& value) {
    HKEY key{};
    check(RegCreateKeyExW(user, path, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr));
    auto result = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                 static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    check(result);
}
bool same(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.data(), int(a.size()), b.data(), int(b.size()), TRUE) == CSTR_EQUAL;
}
void matching_value(HKEY user, const wchar_t* path, const wchar_t* name,
                    const std::wstring& expected, bool remove) {
    const auto value = get(user, path, name);
    if (!value || !same(*value, expected)) return;
    HKEY key{};
    check(RegOpenKeyExW(user, path, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &key));
    DWORD bytes{}, type{};
    auto result = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (result == ERROR_SUCCESS && (type != REG_SZ || !bytes || bytes > 65536 || bytes % sizeof(wchar_t)))
        result = ERROR_INVALID_DATA;
    std::wstring current(result == ERROR_SUCCESS ? bytes / sizeof(wchar_t) + 1 : 1, L'\0');
    if (result == ERROR_SUCCESS)
        result = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(current.data()), &bytes);
    if (result == ERROR_SUCCESS && type == REG_SZ && bytes >= sizeof(wchar_t)) {
        current.resize(bytes / sizeof(wchar_t) - 1);
        if (!same(current, expected)) result = ERROR_RETRY;
        else if (remove) result = RegDeleteValueW(key, name);
    } else if (result == ERROR_SUCCESS) result = ERROR_INVALID_DATA;
    RegCloseKey(key);
    check(result);
}
}
std::optional<std::wstring> StartupRegistration::conflict_for(const std::filesystem::path& executable) const {
    if (owner && !same(*owner, executable.native())) return owner;
    if (installation) {
        const auto installed = (std::filesystem::path(*installation) / L"current" / L"ClipRope.exe").native();
        if (!same(installed, executable.native())) return installed;
    }
    if (command && !same(*command, L"\"" + executable.native() + L"\"")) return command;
    return {};
}
StartupRegistration LoginStartup::registration() const {
    return {get(user_, startup, L"Owner"), get(user_, product, L"InstallLocation"), get(user_, run, L"ClipRope")};
}
bool LoginStartup::enabled() const {
    const auto saved = registration();
    if (auto conflict = saved.conflict_for(executable_)) throw StartupConflict(*conflict);
    return saved.command.has_value();
}
void LoginStartup::check_removal_access() const {
    matching_value(user_, run, L"ClipRope", L"\"" + executable_.native() + L"\"", false);
    matching_value(user_, startup, L"Owner", executable_.native(), false);
}
void LoginStartup::remove_owned() const {
    matching_value(user_, run, L"ClipRope", L"\"" + executable_.native() + L"\"", true);
    matching_value(user_, startup, L"Owner", executable_.native(), true);
}
void LoginStartup::initialize() const {
    const bool on = enabled();
    if (get(user_, startup, L"Owner")) return;
    if (!on && !get(user_, product, L"InstallLocation")) set(true);
    put(user_, startup, L"Owner", executable_.native());
}
void LoginStartup::set(bool on) const {
    if (enabled() == on) return;
    if (on) {
        const auto command = L"\"" + executable_.native() + L"\"";
        if (command.size() > 260) throw std::runtime_error("The executable path is too long for Windows sign-in.");
        put(user_, run, L"ClipRope", command);
    } else {
        HKEY key{};
        check(RegOpenKeyExW(user_, run, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key));
        const auto result = RegDeleteValueW(key, L"ClipRope");
        RegCloseKey(key);
        if (result != ERROR_FILE_NOT_FOUND) check(result);
    }
    if (enabled() != on) throw std::runtime_error("The sign-in registration changed. Try again.");
}
}
