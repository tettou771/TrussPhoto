#pragma once

// =============================================================================
// PhotoDatabase - Photos table CRUD, schema management, JSON migration
// =============================================================================

#include "Database.h"
#include "PhotoEntry.h"
#include <vector>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

class PhotoDatabase {
public:
    static constexpr int SCHEMA_VERSION = 1;

    bool open(const string& dbPath) {
        if (!db_.open(dbPath)) return false;
        return ensureSchema();
    }

    void close() { db_.close(); }
    bool isOpen() const { return db_.isOpen(); }

    // --- Schema ---

    bool ensureSchema() {
        int version = db_.getSchemaVersion();
        if (version >= SCHEMA_VERSION) return true;

        if (version == 0) {
            // Fresh database - create tables
            bool ok = db_.exec(
                "CREATE TABLE IF NOT EXISTS photos ("
                "  id                   TEXT PRIMARY KEY,"
                "  filename             TEXT NOT NULL DEFAULT '',"
                "  file_size            INTEGER NOT NULL DEFAULT 0,"
                "  date_time_original   TEXT NOT NULL DEFAULT '',"
                "  local_path           TEXT NOT NULL DEFAULT '',"
                "  local_thumbnail_path TEXT NOT NULL DEFAULT '',"
                "  camera_make          TEXT NOT NULL DEFAULT '',"
                "  camera               TEXT NOT NULL DEFAULT '',"
                "  lens                 TEXT NOT NULL DEFAULT '',"
                "  lens_make            TEXT NOT NULL DEFAULT '',"
                "  width                INTEGER NOT NULL DEFAULT 0,"
                "  height               INTEGER NOT NULL DEFAULT 0,"
                "  is_raw               INTEGER NOT NULL DEFAULT 0,"
                "  creative_style       TEXT NOT NULL DEFAULT '',"
                "  focal_length         REAL NOT NULL DEFAULT 0,"
                "  aperture             REAL NOT NULL DEFAULT 0,"
                "  iso                  REAL NOT NULL DEFAULT 0,"
                "  sync_state           INTEGER NOT NULL DEFAULT 0"
                ")"
            );
            if (!ok) return false;

            ok = db_.exec("CREATE INDEX IF NOT EXISTS idx_photos_sync_state ON photos(sync_state)");
            if (!ok) return false;

            db_.setSchemaVersion(SCHEMA_VERSION);
            logNotice() << "[PhotoDatabase] Schema v" << SCHEMA_VERSION << " created";
        }
        // Future: version upgrades (ALTER TABLE etc.) go here

        return true;
    }

    // --- CRUD ---

    bool insertPhoto(const PhotoEntry& e) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare(
            "INSERT OR REPLACE INTO photos "
            "(id, filename, file_size, date_time_original, local_path, local_thumbnail_path, "
            "camera_make, camera, lens, lens_make, width, height, is_raw, creative_style, "
            "focal_length, aperture, iso, sync_state) "
            "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18)"
        );
        if (!stmt.valid()) return false;
        bindEntry(stmt, e);
        return stmt.execute();
    }

    bool updatePhoto(const PhotoEntry& e) {
        return insertPhoto(e); // INSERT OR REPLACE
    }

    bool updateSyncState(const string& id, SyncState state) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare("UPDATE photos SET sync_state=?1 WHERE id=?2");
        if (!stmt.valid()) return false;
        stmt.bind(1, static_cast<int>(state));
        stmt.bind(2, id);
        return stmt.execute();
    }

    bool updateLocalPath(const string& id, const string& path) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare("UPDATE photos SET local_path=?1 WHERE id=?2");
        if (!stmt.valid()) return false;
        stmt.bind(1, path);
        stmt.bind(2, id);
        return stmt.execute();
    }

    bool updateThumbnailPath(const string& id, const string& path) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare("UPDATE photos SET local_thumbnail_path=?1 WHERE id=?2");
        if (!stmt.valid()) return false;
        stmt.bind(1, path);
        stmt.bind(2, id);
        return stmt.execute();
    }

    bool updateLocalAndThumbnailPaths(const string& id, const string& localPath, const string& thumbPath) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare("UPDATE photos SET local_path=?1, local_thumbnail_path=?2 WHERE id=?3");
        if (!stmt.valid()) return false;
        stmt.bind(1, localPath);
        stmt.bind(2, thumbPath);
        stmt.bind(3, id);
        return stmt.execute();
    }

    bool deletePhoto(const string& id) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare("DELETE FROM photos WHERE id=?1");
        if (!stmt.valid()) return false;
        stmt.bind(1, id);
        return stmt.execute();
    }

    bool hasPhoto(const string& id) {
        auto stmt = db_.prepare("SELECT 1 FROM photos WHERE id=?1");
        if (!stmt.valid()) return false;
        stmt.bind(1, id);
        return stmt.step();
    }

    // --- Bulk operations ---

    bool insertPhotos(const vector<PhotoEntry>& entries) {
        lock_guard<mutex> lock(db_.writeMutex());
        db_.beginTransaction();

        auto stmt = db_.prepare(
            "INSERT OR REPLACE INTO photos "
            "(id, filename, file_size, date_time_original, local_path, local_thumbnail_path, "
            "camera_make, camera, lens, lens_make, width, height, is_raw, creative_style, "
            "focal_length, aperture, iso, sync_state) "
            "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18)"
        );
        if (!stmt.valid()) {
            db_.rollback();
            return false;
        }

        for (const auto& e : entries) {
            bindEntry(stmt, e);
            if (!stmt.execute()) {
                db_.rollback();
                return false;
            }
            stmt.reset();
        }

        db_.commit();
        return true;
    }

    // --- Load all ---

    vector<PhotoEntry> loadAll() {
        vector<PhotoEntry> result;
        auto stmt = db_.prepare(
            "SELECT id, filename, file_size, date_time_original, local_path, "
            "local_thumbnail_path, camera_make, camera, lens, lens_make, "
            "width, height, is_raw, creative_style, focal_length, aperture, iso, sync_state "
            "FROM photos"
        );
        if (!stmt.valid()) return result;

        while (stmt.step()) {
            PhotoEntry e;
            e.id                 = stmt.getText(0);
            e.filename           = stmt.getText(1);
            e.fileSize           = (uintmax_t)stmt.getInt64(2);
            e.dateTimeOriginal   = stmt.getText(3);
            e.localPath          = stmt.getText(4);
            e.localThumbnailPath = stmt.getText(5);
            e.cameraMake         = stmt.getText(6);
            e.camera             = stmt.getText(7);
            e.lens               = stmt.getText(8);
            e.lensMake           = stmt.getText(9);
            e.width              = stmt.getInt(10);
            e.height             = stmt.getInt(11);
            e.isRaw              = stmt.getInt(12) != 0;
            e.creativeStyle      = stmt.getText(13);
            e.focalLength        = (float)stmt.getDouble(14);
            e.aperture           = (float)stmt.getDouble(15);
            e.iso                = (float)stmt.getDouble(16);
            e.syncState          = static_cast<SyncState>(stmt.getInt(17));

            // Syncing state doesn't survive restart
            if (e.syncState == SyncState::Syncing) {
                e.syncState = SyncState::LocalOnly;
            }

            result.push_back(std::move(e));
        }
        return result;
    }

    // --- JSON migration ---

    bool migrateFromJson(const string& jsonPath) {
        if (!fs::exists(jsonPath)) return false;

        ifstream file(jsonPath);
        if (!file) return false;

        try {
            nlohmann::json j;
            file >> j;
            file.close();

            vector<PhotoEntry> entries;
            for (const auto& photoJson : j["photos"]) {
                entries.push_back(photoJson.get<PhotoEntry>());
            }

            if (entries.empty()) return true;

            bool ok = insertPhotos(entries);
            if (ok) {
                // Rename original JSON as backup
                string backupPath = jsonPath + ".migrated";
                try {
                    fs::rename(jsonPath, backupPath);
                    logNotice() << "[PhotoDatabase] Migrated " << entries.size()
                                << " photos from JSON, backup: " << backupPath;
                } catch (...) {
                    logWarning() << "[PhotoDatabase] Migration OK but failed to rename JSON";
                }
            }
            return ok;
        } catch (const exception& e) {
            logError() << "[PhotoDatabase] JSON migration failed: " << e.what();
            return false;
        }
    }

private:
    Database db_;

    static void bindEntry(Database::Statement& stmt, const PhotoEntry& e) {
        stmt.bind(1, e.id);
        stmt.bind(2, e.filename);
        stmt.bind(3, (int64_t)e.fileSize);
        stmt.bind(4, e.dateTimeOriginal);
        stmt.bind(5, e.localPath);
        stmt.bind(6, e.localThumbnailPath);
        stmt.bind(7, e.cameraMake);
        stmt.bind(8, e.camera);
        stmt.bind(9, e.lens);
        stmt.bind(10, e.lensMake);
        stmt.bind(11, e.width);
        stmt.bind(12, e.height);
        stmt.bind(13, e.isRaw ? 1 : 0);
        stmt.bind(14, e.creativeStyle);
        stmt.bind(15, (double)e.focalLength);
        stmt.bind(16, (double)e.aperture);
        stmt.bind(17, (double)e.iso);
        stmt.bind(18, static_cast<int>(e.syncState));
    }
};
