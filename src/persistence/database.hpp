#pragma once
#include "application/history_failure.hpp"

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cliprope::persistence {

enum class StorageProblem { access, busy, full, missing_blob, damaged_blob, damaged_database, damaged_manifest, unsupported_format };
class StorageFailure final : public application::HistoryFailure {
public:
    explicit StorageFailure(StorageProblem reason);
    StorageProblem reason() const { return reason_; }
private:
    StorageProblem reason_;
};

inline constexpr int history_application_id = 1129467973;
inline constexpr int history_version = 11;
void require_current_history(sqlite3* db);
// A nonempty journal requires checking the committed version on the opened connection.
bool check_closed_history(const std::filesystem::path& root);
void require_history_header(const std::filesystem::path& file);
void exec(sqlite3* db, const char* sql);
void remove_verified_file(const std::filesystem::path& path, std::string_view digest);
std::string file_sha256(const std::filesystem::path& path);
std::string sha256(std::span<const unsigned char> bytes);
std::filesystem::path safe_blob_path(const std::filesystem::path& root, std::string_view id);

class Statement final {
public:
    Statement(sqlite3* db, const char* sql);
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void bind(int index, std::string_view value);
    void bind(int index, std::wstring_view value);
    void bind(int index, const char* value);
    void bind(int index, int value);
    void bind(int index, std::int64_t value);
    void bind(int index, double value);
    void bind(int index, std::nullptr_t);
    void bind_blob(int index, std::span<const unsigned char> bytes);
    bool step();
    void run();
    void reset(); // Reuse the statement with its current bindings.
    std::string text(int column) const;
    std::wstring wide(int column) const;
    std::int64_t integer(int column) const;
    double real(int column) const;
    bool is_null(int column) const;
    std::vector<unsigned char> blob(int column) const;
private:
    sqlite3_stmt* statement_{};
};

class Transaction final {
public:
    explicit Transaction(sqlite3* db, std::unique_lock<std::recursive_mutex>* yield_while_busy = nullptr);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit();
private:
    sqlite3* db_;
};

class Database final {
public:
    using Fault = std::function<void(std::string_view)>;
    explicit Database(const std::filesystem::path& root, Fault inject = {});
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    sqlite3* handle() const { return db_; }
    std::recursive_mutex& mutex() { return mutex_; }
    const std::filesystem::path& root() const { return root_; }
    void fault(std::string_view point) const;
    void backup_to(const std::filesystem::path& destination);
    static void export_profile(const std::filesystem::path& source, const std::filesystem::path& destination);
    static void restore_backup(const std::filesystem::path& backup, const std::filesystem::path& destination);
    static void checkpoint_existing(const std::filesystem::path& root); // Caller holds the profile writer lock.
    static void verify_store(const std::filesystem::path& source);
private:
    friend class HistoryStore;
    struct AdmissionConnection {};
    struct SnapshotConnection {};
    Database(const Database& owner, AdmissionConnection);
    Database(const Database& owner, SnapshotConnection);
    std::filesystem::path root_;
    Fault inject_;
    std::recursive_mutex mutex_;
    void* process_lock_{};
    sqlite3* db_{};
};

} // namespace cliprope::persistence
