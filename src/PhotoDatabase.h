#pragma once

// =============================================================================
// PhotoDatabase - Photos table CRUD, schema management, JSON migration
// =============================================================================

#include "Database.h"
#include "PhotoEntry.h"
#include <vector>
#include <unordered_set>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

class PhotoDatabase {
public:
    static constexpr int SCHEMA_VERSION = 9;

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
                "  is_managed           INTEGER NOT NULL DEFAULT 1,"
                "  face_scanned        INTEGER NOT NULL DEFAULT 0,"
                "  lens_correction_params TEXT NOT NULL DEFAULT '',"
                "  exposure_time       TEXT NOT NULL DEFAULT '',"
                "  exposure_bias       REAL NOT NULL DEFAULT 0,"
                "  orientation         INTEGER NOT NULL DEFAULT 1,"
                "  white_balance       TEXT NOT NULL DEFAULT '',"
                "  focal_length_35mm   INTEGER NOT NULL DEFAULT 0,"
                "  offset_time         TEXT NOT NULL DEFAULT '',"
                "  body_serial         TEXT NOT NULL DEFAULT '',"
                "  lens_serial         TEXT NOT NULL DEFAULT '',"
                "  subject_distance    REAL NOT NULL DEFAULT 0,"
                "  subsec_time_original TEXT NOT NULL DEFAULT '',"
                "  companion_files     TEXT NOT NULL DEFAULT ''"
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
            version = 7;
            db_.setSchemaVersion(version);
            logNotice() << "[PhotoDatabase] Migrated v6 -> v7";
        }

        // v7 -> v8: add face_embedding BLOB to faces + face_scanned to photos
        if (version == 7) {
            if (!db_.exec("ALTER TABLE faces ADD COLUMN face_embedding BLOB DEFAULT NULL") ||
                !db_.exec("ALTER TABLE photos ADD COLUMN face_scanned INTEGER NOT NULL DEFAULT 0")) {
                logError() << "[PhotoDatabase] Migration v7->v8 failed";
                return false;
            }
            version = 8;
            db_.setSchemaVersion(version);
            logNotice() << "[PhotoDatabase] Migrated v7 -> v8";
        }

        // v8 -> v9: add lens correction params + extended EXIF data
        if (version == 8) {
            const char* alters[] = {
                "ALTER TABLE photos ADD COLUMN lens_correction_params TEXT NOT NULL DEFAULT ''",
                "ALTER TABLE photos ADD COLUMN exposure_time TEXT NOT NULL DEFAULT ''",
                "ALTER TABLE photos ADD COLUMN exposure_bias REAL NOT NULL DEFAULT 0",
                "ALTER TABLE photos ADD COLUMN orientation INTEGER NOT NULL DEFAULT 1",
                "ALTER TABLE photos ADD COLUMN white_balance TEXT NOT NULL DEFAULT ''",
                "ALTER TABLE photos ADD COLUMN focal_length_35mm INTEGER NOT NULL DEFAULT 0",
                "ALTER TABLE photos ADD COLUMN offset_time TEXT NOT NULL DEFAULT ''",
                "ALTER TABLE photos ADD COLUMN body_serial TEXT NOT NULL DEFAULT ''",
                "ALTER TABLE photos ADD COLUMN lens_serial TEXT NOT NULL DEFAULT ''",
                "ALTER TABLE photos ADD COLUMN subject_distance REAL NOT NULL DEFAULT 0",
                "ALTER TABLE photos ADD COLUMN subsec_time_original TEXT NOT NULL DEFAULT ''",
                "ALTER TABLE photos ADD COLUMN companion_files TEXT NOT NULL DEFAULT ''",
            };
            for (const auto& sql : alters) {
                if (!db_.exec(sql)) {
                    logError() << "[PhotoDatabase] Migration v8->v9 failed: " << sql;
                    return false;
                }
            }
            db_.setSchemaVersion(SCHEMA_VERSION);
            logNotice() << "[PhotoDatabase] Migrated v8 -> v" << SCHEMA_VERSION;
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
            "latitude, longitude, altitude, develop_settings, is_managed, face_scanned, "
            "lens_correction_params, exposure_time, exposure_bias, orientation, white_balance, "
            "focal_length_35mm, offset_time, body_serial, lens_serial, subject_distance, "
            "subsec_time_original, companion_files) "
            "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,"
            "?20,?21,?22,?23,?24,?25,?26,?27,?28,?29,?30,?31,?32,?33,?34,?35,?36,"
            "?37,?38,?39,?40,?41,?42,?43,?44,?45,?46,?47,?48)"
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

    bool updateFaceScanned(const string& id, bool scanned) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare("UPDATE photos SET face_scanned=?1 WHERE id=?2");
        if (!stmt.valid()) return false;
        stmt.bind(1, scanned ? 1 : 0);
        stmt.bind(2, id);
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

    // Bulk update extended EXIF data (for backfill)
    bool updateExifData(const PhotoEntry& e) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare(
            "UPDATE photos SET "
            "lens_correction_params=?1, exposure_time=?2, exposure_bias=?3, "
            "orientation=?4, white_balance=?5, focal_length_35mm=?6, "
            "offset_time=?7, body_serial=?8, lens_serial=?9, "
            "subject_distance=?10, subsec_time_original=?11, companion_files=?12 "
            "WHERE id=?13");
        if (!stmt.valid()) return false;
        stmt.bind(1, e.lensCorrectionParams);
        stmt.bind(2, e.exposureTime);
        stmt.bind(3, (double)e.exposureBias);
        stmt.bind(4, e.orientation);
        stmt.bind(5, e.whiteBalance);
        stmt.bind(6, e.focalLength35mm);
        stmt.bind(7, e.offsetTime);
        stmt.bind(8, e.bodySerial);
        stmt.bind(9, e.lensSerial);
        stmt.bind(10, (double)e.subjectDistance);
        stmt.bind(11, e.subsecTimeOriginal);
        stmt.bind(12, e.companionFiles);
        stmt.bind(13, e.id);
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
            "latitude, longitude, altitude, develop_settings, is_managed, face_scanned, "
            "lens_correction_params, exposure_time, exposure_bias, orientation, white_balance, "
            "focal_length_35mm, offset_time, body_serial, lens_serial, subject_distance, "
            "subsec_time_original, companion_files) "
            "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,"
            "?20,?21,?22,?23,?24,?25,?26,?27,?28,?29,?30,?31,?32,?33,?34,?35,?36,"
            "?37,?38,?39,?40,?41,?42,?43,?44,?45,?46,?47,?48)"
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
            "latitude, longitude, altitude, develop_settings, is_managed, face_scanned, "
            "lens_correction_params, exposure_time, exposure_bias, orientation, white_balance, "
            "focal_length_35mm, offset_time, body_serial, lens_serial, subject_distance, "
            "subsec_time_original, companion_files "
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
            e.faceScanned        = stmt.getInt(35) != 0;
            e.lensCorrectionParams = stmt.getText(36);
            e.exposureTime       = stmt.getText(37);
            e.exposureBias       = (float)stmt.getDouble(38);
            e.orientation        = stmt.getInt(39);
            e.whiteBalance       = stmt.getText(40);
            e.focalLength35mm    = stmt.getInt(41);
            e.offsetTime         = stmt.getText(42);
            e.bodySerial         = stmt.getText(43);
            e.lensSerial         = stmt.getText(44);
            e.subjectDistance     = (float)stmt.getDouble(45);
            e.subsecTimeOriginal = stmt.getText(46);
            e.companionFiles     = stmt.getText(47);

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
        vector<float> embedding;  // face embedding (512D from ArcFace)
    };

    int insertFaces(const vector<FaceRow>& faces) {
        if (faces.empty()) return 0;

        lock_guard<mutex> lock(db_.writeMutex());
        db_.beginTransaction();

        auto stmt = db_.prepare(
            "INSERT INTO faces (photo_id, person_id, x, y, w, h, source, lr_cluster_id, face_embedding, created_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)");
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
            if (!f.embedding.empty()) {
                stmt.bindBlob(9, f.embedding.data(), (int)(f.embedding.size() * sizeof(float)));
            } else {
                stmt.bindNull(9);
            }
            stmt.bind(10, now);
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

    // Load all face embeddings (face DB id → embedding vector)
    unordered_map<int, vector<float>> loadFaceEmbeddings() {
        unordered_map<int, vector<float>> result;
        auto stmt = db_.prepare(
            "SELECT id, face_embedding FROM faces WHERE face_embedding IS NOT NULL");
        if (!stmt.valid()) return result;
        while (stmt.step()) {
            int faceId = stmt.getInt(0);
            auto [data, size] = stmt.getBlob(1);
            if (data && size > 0) {
                int count = size / (int)sizeof(float);
                vector<float> vec(count);
                memcpy(vec.data(), data, size);
                result[faceId] = std::move(vec);
            }
        }
        return result;
    }

    // Update face embedding by face DB id
    bool updateFaceEmbedding(int faceId, const vector<float>& embedding) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare("UPDATE faces SET face_embedding=?1 WHERE id=?2");
        if (!stmt.valid()) return false;
        stmt.bindBlob(1, embedding.data(), (int)(embedding.size() * sizeof(float)));
        stmt.bind(2, faceId);
        return stmt.execute();
    }

    // Update person_id for a face
    bool updateFacePersonId(int faceId, int personId) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare("UPDATE faces SET person_id=?1 WHERE id=?2");
        if (!stmt.valid()) return false;
        if (personId > 0) {
            stmt.bind(1, personId);
        } else {
            stmt.bindNull(1);
        }
        stmt.bind(2, faceId);
        return stmt.execute();
    }

    // Get photo IDs that have faces with a given source
    unordered_set<string> getPhotosWithFaceSource(const string& source) {
        unordered_set<string> result;
        auto stmt = db_.prepare(
            "SELECT DISTINCT photo_id FROM faces WHERE source=?1");
        if (!stmt.valid()) return result;
        stmt.bind(1, source);
        while (stmt.step()) {
            result.insert(stmt.getText(0));
        }
        return result;
    }

    // Delete faces for a photo with a given source
    bool deleteFacesForPhoto(const string& photoId, const string& source) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare("DELETE FROM faces WHERE photo_id=?1 AND source=?2");
        if (!stmt.valid()) return false;
        stmt.bind(1, photoId);
        stmt.bind(2, source);
        return stmt.execute();
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

    // Load all photo_id -> person names mapping (for search)
    unordered_map<string, vector<string>> loadPersonNamesByPhoto() {
        unordered_map<string, vector<string>> result;
        auto stmt = db_.prepare(
            "SELECT f.photo_id, p.name FROM faces f "
            "JOIN persons p ON f.person_id = p.id "
            "ORDER BY f.photo_id");
        if (!stmt.valid()) return result;
        while (stmt.step()) {
            result[stmt.getText(0)].push_back(stmt.getText(1));
        }
        return result;
    }

    // --- Clustering / People view queries ---

    // Face info for clustering (id, photo_id, person_id, bbox, embedding)
    struct FaceInfo {
        int faceId;
        string photoId;
        int personId = 0;  // 0 = unnamed
        float x, y, w, h;
        vector<float> embedding;
    };

    vector<FaceInfo> loadAllFacesWithEmbeddings() {
        vector<FaceInfo> result;
        auto stmt = db_.prepare(
            "SELECT id, photo_id, COALESCE(person_id, 0), x, y, w, h, face_embedding "
            "FROM faces WHERE face_embedding IS NOT NULL");
        if (!stmt.valid()) return result;
        while (stmt.step()) {
            FaceInfo fi;
            fi.faceId = stmt.getInt(0);
            fi.photoId = stmt.getText(1);
            fi.personId = stmt.getInt(2);
            fi.x = (float)stmt.getDouble(3);
            fi.y = (float)stmt.getDouble(4);
            fi.w = (float)stmt.getDouble(5);
            fi.h = (float)stmt.getDouble(6);
            auto [data, size] = stmt.getBlob(7);
            if (data && size > 0) {
                int count = size / (int)sizeof(float);
                fi.embedding.resize(count);
                memcpy(fi.embedding.data(), data, size);
            }
            result.push_back(std::move(fi));
        }
        return result;
    }

    // Batch update: assign person_id to multiple face IDs
    bool batchUpdateFacePersonId(const vector<int>& faceIds, int personId) {
        if (faceIds.empty()) return true;
        lock_guard<mutex> lock(db_.writeMutex());
        db_.beginTransaction();
        auto stmt = db_.prepare("UPDATE faces SET person_id=?1 WHERE id=?2");
        if (!stmt.valid()) { db_.rollback(); return false; }
        for (int fid : faceIds) {
            if (personId > 0) {
                stmt.bind(1, personId);
            } else {
                stmt.bindNull(1);
            }
            stmt.bind(2, fid);
            stmt.execute();
            stmt.reset();
        }
        db_.commit();
        return true;
    }

    // Unassign faces from their person (set person_id = NULL)
    bool unassignFaces(const vector<int>& faceIds) {
        return batchUpdateFacePersonId(faceIds, 0);
    }

    // Merge persons: move all faces from sourcePersonId to targetPersonId, delete source
    bool mergePersons(int targetPersonId, int sourcePersonId) {
        lock_guard<mutex> lock(db_.writeMutex());
        db_.beginTransaction();
        auto upd = db_.prepare("UPDATE faces SET person_id=?1 WHERE person_id=?2");
        if (!upd.valid()) { db_.rollback(); return false; }
        upd.bind(1, targetPersonId);
        upd.bind(2, sourcePersonId);
        if (!upd.execute()) { db_.rollback(); return false; }

        auto del = db_.prepare("DELETE FROM persons WHERE id=?1");
        if (!del.valid()) { db_.rollback(); return false; }
        del.bind(1, sourcePersonId);
        del.execute();
        db_.commit();
        return true;
    }

    // Rename person
    bool renamePerson(int personId, const string& newName) {
        lock_guard<mutex> lock(db_.writeMutex());
        auto stmt = db_.prepare("UPDATE persons SET name=?1 WHERE id=?2");
        if (!stmt.valid()) return false;
        stmt.bind(1, newName);
        stmt.bind(2, personId);
        return stmt.execute();
    }

    // Get or create person by name (returns person_id)
    int getOrCreatePerson(const string& name) {
        // Try to find existing
        auto sel = db_.prepare("SELECT id FROM persons WHERE name=?1");
        if (sel.valid()) {
            sel.bind(1, name);
            if (sel.step()) return sel.getInt(0);
        }
        // Create new
        lock_guard<mutex> lock(db_.writeMutex());
        int64_t now = chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()).count();
        auto ins = db_.prepare("INSERT INTO persons (name, created_at) VALUES (?1, ?2)");
        if (!ins.valid()) return 0;
        ins.bind(1, name);
        ins.bind(2, now);
        if (!ins.execute()) return 0;
        return (int)sqlite3_last_insert_rowid(db_.rawDb());
    }

    // Load person id->name mapping
    unordered_map<int, string> loadPersonIdToName() {
        unordered_map<int, string> result;
        auto stmt = db_.prepare("SELECT id, name FROM persons");
        if (!stmt.valid()) return result;
        while (stmt.step()) {
            result[stmt.getInt(0)] = stmt.getText(1);
        }
        return result;
    }

    // Get photo IDs that contain faces with the given person_id
    vector<string> getPhotoIdsForPerson(int personId) {
        vector<string> result;
        auto stmt = db_.prepare(
            "SELECT DISTINCT photo_id FROM faces WHERE person_id=?1");
        if (!stmt.valid()) return result;
        stmt.bind(1, personId);
        while (stmt.step()) {
            result.push_back(stmt.getText(0));
        }
        return result;
    }

    // Brief face info for gallery display (face ID, photo, bbox)
    struct FaceBrief {
        int faceId;
        string photoId;
        float x, y, w, h;
    };

    vector<FaceBrief> getFaceBriefs(const vector<int>& faceIds) {
        vector<FaceBrief> result;
        if (faceIds.empty()) return result;
        auto stmt = db_.prepare(
            "SELECT id, photo_id, x, y, w, h FROM faces WHERE id=?1");
        if (!stmt.valid()) return result;
        for (int fid : faceIds) {
            stmt.bind(1, fid);
            if (stmt.step()) {
                FaceBrief fb;
                fb.faceId = stmt.getInt(0);
                fb.photoId = stmt.getText(1);
                fb.x = (float)stmt.getDouble(2);
                fb.y = (float)stmt.getDouble(3);
                fb.w = (float)stmt.getDouble(4);
                fb.h = (float)stmt.getDouble(5);
                result.push_back(std::move(fb));
            }
            stmt.reset();
        }
        return result;
    }

    // Get photo IDs for a set of face IDs
    vector<string> getPhotoIdsForFaceIds(const vector<int>& faceIds) {
        if (faceIds.empty()) return {};
        unordered_set<string> idSet;
        auto stmt = db_.prepare("SELECT photo_id FROM faces WHERE id=?1");
        if (!stmt.valid()) return {};
        for (int fid : faceIds) {
            stmt.bind(1, fid);
            if (stmt.step()) {
                idSet.insert(stmt.getText(0));
            }
            stmt.reset();
        }
        return vector<string>(idSet.begin(), idSet.end());
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
            "  face_embedding BLOB DEFAULT NULL,"
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
        stmt.bind(36, e.faceScanned ? 1 : 0);
        stmt.bind(37, e.lensCorrectionParams);
        stmt.bind(38, e.exposureTime);
        stmt.bind(39, (double)e.exposureBias);
        stmt.bind(40, e.orientation);
        stmt.bind(41, e.whiteBalance);
        stmt.bind(42, e.focalLength35mm);
        stmt.bind(43, e.offsetTime);
        stmt.bind(44, e.bodySerial);
        stmt.bind(45, e.lensSerial);
        stmt.bind(46, (double)e.subjectDistance);
        stmt.bind(47, e.subsecTimeOriginal);
        stmt.bind(48, e.companionFiles);
    }
};
