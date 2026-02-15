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
    static constexpr int SCHEMA_VERSION = 7;

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
                "  smart_preview_path   TEXT NOT NULL DEFAULT '',"
                "  camera_make          TEXT NOT NULL DEFAULT '',"
                "  camera               TEXT NOT NULL DEFAULT '',"
                "  lens                 TEXT NOT NULL DEFAULT '',"
                "  lens_make            TEXT NOT NULL DEFAULT '',"
                "  width                INTEGER NOT NULL DEFAULT 0,"
                "  height               INTEGER NOT NULL DEFAULT 0,"
                "  is_raw               INTEGER NOT NULL DEFAULT 0,"
                "  is_video             INTEGER NOT NULL DEFAULT 0,"
                "  creative_style       TEXT NOT NULL DEFAULT '',"
                "  focal_length         REAL NOT NULL DEFAULT 0,"
                "  aperture             REAL NOT NULL DEFAULT 0,"
                "  iso                  REAL NOT NULL DEFAULT 0,"
                "  sync_state           INTEGER NOT NULL DEFAULT 0,"
                "  rating               INTEGER NOT NULL DEFAULT 0,"
                "  color_label          TEXT NOT NULL DEFAULT '',"
                "  flag                 INTEGER NOT NULL DEFAULT 0,"
                "  memo                 TEXT NOT NULL DEFAULT '',"
                "  tags                 TEXT NOT NULL DEFAULT '',"
                "  rating_updated_at    INTEGER NOT NULL DEFAULT 0,"
                "  color_label_updated_at INTEGER NOT NULL DEFAULT 0,"
                "  flag_updated_at      INTEGER NOT NULL DEFAULT 0,"
                "  memo_updated_at      INTEGER NOT NULL DEFAULT 0,"
                "  tags_updated_at      INTEGER NOT NULL DEFAULT 0,"
                "  latitude             REAL NOT NULL DEFAULT 0,"
                "  longitude            REAL NOT NULL DEFAULT 0,"
                "  altitude             REAL NOT NULL DEFAULT 0,"
                "  develop_settings     TEXT NOT NULL DEFAULT '',"
                "  is_managed           INTEGER NOT NULL DEFAULT 1"
                ")"
            );
            if (!ok) return false;

            ok = db_.exec("CREATE INDEX IF NOT EXISTS idx_photos_sync_state ON photos(sync_state)");
            if (!ok) return false;

            if (!createEmbeddingsTable()) return false;
            if (!createFaceTables()) return false;

            db_.setSchemaVersion(SCHEMA_VERSION);
            logNotice() << "[PhotoDatabase] Schema v" << SCHEMA_VERSION << " created";
        }

        // v1 -> v2: add rich metadata columns
        if (version == 1) {
            const char* alters[] = {
                "ALTER TABLE photos ADD COLUMN rating INTEGER NOT NULL DEFAULT 0",
                "ALTER TABLE photos ADD COLUMN color_label TEXT NOT NULL DEFAULT ''",
                "ALTER TABLE photos ADD COLUMN flag INTEGER NOT NULL DEFAULT 0",
                "ALTER TABLE photos ADD COLUMN memo TEXT NOT NULL DEFAULT ''",
                "ALTER TABLE photos ADD COLUMN tags TEXT NOT NULL DEFAULT ''",
                "ALTER TABLE photos ADD COLUMN rating_updated_at INTEGER NOT NULL DEFAULT 0",
                "ALTER TABLE photos ADD COLUMN color_label_updated_at INTEGER NOT NULL DEFAULT 0",
                "ALTER TABLE photos ADD COLUMN flag_updated_at INTEGER NOT NULL DEFAULT 0",
                "ALTER TABLE photos ADD COLUMN memo_updated_at INTEGER NOT NULL DEFAULT 0",
                "ALTER TABLE photos ADD COLUMN tags_updated_at INTEGER NOT NULL DEFAULT 0",
            };
            for (const auto& sql : alters) {
                if (!db_.exec(sql)) {
                    logError() << "[PhotoDatabase] Migration v1->v2 failed: " << sql;
                    return false;
                }
            }
            db_.setSchemaVersion(SCHEMA_VERSION);
            logNotice() << "[PhotoDatabase] Migrated v1 -> v" << SCHEMA_VERSION;
        }

        // v2 -> v3: add smart preview path
        if (version == 2) {
            if (!db_.exec("ALTER TABLE photos ADD COLUMN smart_preview_path TEXT NOT NULL DEFAULT ''")) {
                logError() << "[PhotoDatabase] Migration v2->v3 failed";
                return false;
            }
            version = 3;
            db_.setSchemaVersion(version);
            logNotice() << "[PhotoDatabase] Migrated v2 -> v3";
        }

        // v3 -> v4: add embeddings table
        if (version == 3) {
            if (!createEmbeddingsTable()) {
                logError() << "[PhotoDatabase] Migration v3->v4 failed";
                return false;
            }
            version = 4;
            db_.setSchemaVersion(version);
            logNotice() << "[PhotoDatabase] Migrated v3 -> v4";
        }

        // v4 -> v5: add GPS columns
        if (version == 4) {
            const char* alters[] = {
                "ALTER TABLE photos ADD COLUMN latitude REAL NOT NULL DEFAULT 0",
                "ALTER TABLE photos ADD COLUMN longitude REAL NOT NULL DEFAULT 0",
                "ALTER TABLE photos ADD COLUMN altitude REAL NOT NULL DEFAULT 0",
            };
            for (const auto& sql : alters) {
                if (!db_.exec(sql)) {
                    logError() << "[PhotoDatabase] Migration v4->v5 failed: " << sql;
                    return false;
                }
            }
            version = 5;
            db_.setSchemaVersion(version);
            logNotice() << "[PhotoDatabase] Migrated v4 -> v5";
        }

        // v5 -> v6: add develop_settings + is_managed
        if (version == 5) {
            const char* alters[] = {
                "ALTER TABLE photos ADD COLUMN is_video INTEGER NOT NULL DEFAULT 0",
                "ALTER TABLE photos ADD COLUMN develop_settings TEXT NOT NULL DEFAULT ''",
                "ALTER TABLE photos ADD COLUMN is_managed INTEGER NOT NULL DEFAULT 1",
            };
            for (const auto& sql : alters) {
                if (!db_.exec(sql)) {
                    logError() << "[PhotoDatabase] Migration v5->v6 failed: " << sql;
                    return false;
                }
            }
            version = 6;
            db_.setSchemaVersion(version);
            logNotice() << "[PhotoDatabase] Migrated v5 -> v6";
        }

        // v6 -> v7: add faces + persons tables
        if (version == 6) {
            if (!createFaceTables()) {
                logError() << "[PhotoDatabase] Migration v6->v7 failed";
                return false;
            }
            db_.setSchemaVersion(SCHEMA_VERSION);
            logNotice() << "[PhotoDatabase] Migrated v6 -> v" << SCHEMA_VERSION;
        }

        return true;
    }

    // --- CRUD ---

    bool insertPhoto(const PhotoEntry& e) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare(
            "INSERT OR REPLACE INTO photos "
            "(id, filename, file_size, date_time_original, local_path, local_thumbnail_path, "
            "smart_preview_path, "
            "camera_make, camera, lens, lens_make, width, height, is_raw, is_video, creative_style, "
            "focal_length, aperture, iso, sync_state, "
            "rating, color_label, flag, memo, tags, "
            "rating_updated_at, color_label_updated_at, flag_updated_at, memo_updated_at, tags_updated_at, "
            "latitude, longitude, altitude, develop_settings, is_managed) "
            "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,"
            "?20,?21,?22,?23,?24,?25,?26,?27,?28,?29,?30,?31,?32,?33,?34,?35)"
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

    bool updateRating(const string& id, int rating, int64_t updatedAt) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare("UPDATE photos SET rating=?1, rating_updated_at=?2 WHERE id=?3");
        if (!stmt.valid()) return false;
        stmt.bind(1, rating);
        stmt.bind(2, updatedAt);
        stmt.bind(3, id);
        return stmt.execute();
    }

    bool updateColorLabel(const string& id, const string& label, int64_t updatedAt) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare("UPDATE photos SET color_label=?1, color_label_updated_at=?2 WHERE id=?3");
        if (!stmt.valid()) return false;
        stmt.bind(1, label);
        stmt.bind(2, updatedAt);
        stmt.bind(3, id);
        return stmt.execute();
    }

    bool updateFlag(const string& id, int flag, int64_t updatedAt) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare("UPDATE photos SET flag=?1, flag_updated_at=?2 WHERE id=?3");
        if (!stmt.valid()) return false;
        stmt.bind(1, flag);
        stmt.bind(2, updatedAt);
        stmt.bind(3, id);
        return stmt.execute();
    }

    bool updateMemo(const string& id, const string& memo, int64_t updatedAt) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare("UPDATE photos SET memo=?1, memo_updated_at=?2 WHERE id=?3");
        if (!stmt.valid()) return false;
        stmt.bind(1, memo);
        stmt.bind(2, updatedAt);
        stmt.bind(3, id);
        return stmt.execute();
    }

    bool updateTags(const string& id, const string& tags, int64_t updatedAt) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare("UPDATE photos SET tags=?1, tags_updated_at=?2 WHERE id=?3");
        if (!stmt.valid()) return false;
        stmt.bind(1, tags);
        stmt.bind(2, updatedAt);
        stmt.bind(3, id);
        return stmt.execute();
    }

    bool updateSmartPreviewPath(const string& id, const string& path) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare("UPDATE photos SET smart_preview_path=?1 WHERE id=?2");
        if (!stmt.valid()) return false;
        stmt.bind(1, path);
        stmt.bind(2, id);
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
            "smart_preview_path, "
            "camera_make, camera, lens, lens_make, width, height, is_raw, is_video, creative_style, "
            "focal_length, aperture, iso, sync_state, "
            "rating, color_label, flag, memo, tags, "
            "rating_updated_at, color_label_updated_at, flag_updated_at, memo_updated_at, tags_updated_at, "
            "latitude, longitude, altitude, develop_settings, is_managed) "
            "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,"
            "?20,?21,?22,?23,?24,?25,?26,?27,?28,?29,?30,?31,?32,?33,?34,?35)"
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
            "local_thumbnail_path, smart_preview_path, "
            "camera_make, camera, lens, lens_make, "
            "width, height, is_raw, is_video, creative_style, focal_length, aperture, iso, sync_state, "
            "rating, color_label, flag, memo, tags, "
            "rating_updated_at, color_label_updated_at, flag_updated_at, memo_updated_at, tags_updated_at, "
            "latitude, longitude, altitude, develop_settings, is_managed "
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
            e.localSmartPreviewPath = stmt.getText(6);
            e.cameraMake         = stmt.getText(7);
            e.camera             = stmt.getText(8);
            e.lens               = stmt.getText(9);
            e.lensMake           = stmt.getText(10);
            e.width              = stmt.getInt(11);
            e.height             = stmt.getInt(12);
            e.isRaw              = stmt.getInt(13) != 0;
            e.isVideo            = stmt.getInt(14) != 0;
            e.creativeStyle      = stmt.getText(15);
            e.focalLength        = (float)stmt.getDouble(16);
            e.aperture           = (float)stmt.getDouble(17);
            e.iso                = (float)stmt.getDouble(18);
            e.syncState          = static_cast<SyncState>(stmt.getInt(19));
            e.rating             = stmt.getInt(20);
            e.colorLabel         = stmt.getText(21);
            e.flag               = stmt.getInt(22);
            e.memo               = stmt.getText(23);
            e.tags               = stmt.getText(24);
            e.ratingUpdatedAt    = stmt.getInt64(25);
            e.colorLabelUpdatedAt = stmt.getInt64(26);
            e.flagUpdatedAt      = stmt.getInt64(27);
            e.memoUpdatedAt      = stmt.getInt64(28);
            e.tagsUpdatedAt      = stmt.getInt64(29);
            e.latitude           = stmt.getDouble(30);
            e.longitude          = stmt.getDouble(31);
            e.altitude           = stmt.getDouble(32);
            e.developSettings    = stmt.getText(33);
            e.isManaged          = stmt.getInt(34) != 0;

            // Syncing state doesn't survive restart
            if (e.syncState == SyncState::Syncing) {
                e.syncState = SyncState::LocalOnly;
            }

            result.push_back(std::move(e));
        }
        return result;
    }

    // --- Embeddings ---

    bool insertEmbedding(const string& photoId, const string& model,
                         const string& source, const vector<float>& vec) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare(
            "INSERT OR REPLACE INTO embeddings (photo_id, model, source, vector, created_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5)");
        if (!stmt.valid()) return false;
        stmt.bind(1, photoId);
        stmt.bind(2, model);
        stmt.bind(3, source);
        stmt.bindBlob(4, vec.data(), (int)(vec.size() * sizeof(float)));
        stmt.bind(5, (int64_t)chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()).count());
        return stmt.execute();
    }

    vector<float> getEmbedding(const string& photoId, const string& model,
                               const string& source = "image") {
        auto stmt = db_.prepare(
            "SELECT vector FROM embeddings WHERE photo_id=?1 AND model=?2 AND source=?3");
        if (!stmt.valid()) return {};
        stmt.bind(1, photoId);
        stmt.bind(2, model);
        stmt.bind(3, source);
        if (stmt.step()) {
            auto [data, size] = stmt.getBlob(0);
            if (data && size > 0) {
                int count = size / (int)sizeof(float);
                vector<float> vec(count);
                memcpy(vec.data(), data, size);
                return vec;
            }
        }
        return {};
    }

    bool hasEmbedding(const string& photoId, const string& model,
                      const string& source = "image") {
        auto stmt = db_.prepare(
            "SELECT 1 FROM embeddings WHERE photo_id=?1 AND model=?2 AND source=?3");
        if (!stmt.valid()) return false;
        stmt.bind(1, photoId);
        stmt.bind(2, model);
        stmt.bind(3, source);
        return stmt.step();
    }

    bool deleteEmbeddings(const string& photoId) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare("DELETE FROM embeddings WHERE photo_id=?1");
        if (!stmt.valid()) return false;
        stmt.bind(1, photoId);
        return stmt.execute();
    }

    vector<string> getPhotosWithoutEmbedding(const string& model,
                                              const string& source = "image") {
        vector<string> result;
        auto stmt = db_.prepare(
            "SELECT p.id FROM photos p "
            "LEFT JOIN embeddings e ON p.id = e.photo_id AND e.model = ?1 AND e.source = ?2 "
            "WHERE e.photo_id IS NULL");
        if (!stmt.valid()) return result;
        stmt.bind(1, model);
        stmt.bind(2, source);
        while (stmt.step()) {
            result.push_back(stmt.getText(0));
        }
        return result;
    }

    // --- Faces / Persons ---

    // Insert persons (name -> assigned id). Returns name->id map.
    unordered_map<string, int> insertPersons(const vector<string>& names) {
        unordered_map<string, int> result;
        if (names.empty()) return result;

        lock_guard<mutex> lock(db_.writeMutex());
        db_.beginTransaction();

        auto ins = db_.prepare(
            "INSERT OR IGNORE INTO persons (name, created_at) VALUES (?1, ?2)");
        if (!ins.valid()) { db_.rollback(); return result; }

        int64_t now = chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()).count();

        for (const auto& name : names) {
            ins.bind(1, name);
            ins.bind(2, now);
            ins.execute();
            ins.reset();
        }
        db_.commit();

        // Read back all persons to get IDs
        auto sel = db_.prepare("SELECT id, name FROM persons");
        if (sel.valid()) {
            while (sel.step()) {
                result[sel.getText(1)] = sel.getInt(0);
            }
        }
        return result;
    }

    struct FaceRow {
        string photoId;
        int personId = 0;    // 0 = unnamed
        float x, y, w, h;
        string source;
        int lrClusterId = 0;
    };

    int insertFaces(const vector<FaceRow>& faces) {
        if (faces.empty()) return 0;

        lock_guard<mutex> lock(db_.writeMutex());
        db_.beginTransaction();

        auto stmt = db_.prepare(
            "INSERT INTO faces (photo_id, person_id, x, y, w, h, source, lr_cluster_id, created_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)");
        if (!stmt.valid()) { db_.rollback(); return 0; }

        int64_t now = chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()).count();

        int count = 0;
        for (const auto& f : faces) {
            stmt.bind(1, f.photoId);
            if (f.personId > 0) {
                stmt.bind(2, f.personId);
            } else {
                stmt.bindNull(2);
            }
            stmt.bind(3, (double)f.x);
            stmt.bind(4, (double)f.y);
            stmt.bind(5, (double)f.w);
            stmt.bind(6, (double)f.h);
            stmt.bind(7, f.source);
            stmt.bind(8, f.lrClusterId);
            stmt.bind(9, now);
            if (stmt.execute()) count++;
            stmt.reset();
        }

        db_.commit();
        return count;
    }

    vector<FaceRow> getFacesForPhoto(const string& photoId) {
        vector<FaceRow> result;
        auto stmt = db_.prepare(
            "SELECT f.photo_id, COALESCE(f.person_id, 0), f.x, f.y, f.w, f.h, "
            "f.source, COALESCE(f.lr_cluster_id, 0), COALESCE(p.name, '') "
            "FROM faces f LEFT JOIN persons p ON f.person_id = p.id "
            "WHERE f.photo_id = ?1");
        if (!stmt.valid()) return result;
        stmt.bind(1, photoId);
        while (stmt.step()) {
            FaceRow row;
            row.photoId = stmt.getText(0);
            row.personId = stmt.getInt(1);
            row.x = (float)stmt.getDouble(2);
            row.y = (float)stmt.getDouble(3);
            row.w = (float)stmt.getDouble(4);
            row.h = (float)stmt.getDouble(5);
            row.source = stmt.getText(6);
            row.lrClusterId = stmt.getInt(7);
            result.push_back(std::move(row));
        }
        return result;
    }

    // Get person list with face count, sorted by count descending
    vector<pair<string, int>> getPersonList() {
        vector<pair<string, int>> result;
        auto stmt = db_.prepare(
            "SELECT p.name, COUNT(*) as cnt FROM faces f "
            "JOIN persons p ON f.person_id = p.id "
            "GROUP BY p.name ORDER BY cnt DESC");
        if (!stmt.valid()) return result;
        while (stmt.step()) {
            result.push_back({stmt.getText(0), stmt.getInt(1)});
        }
        return result;
    }

    int getFaceCount() {
        auto stmt = db_.prepare("SELECT COUNT(*) FROM faces");
        if (!stmt.valid()) return 0;
        if (stmt.step()) return stmt.getInt(0);
        return 0;
    }

    int getPersonCount() {
        auto stmt = db_.prepare("SELECT COUNT(*) FROM persons");
        if (!stmt.valid()) return 0;
        if (stmt.step()) return stmt.getInt(0);
        return 0;
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

    bool createEmbeddingsTable() {
        return db_.exec(
            "CREATE TABLE IF NOT EXISTS embeddings ("
            "  photo_id   TEXT NOT NULL,"
            "  model      TEXT NOT NULL,"
            "  source     TEXT NOT NULL,"
            "  vector     BLOB NOT NULL,"
            "  created_at INTEGER NOT NULL DEFAULT 0,"
            "  PRIMARY KEY (photo_id, model, source)"
            ")");
    }

    bool createFaceTables() {
        bool ok = db_.exec(
            "CREATE TABLE IF NOT EXISTS persons ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  name TEXT NOT NULL UNIQUE,"
            "  created_at INTEGER NOT NULL DEFAULT 0"
            ")");
        if (!ok) return false;

        ok = db_.exec("CREATE INDEX IF NOT EXISTS idx_persons_name ON persons(name)");
        if (!ok) return false;

        ok = db_.exec(
            "CREATE TABLE IF NOT EXISTS faces ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  photo_id TEXT NOT NULL,"
            "  person_id INTEGER,"
            "  x REAL NOT NULL,"
            "  y REAL NOT NULL,"
            "  w REAL NOT NULL,"
            "  h REAL NOT NULL,"
            "  source TEXT NOT NULL DEFAULT 'lightroom',"
            "  lr_cluster_id INTEGER,"
            "  created_at INTEGER NOT NULL DEFAULT 0"
            ")");
        if (!ok) return false;

        ok = db_.exec("CREATE INDEX IF NOT EXISTS idx_faces_photo ON faces(photo_id)");
        if (!ok) return false;

        ok = db_.exec("CREATE INDEX IF NOT EXISTS idx_faces_person ON faces(person_id)");
        return ok;
    }

    static void bindEntry(Database::Statement& stmt, const PhotoEntry& e) {
        stmt.bind(1, e.id);
        stmt.bind(2, e.filename);
        stmt.bind(3, (int64_t)e.fileSize);
        stmt.bind(4, e.dateTimeOriginal);
        stmt.bind(5, e.localPath);
        stmt.bind(6, e.localThumbnailPath);
        stmt.bind(7, e.localSmartPreviewPath);
        stmt.bind(8, e.cameraMake);
        stmt.bind(9, e.camera);
        stmt.bind(10, e.lens);
        stmt.bind(11, e.lensMake);
        stmt.bind(12, e.width);
        stmt.bind(13, e.height);
        stmt.bind(14, e.isRaw ? 1 : 0);
        stmt.bind(15, e.isVideo ? 1 : 0);
        stmt.bind(16, e.creativeStyle);
        stmt.bind(17, (double)e.focalLength);
        stmt.bind(18, (double)e.aperture);
        stmt.bind(19, (double)e.iso);
        stmt.bind(20, static_cast<int>(e.syncState));
        stmt.bind(21, e.rating);
        stmt.bind(22, e.colorLabel);
        stmt.bind(23, e.flag);
        stmt.bind(24, e.memo);
        stmt.bind(25, e.tags);
        stmt.bind(26, e.ratingUpdatedAt);
        stmt.bind(27, e.colorLabelUpdatedAt);
        stmt.bind(28, e.flagUpdatedAt);
        stmt.bind(29, e.memoUpdatedAt);
        stmt.bind(30, e.tagsUpdatedAt);
        stmt.bind(31, e.latitude);
        stmt.bind(32, e.longitude);
        stmt.bind(33, e.altitude);
        stmt.bind(34, e.developSettings);
        stmt.bind(35, e.isManaged ? 1 : 0);
    }
};
