#include "history_fixture.hpp"
#include "persistence/database.hpp"
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>

using namespace cliprope::persistence;
using history_fixture::require;
using history_fixture::rejects;
namespace fs = std::filesystem;
namespace {
class Raw {
public:
    Raw(const fs::path& root, bool create = false) {
        if (create) fs::create_directories(root);
        const auto file = (root / "history.db").u8string();
        int rc = sqlite3_open_v2(reinterpret_cast<const char*>(file.c_str()), &db,
                create ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY, nullptr);
        if (rc != SQLITE_OK) { sqlite3_close(db); throw std::runtime_error("Fixture SQLite open failed"); }
    }
    ~Raw() { sqlite3_close(db); }
    sqlite3* db{};
};
std::int64_t number(sqlite3* db, const char* sql) {
    Statement st(db, sql);
    require(st.step(), "No fixture row");
    return st.integer(0);
}
std::string str(sqlite3* db, const char* sql) {
    Statement st(db, sql);
    require(st.step(), "No fixture value");
    return st.text(0);
}
void schema_test(const fs::path& root) {
    rejects([&]{
        Database interrupted(root, [](auto point) {
            if (point == "schema-create") throw StorageFailure(StorageProblem::full);
        });
    }, "First creation ignored a failed transaction");
    require(!fs::exists(root / "history.db") && fs::is_empty(root / "staging"),
            "Failed first creation installed an incomplete database");
    {
        Database db(root);
        rejects([&]{ Database duplicate(root); }, "A second writer acquired the same store");
        require(number(db.handle(), "PRAGMA foreign_keys")==1, "Foreign keys not enforced");
        require(str(db.handle(), "PRAGMA journal_mode")=="wal" && number(db.handle(),"PRAGMA synchronous")==2,
                "History durability settings mismatch");
        exec(db.handle(), R"(
            INSERT INTO snapshots(id) VALUES('s');
            INSERT INTO blobs(id,size,inline_bytes) VALUES('ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',3,X'616263');
            INSERT INTO representations(snapshot_id,ordinal,standard_id,status,byte_count,blob_id)
               SELECT 's',0,13,'saved',3,id FROM blobs;
            INSERT INTO representations(snapshot_id,ordinal,format_name,status) VALUES('s',1,'HTML Format','skipped');
            INSERT INTO ropes(id,name) VALUES('other','Other');
            INSERT INTO entries(id,snapshot_id,state,rope_id,sort_key) VALUES
               ('B','s','Hanging','main',20),('C','s','Hanging','main',20),('A','s','Hanging','main',10),
               ('D','s','Hanging','other',1),('F','s','Floating',NULL,NULL);
            UPDATE card_presentation SET font_size=19.5,font_override=1,bold=1,opacity=0.75,x=-32,y=42,monitor='Panel L' WHERE entry_id='F';
            UPDATE rope_view_state SET anchor_entry_id='B',offset=2.5,version=7 WHERE rope_id='main';
        )");
        for (const auto* sql : {
                 "UPDATE entries SET rope_id=NULL WHERE id='A'", "UPDATE entries SET sort_key=NULL WHERE id='A'",
                 "UPDATE entries SET sort_key=1 WHERE id='F'", "UPDATE entries SET rope_id='main' WHERE id='F'",
                 "UPDATE entries SET state='Other' WHERE id='A'", "UPDATE entries SET rope_id='missing' WHERE id='A'",
                 "INSERT INTO entries(id,snapshot_id,state,rope_id,sort_key) VALUES('bad','s','Floating','main',1)",
                 "INSERT INTO ropes(id,name,is_main) VALUES('bad','Bad',1)", "UPDATE ropes SET capacity=0 WHERE id='main'",
                 "UPDATE ropes SET name='Changed' WHERE is_main=1",
                 "DELETE FROM ropes WHERE id='system:rebin'",
                 "UPDATE ropes SET name='Other' WHERE id='system:rebin'",
                 "UPDATE ropes SET id='other-id' WHERE id='system:rebin'",
                 "UPDATE ropes SET capacity=100 WHERE id='system:rebin'",
                 "UPDATE ropes SET capacity=NULL WHERE id='other'",
                 "INSERT INTO ropes(id,name,capacity) VALUES('extra','ReBin',NULL)",
                 "UPDATE app_settings SET active_rope_id='system:rebin'",
                 "UPDATE rope_view_state SET compact=2 WHERE rope_id='main'",
                 "DELETE FROM ropes WHERE id='main'"})
            rejects([&]{exec(db.handle(), sql);}, "Expected history constraint was not enforced");
    }
    Database db(root);
    require(number(db.handle(),"PRAGMA user_version")==11 && number(db.handle(),"PRAGMA foreign_keys")==1 &&
            number(db.handle(),"SELECT count(*) FROM ropes WHERE is_main=1")==1, "Reopen metadata mismatch");
    require(str(db.handle(), "SELECT name FROM ropes WHERE is_main=1") == "main", "Main name is not fixed");
    require(str(db.handle(), "SELECT group_concat(id,'') FROM (SELECT id FROM entries WHERE rope_id='main' AND state='Hanging' ORDER BY sort_key,id)")=="ABC",
            "Displayed order or tie break mismatch");
    require(number(db.handle(),"SELECT count(*) FROM entries WHERE snapshot_id='s'")==5 &&
            number(db.handle(),"SELECT count(*) FROM representations WHERE snapshot_id='s'")==2 &&
            str(db.handle(),"SELECT hex(inline_bytes) FROM blobs")=="616263", "Representations did not survive reopen");
    require(number(db.handle(),"SELECT count(*) FROM card_presentation WHERE entry_id='F' AND font_override=1 AND font_size=19.5 AND bold=1 AND opacity=.75 AND monitor='Panel L' AND x=-32 AND y=42")==1,
            "Floating layout or style mismatch");
    require(number(db.handle(),"SELECT count(*) FROM rope_view_state WHERE rope_id='main' AND anchor_entry_id='B' AND offset=2.5 AND version=7")==1,
            "Viewport did not survive reopen");
}
template<class Action> void rejects_format(Action action) {
    try { action(); }
    catch (const StorageFailure& failure) {
        require(failure.reason() == StorageProblem::unsupported_format, "Wrong format diagnostic");
        return;
    }
    throw std::runtime_error("Unsupported database accepted");
}
void unsupported_test(const fs::path& directory) {
    const auto backup = directory / "backup", empty = directory / "empty";
    fs::create_directory(empty);
    for (const int version : {0,1,2,3,4,5,6,7,8,9,10,11,12}) {
        const auto root = directory / std::to_string(version);
        {
            Raw raw(root,true);
            exec(raw.db, "PRAGMA journal_mode=WAL");
            exec(raw.db, "CREATE TABLE sentinel(data TEXT); INSERT INTO sentinel VALUES('keep')");
            exec(raw.db, version == 11 ? "PRAGMA application_id=123" : "PRAGMA application_id=1129467973");
            exec(raw.db, ("PRAGMA user_version=" + std::to_string(version)).c_str());
        }
        const auto hash = file_sha256(root / "history.db");
        rejects_format([&]{ Database db(root); });
        rejects_format([&]{ Database::verify_store(root); });
        rejects_format([&]{ Database::checkpoint_existing(root); });
        rejects_format([&]{ Database::export_profile(root, backup); });
        rejects_format([&]{ Database::restore_backup(root, empty); });
        require(std::distance(fs::directory_iterator(root), fs::directory_iterator{}) == 1 &&
                file_sha256(root / "history.db") == hash &&
                !fs::exists(backup) && fs::is_empty(empty), "Rejected format changed data");
    }
}

void backup_retry(const fs::path& directory) {
    Database db(directory / "store");
    const auto restored_path = directory / "restored";
    db.backup_to(restored_path);
    rejects([&]{db.backup_to(restored_path);},"Backup replaced an existing destination");
    std::jthread reader;
    bool held = false;
    Database restored(restored_path,[&](std::string_view point) {
        if (point!="backup-install") return;
        for (const auto& item : fs::directory_iterator(directory)) {
            if (!item.path().filename().wstring().starts_with(L"cliprope-partial-")) continue;
            HANDLE handle=CreateFileW(item.path().c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
                                      nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
            require(handle!=INVALID_HANDLE_VALUE,"Could not hold a transient backup reader");
            reader=std::jthread([handle]{ Sleep(100); CloseHandle(handle); });
            held=true;
            return;
        }
        throw std::runtime_error("No completed backup staging directory");
    });
    require(number(restored.handle(),"SELECT count(*) FROM ropes")==2 && number(restored.handle(),"PRAGMA user_version")==11,
            "Exported store could not reopen");
    restored.backup_to(directory/"after-reader");
    require(held && fs::is_regular_file(directory/"after-reader"/"history.db"),
            "Verified backup did not install after a transient reader released it");
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        history_fixture::Directory tmp;
        if (argc==2 && std::wstring_view(argv[1])==L"--unsupported") unsupported_test(tmp.path);
        else if (argc==2 && std::wstring_view(argv[1])==L"--backup-retry") backup_retry(tmp.path);
        else if (argc==1) schema_test(tmp.path);
        else return 2;
        std::cout << "History database scenario passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
