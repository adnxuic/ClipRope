#include "platform_win/uninstall_dialog.hpp"
#include "platform_win/fluent.hpp"
#include "platform_win/localization.hpp"
#include "persistence/database.hpp"
#include "preview_worker/protocol.hpp"
#include <shlobj.h>
#include <shellapi.h>
#include <userenv.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <system_error>

namespace {
namespace fs = std::filesystem;
using namespace cliprope;
struct Close { void operator()(void* value) const { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
using Handle = std::unique_ptr<void, Close>;
void require(bool result, const char* reason) {
    if (!result) throw std::runtime_error(reason);
}
void windows(bool result) {
    if (!result) throw std::system_error(int(GetLastError()), std::system_category());
}
std::wstring environment(const wchar_t* name) {
    const DWORD count = GetEnvironmentVariableW(name, nullptr, 0);
    if (!count) return {};
    require(count <= 32768, "Uninstaller environment value is too long");
    std::wstring value(count, L'\0');
    const auto length = GetEnvironmentVariableW(name, value.data(), count);
    require(length && length < count, "Uninstaller environment changed");
    value.resize(length); return value;
}
fs::path own_executable() {
    std::array<wchar_t,32768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    require(length && length < path.size(), "Uninstaller image path is unavailable");
    return fs::path(std::wstring(path.data(),length));
}
std::wstring quote(std::wstring_view value) {
    std::wstring out = L"\"";
    size_t slashes{};
    for (auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? slashes*2+1 : slashes, L'\\');
        out += c; slashes=0;
    }
    out.append(slashes*2,L'\\'); return out + L"\"";
}
void ordinary_directory(const fs::path& directory) {
    require(directory.is_absolute() && directory == directory.lexically_normal(), "Uninstaller directory is not absolute");
    for (auto path=directory;;path=path.parent_path()) {
        const auto attributes = GetFileAttributesW(path.c_str());
        require(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "Uninstaller refuses linked directories");
        if (path == path.root_path()) break;
    }
}
struct Context {
    fs::path local, temporary;
    HKEY user=HKEY_CURRENT_USER;
    std::shared_ptr<std::remove_pointer_t<HKEY>> isolated_user;
    bool test{};
};
Context context() {
    Context result;
    const auto test_id = environment(L"CLIPROPE_UNINSTALL_TEST_ID");
    const auto test_local = environment(L"CLIPROPE_UNINSTALL_TEST_LOCAL");
    if (!test_id.empty() || !test_local.empty()) {
#ifdef CLIPROPE_DESKTOP_PROBES
        require(test_id.size()==32 && test_id.find_first_not_of(L"0123456789abcdef")==std::wstring::npos && !test_local.empty(),
                "Invalid isolated uninstaller namespace");
        result.local=fs::path(test_local);
        ordinary_directory(result.local);
        std::ifstream marker(result.local.parent_path()/L"uninstall-fixture.id");
        std::string identity; std::getline(marker,identity);
        require(std::wstring(identity.begin(),identity.end())==test_id, "Uninstaller fixture marker does not match");
        HKEY key{};
        const auto path=L"Software\\ClipRope.TestUninstall."+test_id;
        require(RegOpenKeyExW(HKEY_CURRENT_USER,path.c_str(),0,KEY_ALL_ACCESS | KEY_WOW64_64KEY,&key)==ERROR_SUCCESS,
                "Isolated uninstaller registry is unavailable");
        result.isolated_user.reset(key,RegCloseKey); result.user=key; result.test=true;
        result.temporary=result.local.parent_path()/L"temp";
        fs::create_directory(result.temporary); ordinary_directory(result.temporary);
        return result;
#else
        throw std::runtime_error("Retail uninstaller does not accept fixture overrides");
#endif
    }
    PWSTR local{};
    require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local)), "Local app data is unavailable");
    std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> free(local,CoTaskMemFree);
    result.local=local;
    std::array<wchar_t,32768> temp{};
    const auto length=GetTempPathW(DWORD(temp.size()),temp.data());
    require(length && length<temp.size(), "Temporary directory is unavailable");
    result.temporary=fs::path(temp.data()).lexically_normal();
    return result;
}
fs::path worker_copy(const fs::path& executable,const Context& context) {
    auto directory=(context.temporary/L"ClipRope.Uninstall").lexically_normal();
    ordinary_directory(context.temporary);
    constexpr auto identity="ClipRope.Uninstall.Helper.v1";
    if (CreateDirectoryW(directory.c_str(),nullptr)) {
        Handle marker(CreateFileW((directory/L"helper.id").c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        windows(marker.get()!=INVALID_HANDLE_VALUE);
        DWORD written{};
        windows(WriteFile(marker.get(),identity,DWORD(std::char_traits<char>::length(identity)),&written,nullptr)!=FALSE);
        require(written==std::char_traits<char>::length(identity), "Helper marker write was incomplete");
    } else windows(GetLastError()==ERROR_ALREADY_EXISTS);
    ordinary_directory(directory);
    const auto marker_path=directory/L"helper.id";
    const auto marker_attributes=GetFileAttributesW(marker_path.c_str());
    require(marker_attributes!=INVALID_FILE_ATTRIBUTES && !(marker_attributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)),
            "Helper marker is not an ordinary file");
    std::ifstream input(marker_path); std::string saved; std::getline(input,saved);
    require(saved==identity, "Refusing an unrecognized helper directory");
    const auto target=directory/L"Uninstall.exe";
    const auto attributes=GetFileAttributesW(target.c_str());
    require(attributes==INVALID_FILE_ATTRIBUTES || !(attributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_READONLY)),
            "Helper image is not an ordinary file");
    windows(CopyFileW(executable.c_str(),target.c_str(),FALSE)!=FALSE);
    require(persistence::file_sha256(executable)==persistence::file_sha256(target), "Helper image verification failed");
    return target;
}
void bootstrap(const fs::path& executable,const Context& context) {
    const auto directory=executable.parent_path();
    ordinary_directory(directory);
    wchar_t layout[64]{};
    GetPrivateProfileStringW(L"ClipRope",L"Layout",L"",layout,DWORD(std::size(layout)),(directory/L"ClipRope.build.ini").c_str());
    require(std::wstring_view(layout)==L"direct-build-v1", "Not a ClipRope direct-build layout");
    const auto helper=worker_copy(executable,context);
    auto command=quote(helper.native())+L" --worker "+quote(directory.native())+L" --parent "+std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    windows(CreateProcessW(helper.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,helper.parent_path().c_str(),&startup,&process)!=FALSE);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
}
void await_parent(const fs::path& directory,const wchar_t* text) {
    wchar_t* end{};
    const auto pid=wcstoul(text,&end,10);
    require(pid && end && !*end && pid!=GetCurrentProcessId(), "Invalid uninstaller parent");
    Handle parent(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
    if (!parent) { require(GetLastError()==ERROR_INVALID_PARAMETER,"Uninstaller parent is inaccessible"); return; }
    if (WaitForSingleObject(parent.get(),0)==WAIT_OBJECT_0) return;
    std::array<wchar_t,32768> image{}; DWORD count=DWORD(image.size());
    if (!QueryFullProcessImageNameW(parent.get(),0,image.data(),&count)) {
        if (WaitForSingleObject(parent.get(),0)==WAIT_OBJECT_0) return;
        windows(false);
    }
    require(CompareStringOrdinal(image.data(),-1,(directory/L"Uninstall.exe").c_str(),-1,TRUE)==CSTR_EQUAL,
            "Uninstaller parent is not the target copy");
    require(WaitForSingleObject(parent.get(),5000)==WAIT_OBJECT_0,"Uninstaller parent did not exit");
}
} // namespace

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    using namespace cliprope;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(OleInitialize(nullptr))) return 1;
    localization::LocalizationManager manager;
    manager.set_language(localization::Language::automatic,win::preferred_ui_locale());
    win::UiLocalization binding(manager);
    int result=1;
    try {
        int argc{}; const auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
        require(argv!=nullptr,"Uninstaller arguments are unavailable");
        std::unique_ptr<void,decltype(&LocalFree)> arguments(argv,LocalFree);
        const auto executable=own_executable();
        const auto settings=context();
        if (argc==1) { bootstrap(executable,settings); result=0; }
        else {
            require(argc==5 && std::wstring_view(argv[1])==L"--worker" && std::wstring_view(argv[3])==L"--parent",
                    "Unsupported uninstaller arguments");
            const fs::path directory(argv[2]);
            require(executable.parent_path()==(settings.temporary/L"ClipRope.Uninstall").lexically_normal(),
                    "Uninstaller worker must run from its owned helper directory");
            await_parent(directory,argv[4]);
            win::BuildUninstallOptions options{directory,settings.local,settings.user};
            if (settings.test) options.processes=[](const auto& root) {
                auto state=win::active_build_copies(root); state.other=false; return state;
            };
            if (!settings.test) options.remove_preview=[] {
                const auto status=DeleteAppContainerProfile(preview_worker::container_name);
                if (FAILED(status)) throw std::runtime_error("Preview container cleanup failed");
            };
            win::BuildUninstall cleanup(std::move(options));
            result=win::uninstall_dialog(nullptr,{[&] { return cleanup.preview(); },[&](bool erase) { return cleanup.remove(erase); }}) ? 0 : 2;
        }
    } catch (const std::exception&) {
        OutputDebugStringA("ClipRope direct-build uninstaller could not start.\n");
        win::fluent::management_prompt(nullptr,win::tr(win::TextKey::uninstall_title),L"ClipRope",
                                       win::tr(win::TextKey::uninstall_bootstrap_failed),win::tr(win::TextKey::common_close),false,280);
    }
    OleUninitialize(); return result;
}
