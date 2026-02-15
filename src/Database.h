#pragma once

// =============================================================================
// Database - Thin RAII wrapper around SQLite3 C API
// =============================================================================

#include <sqlite3.h>
#include <string>
#include <mutex>
#include <TrussC.h>

using namespace std;
using namespace tc;

class Database {
public:
    Database() = default;
    ~Database() { close(); }

    // Non-copyable
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open(const string& path) {
        close();
        int rc = sqlite3_open(path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            logError() << "[Database] Failed to open: " << path << " - " << sqlite3_errmsg(db_);
            sqlite3_close(db_);
            db_ = nullptr;
            return false;
        }
        // WAL mode for concurrent reads
        exec("PRAGMA journal_mode=WAL");
        // Busy timeout 5 seconds
        sqlite3_busy_timeout(db_, 5000);
        logNotice() << "[Database] Opened: " << path;
        return true;
    }

    void close() {
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
    }

    bool isOpen() const { return db_ != nullptr; }

    bool exec(const string& sql) {
        if (!db_) return false;
        char* errMsg = nullptr;
        int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            logError() << "[Database] exec error: " << (errMsg ? errMsg : "unknown");
            sqlite3_free(errMsg);
            return false;
        }
        return true;
    }

    bool beginTransaction() { return exec("BEGIN TRANSACTION"); }
    bool commit() { return exec("COMMIT"); }
    bool rollback() { return exec("ROLLBACK"); }

    // RAII prepared statement
    class Statement {
    public:
        Statement() = default;
        Statement(sqlite3_stmt* stmt) : stmt_(stmt) {}
        ~Statement() { finalize(); }

        // Move only
        Statement(Statement&& other) noexcept : stmt_(other.stmt_) { other.stmt_ = nullptr; }
        Statement& operator=(Statement&& other) noexcept {
            if (this != &other) {
                finalize();
                stmt_ = other.stmt_;
                other.stmt_ = nullptr;
            }
            return *this;
        }
        Statement(const Statement&) = delete;
        Statement& operator=(const Statement&) = delete;

        bool valid() const { return stmt_ != nullptr; }

        void bind(int idx, const string& val) {
            if (stmt_) sqlite3_bind_text(stmt_, idx, val.c_str(), (int)val.size(), SQLITE_TRANSIENT);
        }
        void bind(int idx, int val) {
            if (stmt_) sqlite3_bind_int(stmt_, idx, val);
        }
        void bind(int idx, int64_t val) {
            if (stmt_) sqlite3_bind_int64(stmt_, idx, val);
        }
        void bind(int idx, double val) {
            if (stmt_) sqlite3_bind_double(stmt_, idx, val);
        }
        void bindBlob(int idx, const void* data, int size) {
            if (stmt_) sqlite3_bind_blob(stmt_, idx, data, size, SQLITE_TRANSIENT);
        }

        // Step: returns true if there's a row (SQLITE_ROW)
        bool step() {
            if (!stmt_) return false;
            int rc = sqlite3_step(stmt_);
            return rc == SQLITE_ROW;
        }

        // Execute without expecting rows (INSERT/UPDATE/DELETE)
        bool execute() {
            if (!stmt_) return false;
            int rc = sqlite3_step(stmt_);
            return rc == SQLITE_DONE;
        }

        string getText(int col) {
            if (!stmt_) return "";
            const unsigned char* txt = sqlite3_column_text(stmt_, col);
            return txt ? string(reinterpret_cast<const char*>(txt)) : "";
        }
        int getInt(int col) {
            return stmt_ ? sqlite3_column_int(stmt_, col) : 0;
        }
        int64_t getInt64(int col) {
            return stmt_ ? sqlite3_column_int64(stmt_, col) : 0;
        }
        double getDouble(int col) {
            return stmt_ ? sqlite3_column_double(stmt_, col) : 0.0;
        }
        pair<const void*, int> getBlob(int col) {
            if (!stmt_) return {nullptr, 0};
            const void* data = sqlite3_column_blob(stmt_, col);
            int size = sqlite3_column_bytes(stmt_, col);
            return {data, size};
        }

        void bindNull(int idx) {
            if (stmt_) sqlite3_bind_null(stmt_, idx);
        }

        sqlite3_stmt* rawStmt() const { return stmt_; }

        void reset() {
            if (stmt_) {
                sqlite3_reset(stmt_);
                sqlite3_clear_bindings(stmt_);
            }
        }

    private:
        sqlite3_stmt* stmt_ = nullptr;

        void finalize() {
            if (stmt_) {
                sqlite3_finalize(stmt_);
                stmt_ = nullptr;
            }
        }
    };

    Statement prepare(const string& sql) {
        if (!db_) return Statement();
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), (int)sql.size(), &stmt, nullptr);
        if (rc != SQLITE_OK) {
            logError() << "[Database] prepare error: " << sqlite3_errmsg(db_);
            return Statement();
        }
        return Statement(stmt);
    }

    int getSchemaVersion() {
        auto stmt = prepare("PRAGMA user_version");
        if (stmt.step()) {
            return stmt.getInt(0);
        }
        return 0;
    }

    void setSchemaVersion(int version) {
        exec("PRAGMA user_version=" + to_string(version));
    }

    // Write mutex for serializing writes from multiple threads
    mutex& writeMutex() { return writeMutex_; }

private:
    sqlite3* db_ = nullptr;
    mutex writeMutex_;
};
