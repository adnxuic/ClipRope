#include "history_fixture.hpp"
#include "platform_win/build_uninstall.hpp"
#include "platform_win/login_startup.hpp"
#include "persistence/history_store.hpp"
#include "persistence/profile_location.hpp"
#include <aclapi.h>
#include <ole2.h>
#include <winioctl.h>
#include <array>
#include <fstream>
#include <iostream>

namespace fs=std::filesystem;
using namespace cliprope;
using namespace cliprope::win;
using history_fixture::require;
namespace {
constexpr std::array runtime{L"ClipRope.exe",L"ClipRopeWorker.exe",L"ClipRopePreviewWorker.exe", L"ClipRopeTargetWorker.exe",
                             L"cliprope_headless.exe",L"cliprope_profile_support.exe",L"Uninstall.exe"};
struct Fixture {
    history_fixture::Directory temporary;
    fs::path root=temporary.path/L"copy-\u526f\u672c",local=temporary.path/L"local";
    HKEY user{};
    std::wstring key_name,id;
    unsigned preview_calls{};
    Fixture() {
        GUID guid{}; require(SUCCEEDED(CoCreateGuid(&guid)),"Fixture GUID failed");
        wchar_t text[40]{}; StringFromGUID2(guid,text,40);
        for (auto c:std::wstring_view(text)) if (iswxdigit(c)) id+=wchar_t(towlower(c));
        key_name=L"Software\\ClipRope.TestUninstall."+id;
        require(RegCreateKeyExW(HKEY_CURRENT_USER,key_name.c_str(),0,nullptr,0,KEY_ALL_ACCESS | KEY_WOW64_64KEY,
                                nullptr,&user,nullptr)==ERROR_SUCCESS,"Fixture registry failed");
        fs::create_directory(root); fs::create_directory(local);
        std::string ascii_id; for (auto c:id) ascii_id+=static_cast<char>(c);
        std::ofstream(temporary.path/L"uninstall-fixture.id")<<ascii_id;
        for (auto file:runtime) std::ofstream(root/file)<<"Disposable runtime file";
        std::ofstream(root/L"ClipRope.build.ini")<<"[ClipRope]\nLayout=direct-build-v1\n";
        std::ofstream(root/L"developer.lib")<<"Unrelated build library";
        std::ofstream(root/L"notes.txt")<<"Unrecognized file";
    }
    ~Fixture() {
        for (auto file:runtime) SetFileAttributesW((root/file).c_str(),FILE_ATTRIBUTE_NORMAL);
        if (user) RegCloseKey(user);
        RegDeleteTreeW(HKEY_CURRENT_USER,key_name.c_str());
    }
    BuildUninstall cleanup() {
        BuildUninstallOptions options{root,local,user};
        options.processes=[](const auto&) { return ActiveBuildCopies{}; };
        options.remove_preview=[this] { ++preview_calls; };
        return BuildUninstall(std::move(options));
    }
    void startup(bool on=true) {
        LoginStartup registration(root/L"ClipRope.exe",user);
        registration.initialize(); registration.set(on);
    }
    void seed() {
        persistence::ProfileLocation location(local);
        persistence::HistoryStore history(local/L"ClipRope");
        domain::Format text; text.standard_id=CF_UNICODETEXT; text.bytes={'S',0,0,0};
        const auto first=history.admit("main",{{text}});
        const auto floating=history.copy(first,"main");
        history.detach(floating,{"fixture",40,50,200,160,true});
        history.database().backup_to(local/L"ClipRope"/L"backups"/L"initial");
    }
    void intact() const {
        for (auto file:runtime) require(fs::is_regular_file(root/file),"Preflight failure removed a runtime file");
        require(fs::exists(root/L"ClipRope.build.ini"),"Preflight failure removed the layout marker");
    }
    void removed() const {
        for (auto file:runtime) require(!fs::exists(root/file),"Runtime file remained");
        require(!fs::exists(root/L"ClipRope.build.ini"),"Layout marker remained");
        require(fs::exists(root/L"developer.lib") && fs::exists(root/L"notes.txt"),"Cleanup broadened beyond runtime files");
    }
};
void value(HKEY user,const wchar_t* path,const wchar_t* name,const std::wstring& text) {
    HKEY key{};
    require(RegCreateKeyExW(user,path,0,nullptr,0,KEY_ALL_ACCESS | KEY_WOW64_64KEY,nullptr,&key,nullptr)==ERROR_SUCCESS,"Registry fixture key failed");
    const auto result=RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(text.c_str()),DWORD((text.size()+1)*sizeof(wchar_t)));
    RegCloseKey(key); require(result==ERROR_SUCCESS,"Registry fixture value failed");
}
void keep() {
    for (bool on:{false,true}) {
        Fixture f; f.startup(on); f.seed();
        const auto before=persistence::file_sha256(f.local/L"ClipRope"/L"backups"/L"initial"/L"history.db");
        const auto result=f.cleanup().remove(false);
        require(result.success && !result.history_touched,"Keep-history uninstall failed"); f.removed();
        const auto registration=LoginStartup(f.root/L"ClipRope.exe",f.user).registration();
        require(!registration.owner && !registration.command,"Owned startup values remained");
        persistence::HistoryStore reopened(f.local/L"ClipRope");
        require(reopened.hanging("main").size()==1 && reopened.floating().size()==1,"Kept history changed entry state");
        require(before==persistence::file_sha256(f.local/L"ClipRope"/L"backups"/L"initial"/L"history.db"),"Backup changed");
        require(f.preview_calls==1,"Last-copy preview cleanup was not called once");
    }
    std::cout<<"Enabled and disabled startup markers removed; Hanging/Floating history and backups retained; unknown files retained\n";
}
void purge() {
    Fixture f; f.startup(); f.seed();
    const auto result=f.cleanup().remove(true);
    require(result.success && result.history_touched,"Explicit history purge failed"); f.removed();
    require(!fs::exists(f.local/L"ClipRope") && !fs::exists(f.local/L"ClipRope.Settings"),"Explicit purge retained the profile/locator");
    std::cout<<"Explicit purge removed recognized history, Floating cards, backups and locator\n";
}
void foreign() {
    Fixture f; f.seed();
    const auto other=f.temporary.path/L"other"/L"ClipRope.exe";
    value(f.user,L"Software\\ClipRope\\Startup",L"Owner",other.native());
    value(f.user,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",L"ClipRope",L"\""+other.native()+L"\"");
    auto cleanup=f.cleanup();
    require(!cleanup.preview().delete_history_available,"Foreign history deletion was offered");
    require(cleanup.remove(true).issue==UninstallIssue::shared_history,"Foreign history purge was not rejected"); f.intact();
    require(cleanup.remove(false).success,"Current copy could not be removed with foreign registration retained"); f.removed();
    const auto saved=LoginStartup(f.root/L"ClipRope.exe",f.user).registration();
    require(saved.owner==other.native() && saved.command==L"\""+other.native()+L"\"","Foreign registration changed");
    require(fs::exists(f.local/L"ClipRope"/L"history.db") && !f.preview_calls,"Shared history or preview resources changed");
    bool specific{};
    try { (void)LoginStartup(f.root/L"ClipRope.exe",f.user).enabled(); }
    catch (const StartupConflict& error) { specific=error.other_executable()==other.native(); }
    require(specific,"Ownership conflict was not reported distinctly");
    std::cout<<"Foreign registration/data retained; current payload removed; typed conflict includes the other executable\n";
}
class DenyRegistryWrite {
public:
    explicit DenyRegistryWrite(HKEY user) {
        require(RegOpenKeyExW(user,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,
                              KEY_READ | WRITE_DAC | KEY_WOW64_64KEY,&key_)==ERROR_SUCCESS,"Fixture ACL key unavailable");
        DWORD bytes{};
        RegGetKeySecurity(key_,DACL_SECURITY_INFORMATION,nullptr,&bytes);
        original_.resize(bytes);
        require(bytes && RegGetKeySecurity(key_,DACL_SECURITY_INFORMATION,original_.data(),&bytes)==ERROR_SUCCESS,
                "Fixture ACL read failed");
        PACL previous{}; BOOL present{},defaulted{};
        require(GetSecurityDescriptorDacl(original_.data(),&present,&previous,&defaulted),"Fixture ACL unavailable");
        BYTE world[SECURITY_MAX_SID_SIZE]; DWORD size=sizeof(world);
        require(CreateWellKnownSid(WinWorldSid,nullptr,world,&size),"Fixture SID failed");
        EXPLICIT_ACCESSW entry{}; entry.grfAccessPermissions=KEY_SET_VALUE; entry.grfAccessMode=DENY_ACCESS;
        BuildTrusteeWithSidW(&entry.Trustee,world);
        PACL denied{};
        require(SetEntriesInAclW(1,&entry,previous,&denied)==ERROR_SUCCESS,"Fixture deny ACL failed");
        SECURITY_DESCRIPTOR descriptor{};
        InitializeSecurityDescriptor(&descriptor,SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&descriptor,TRUE,denied,FALSE);
        const auto status=RegSetKeySecurity(key_,DACL_SECURITY_INFORMATION,&descriptor);
        LocalFree(denied);
        require(status==ERROR_SUCCESS,"Fixture deny ACL could not be applied");
    }
    ~DenyRegistryWrite() {
        if (key_) { RegSetKeySecurity(key_,DACL_SECURITY_INFORMATION,original_.data()); RegCloseKey(key_); }
    }
private:
    HKEY key_{};
    std::vector<BYTE> original_;
};
void registry() {
    {
        Fixture f; f.startup(); f.seed();
        {
            DenyRegistryWrite denied(f.user);
            const auto result=f.cleanup().remove(true);
            require(result.issue==UninstallIssue::registry && !result.history_touched && !f.preview_calls,
                    "Registry denial was not caught before data/resource mutation");
            f.intact();
            require(LoginStartup(f.root/L"ClipRope.exe",f.user).enabled(),"Registry denial changed owned startup");
        }
        require(f.cleanup().remove(false).success,"Registry access recovery could not retry");
    }
    {
        Fixture f; f.startup(); f.seed();
        const auto other=f.temporary.path/L"other"/L"ClipRope.exe";
        unsigned observations{};
        BuildUninstallOptions options{f.root,f.local,f.user};
        options.processes=[&](const auto&) {
            if (++observations==2) value(f.user,L"Software\\ClipRope\\Startup",L"Owner",other.native());
            return ActiveBuildCopies{};
        };
        options.remove_preview=[&] { ++f.preview_calls; };
        const auto result=BuildUninstall(options).remove(true);
        require(result.issue==UninstallIssue::shared_history && !result.history_touched && !f.preview_calls,
                "Changed ownership was not revalidated before purge");
        f.intact();
        require(LoginStartup(f.root/L"ClipRope.exe",f.user).registration().owner==other.native(),"Concurrent owner was replaced");
    }
    std::cout<<"Isolated registry write denial preserved data/payload, restored access retried, changed owner blocked purge\n";
}
void guards() {
    {
        Fixture f; f.startup(); f.seed();
        HANDLE locked=CreateFileW((f.root/L"ClipRope.exe").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        require(locked!=INVALID_HANDLE_VALUE,"Lock fixture failed");
        const auto result=f.cleanup().remove(true); CloseHandle(locked);
        require(!result.success && result.issue==UninstallIssue::payload && !result.history_touched,"Locked payload was not rejected before purge");
        f.intact(); require(LoginStartup(f.root/L"ClipRope.exe",f.user).enabled() && !f.preview_calls,"Locked payload changed registration/resources");
        require(f.cleanup().remove(false).success,"Retry after unlocking failed");
    }
    {
        Fixture f; f.startup();
        SetFileAttributesW((f.root/L"ClipRopeWorker.exe").c_str(),FILE_ATTRIBUTE_READONLY);
        require(f.cleanup().remove(false).issue==UninstallIssue::payload,"Readonly payload was forced"); f.intact();
    }
    {
        Fixture f; f.startup();
        std::ofstream(f.root/L"ClipRope.build.ini",std::ios::trunc)<<"[ClipRope]\nLayout=unknown\n";
        require(f.cleanup().remove(false).issue==UninstallIssue::layout,"Unrecognized layout accepted"); f.intact();
    }
    {
        Fixture f; f.startup(); f.seed();
        std::ofstream(f.local/L"ClipRope"/L"unrecognized.txt")<<"Keep this file";
        require(f.cleanup().remove(true).issue==UninstallIssue::history,"Unrecognized history content was erased"); f.intact();
        require(LoginStartup(f.root/L"ClipRope.exe",f.user).enabled() && fs::exists(f.local/L"ClipRope"/L"unrecognized.txt"),"History guard changed program/registration/data");
    }
    std::cout<<"Locked/readonly payload, invalid marker and unrecognized profile guards passed; unlocked retry passed\n";
}
void active() {
    Fixture f; f.startup(); f.seed();
    BuildUninstallOptions options{f.root,f.local,f.user};
    options.processes=[](const auto&) { return ActiveBuildCopies{true,false}; };
    BuildUninstall cleanup(options);
    require(cleanup.remove(false).issue==UninstallIssue::running,"Running target was removed"); f.intact();
    options.processes=[](const auto&) { return ActiveBuildCopies{false,true}; };
    cleanup=BuildUninstall(options);
    require(cleanup.remove(true).issue==UninstallIssue::shared_history,"Other running copy lost shared data"); f.intact();
    value(f.user,L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ClipRope",L"InstallLocation",f.root.parent_path().native());
    // An exact installed current slot cannot be handled as a direct build.
    fs::rename(f.root,f.root.parent_path()/L"current"); f.root=f.root.parent_path()/L"current";
    options.directory=f.root; options.processes=[](const auto&) { return ActiveBuildCopies{}; };
    require(BuildUninstall(options).remove(false).issue==UninstallIssue::layout,"Installed current slot accepted direct cleanup"); f.intact();
    std::cout<<"Running target, another active copy and installed-layout guards passed\n";
}
void reparse() {
    Fixture f; f.startup();
    const auto link=f.temporary.path/L"linked-copy";
    require(fs::create_directory(link),"Junction directory failed");
    const auto substitute=L"\\??\\"+f.root.native(), print=f.root.native();
    struct Header { DWORD tag; WORD length,reserved,sub_offset,sub_length,print_offset,print_length; };
    std::vector<unsigned char> bytes(sizeof(Header)+(substitute.size()+print.size()+2)*sizeof(wchar_t));
    auto* header=reinterpret_cast<Header*>(bytes.data());
    *header={IO_REPARSE_TAG_MOUNT_POINT,WORD(bytes.size()-8),0,0,WORD(substitute.size()*2),WORD((substitute.size()+1)*2),WORD(print.size()*2)};
    auto* names=reinterpret_cast<wchar_t*>(bytes.data()+sizeof(Header));
    std::copy(substitute.begin(),substitute.end(),names);
    std::copy(print.begin(),print.end(),names+substitute.size()+1);
    HANDLE handle=CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    require(handle!=INVALID_HANDLE_VALUE,"Junction handle failed");
    DWORD returned{};
    const auto made=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,bytes.data(),DWORD(bytes.size()),nullptr,0,&returned,nullptr);
    CloseHandle(handle);
    if (!made) { RemoveDirectoryW(link.c_str()); throw std::runtime_error("Junction creation failed"); }
    BuildUninstallOptions options{link,f.local,f.user}; options.processes=[](const auto&) { return ActiveBuildCopies{}; };
    const auto result=BuildUninstall(options).remove(false);
    const auto removed=RemoveDirectoryW(link.c_str());
    require(removed && result.issue==UninstallIssue::layout,"Linked layout was followed"); f.intact();
    require(LoginStartup(f.root/L"ClipRope.exe",f.user).enabled(),"Linked path changed registration");
    std::cout<<"Native NTFS junction rejected without following its target\n";
}
struct Environment {
    std::wstring name,previous;
    bool existed{};
    Environment(const wchar_t* key,const std::wstring& value):name(key) {
        const auto count=GetEnvironmentVariableW(key,nullptr,0);
        if (count) { previous.resize(count); const auto length=GetEnvironmentVariableW(key,previous.data(),count); previous.resize(length); existed=true; }
        require(SetEnvironmentVariableW(key,value.c_str())!=FALSE,"Fixture environment failed");
    }
    ~Environment() { SetEnvironmentVariableW(name.c_str(),existed ? previous.c_str() : nullptr); }
};
HWND helper_dialog(const fs::path& executable) {
    struct Search { const fs::path& path; HWND result{}; } search{executable};
    EnumWindows([](HWND window,LPARAM data)->BOOL {
        auto& search=*reinterpret_cast<Search*>(data);
        if (!GetDlgItem(window,2450) || !IsWindowVisible(window)) return TRUE;
        DWORD pid{}; GetWindowThreadProcessId(window,&pid);
        const auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
        if (!process) return TRUE;
        wchar_t path[32768]{}; DWORD count=DWORD(std::size(path));
        const bool matches=QueryFullProcessImageNameW(process,0,path,&count) &&
            CompareStringOrdinal(path,-1,search.path.c_str(),-1,TRUE)==CSTR_EQUAL;
        CloseHandle(process);
        if (matches) { search.result=window; return FALSE; }
        return TRUE;
    },reinterpret_cast<LPARAM>(&search));
    return search.result;
}
template<class F> void await(F predicate,const char* reason) {
    const auto end=GetTickCount64()+8000;
    do { if (predicate()) return; Sleep(10); } while (GetTickCount64()<end);
    throw std::runtime_error(reason);
}
void bootstrap(const fs::path& binary,bool purge_history,bool cancel) {
    const auto desktop=CreateDesktopW((L"ClipRope.UninstallFixture."+std::to_wstring(GetCurrentProcessId())).c_str(),
                                      nullptr,nullptr,0,GENERIC_ALL,nullptr);
    require(desktop && SetThreadDesktop(desktop),"Bootstrap fixture desktop unavailable");
    Fixture f; f.startup(!cancel); f.seed();
    fs::copy_file(binary,f.root/L"Uninstall.exe",fs::copy_options::overwrite_existing);
    const auto helper=f.temporary.path/L"temp"/L"ClipRope.Uninstall"/L"Uninstall.exe";
    Environment id(L"CLIPROPE_UNINSTALL_TEST_ID",f.id),local(L"CLIPROPE_UNINSTALL_TEST_LOCAL",f.local.native());
    wchar_t station[256]{},name[256]{}; DWORD bytes{};
    require(GetUserObjectInformationW(GetProcessWindowStation(),UOI_NAME,station,sizeof(station),&bytes) &&
            GetUserObjectInformationW(desktop,UOI_NAME,name,sizeof(name),&bytes),"Fixture desktop name unavailable");
    auto desktop_name=std::wstring(station)+L"\\"+name;
    auto command=L"\""+(f.root/L"Uninstall.exe").native()+L"\"";
    STARTUPINFOW startup{sizeof(startup)}; startup.lpDesktop=desktop_name.data();
    PROCESS_INFORMATION process{};
    require(CreateProcessW((f.root/L"Uninstall.exe").c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,f.root.c_str(),&startup,&process),
            "Bootstrap process launch failed");
    CloseHandle(process.hThread);
    require(WaitForSingleObject(process.hProcess,8000)==WAIT_OBJECT_0,"Bootstrap parent did not exit");
    DWORD status{}; GetExitCodeProcess(process.hProcess,&status); CloseHandle(process.hProcess);
    require(status==0,"Bootstrap parent failed");
    HWND dialog{};
    await([&] { dialog=helper_dialog(helper); return dialog!=nullptr; },"Temporary helper dialog unavailable");
    DWORD helper_pid{}; GetWindowThreadProcessId(dialog,&helper_pid);
    const auto worker=OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,FALSE,helper_pid);
    require(worker!=nullptr,"Helper process unavailable");
    if (cancel) {
        SendMessageW(dialog,WM_COMMAND,IDCANCEL,0);
        require(WaitForSingleObject(worker,8000)==WAIT_OBJECT_0,"Cancelled helper did not exit");
        CloseHandle(worker); f.intact();
        require(!LoginStartup(f.root/L"ClipRope.exe",f.user).enabled(),"Cancellation changed the disabled startup choice");
        require(fs::exists(f.local/L"ClipRope"/L"backups"/L"initial"/L"history.db"),"Cancellation removed backups");
    } else {
        if (purge_history) {
            const auto toggle=GetDlgItem(dialog,2450);
            require(IsWindowEnabled(toggle),"Isolated history choice was unavailable");
            SendMessageW(toggle,BM_CLICK,0,0);
            require(SendMessageW(toggle,BM_GETCHECK,0,0)==BST_CHECKED,"Explicit purge choice did not toggle");
        }
        SendMessageW(GetDlgItem(dialog,IDOK),BM_CLICK,0,0);
        await([&] { return !IsWindowVisible(GetDlgItem(dialog,IDCANCEL)); },"Helper cleanup did not finish successfully");
        SendMessageW(GetDlgItem(dialog,IDOK),BM_CLICK,0,0);
        require(WaitForSingleObject(worker,8000)==WAIT_OBJECT_0,"Completed helper did not exit"); CloseHandle(worker);
        f.removed();
        const auto saved=LoginStartup(f.root/L"ClipRope.exe",f.user).registration();
        require(!saved.owner && !saved.command,"Bootstrap retained owned startup entries");
        require(fs::exists(f.local/L"ClipRope"/L"history.db")!=purge_history,"Bootstrap applied the wrong history choice");
    }
    require(fs::exists(helper),"Reusable bounded helper is unavailable");
    std::cout<<"Real Uninstall.exe bootstrap, isolated registry/profile, "<<(cancel ? "cancel" : purge_history ? "purge" : "keep")
             <<" choice and original uninstaller self-removal verified\n";
}
} // namespace
int wmain(int argc,wchar_t** argv) try {
    require(argc==2 || argc==3,"Pass a cleanup case and optional uninstaller path");
    std::string name;
    for (auto c:std::wstring_view(argv[1])) { require(c<128,"Cleanup case must be ASCII"); name+=static_cast<char>(c); }
    if (name=="keep") keep(); else if (name=="purge") purge(); else if (name=="foreign") foreign();
    else if (name=="guards") guards(); else if (name=="active") active(); else if (name=="reparse") reparse();
    else if (name=="registry") registry();
    else if (name.starts_with("bootstrap-") && argc==3) bootstrap(fs::path(argv[2]),name=="bootstrap-purge",name=="bootstrap-cancel");
    else throw std::runtime_error("Unknown cleanup case");
    return 0;
} catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
