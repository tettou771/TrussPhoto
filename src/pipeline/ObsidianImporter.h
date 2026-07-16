#pragma once

// =============================================================================
// ObsidianImporter.h - Import Obsidian QuickMemo notes as text catalog entries
// =============================================================================
// Reads *.md notes (recursively) from one or more vault folders. Notes with YAML
// frontmatter carrying a `created` timestamp become first-class text entries
// (entryType == 1). The body (frontmatter stripped, `#photo-memo` line kept) is
// stored in PhotoEntry::memo. Import is idempotent: existing entries are matched
// by filename (Obsidian edits change the file size, and the id embeds the size),
// and only content changes bump updatedAt.
//
// Frontmatter shape (QuickMemo):
//   ---
//   tags: [photo-memo]
//   created: 2026-05-30T01:41:28Z
//   source: apple-watch
//   location: [35.687608, 139.835885]
//   ---
//   #photo-memo body text...
//
// The vault is treated as READ-ONLY: we never write next to the notes.

#include "../PhotoProvider.h"
#include "../PhotoEntry.h"
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <ctime>

using namespace std;
namespace fs = std::filesystem;

class ObsidianImporter {
public:
    struct Result {
        int scanned = 0;   // notes with valid frontmatter examined
        int added = 0;     // new text entries created
        int updated = 0;   // existing entries whose content changed
    };

    // Import every *.md note under each folder. Returns aggregate counts.
    static Result importPaths(PhotoProvider& provider, const vector<string>& folders) {
        Result r;
        for (const auto& folder : folders) {
            if (folder.empty() || !fs::exists(folder)) continue;
            error_code ec;
            for (auto it = fs::recursive_directory_iterator(folder, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                if (it->path().extension() != ".md") continue;
                importFile(provider, it->path(), r);
            }
        }
        logNotice() << "[Obsidian] scanned=" << r.scanned
                    << " added=" << r.added << " updated=" << r.updated;
        return r;
    }

private:
    struct Frontmatter {
        string created;
        string source;
        double lat = 0, lon = 0;
        vector<string> tags;
        bool hasLocation = false;
    };

    static void importFile(PhotoProvider& provider, const fs::path& path, Result& r) {
        ifstream f(path, ios::binary);
        if (!f) return;
        stringstream ss;
        ss << f.rdbuf();
        string content = ss.str();

        Frontmatter fm;
        string body;
        if (!parse(content, fm, body)) return;   // no frontmatter -> skip
        if (fm.created.empty()) return;           // not a dated memo -> skip
        r.scanned++;

        string filename = path.filename().string();
        error_code ec;
        uintmax_t fsize = fs::file_size(path, ec);
        if (ec) fsize = 0;

        string dateTime = isoUtcToLocal(fm.created);
        string tagsJson = tagsToJson(fm.tags);
        double lat = fm.hasLocation ? fm.lat : 0.0;
        double lon = fm.hasLocation ? fm.lon : 0.0;

        PhotoEntry* existing = provider.findTextEntryByFilename(filename);
        if (existing) {
            if (provider.updateTextEntryContent(*existing, body, tagsJson,
                                                lat, lon, dateTime, path.string())) {
                r.updated++;
            }
            return;
        }

        PhotoEntry e;
        e.filename = filename;
        e.fileSize = fsize;
        e.id = filename + "_" + to_string(fsize);
        e.entryType = 1;                 // text
        e.isManaged = false;             // external reference (vault owns the file)
        e.localPath = path.string();
        e.memo = body;
        e.memoUpdatedAt = PhotoProvider::nowMs();
        e.tags = tagsJson;
        e.tagsUpdatedAt = PhotoProvider::nowMs();
        e.dateTimeOriginal = dateTime;
        e.latitude = lat;
        e.longitude = lon;
        e.syncState = SyncState::LocalOnly;
        if (provider.addTextEntry(e)) r.added++;
    }

    // Split YAML frontmatter (--- ... ---) from body. Returns false if none.
    static bool parse(const string& content, Frontmatter& fm, string& body) {
        size_t p = 0;
        // Skip UTF-8 BOM
        if (content.size() >= 3 &&
            (unsigned char)content[0] == 0xEF &&
            (unsigned char)content[1] == 0xBB &&
            (unsigned char)content[2] == 0xBF) {
            p = 3;
        }
        if (content.compare(p, 3, "---") != 0) return false;
        size_t firstLineEnd = content.find('\n', p);
        if (firstLineEnd == string::npos) return false;
        size_t fmStart = firstLineEnd + 1;

        // Closing delimiter: a line that is exactly '---'
        size_t close = content.find("\n---", fmStart);
        if (close == string::npos) return false;
        string fmBlock = content.substr(fmStart, close - fmStart);

        // Body begins after the newline that ends the closing '---' line
        size_t bodyStart = content.find('\n', close + 1);
        body = (bodyStart == string::npos) ? "" : content.substr(bodyStart + 1);
        body = ltrimNewlines(body);

        istringstream is(fmBlock);
        string line;
        while (getline(is, line)) {
            size_t colon = line.find(':');
            if (colon == string::npos) continue;
            string key = trim(line.substr(0, colon));
            string val = trim(line.substr(colon + 1));
            if (key == "created")       fm.created = stripQuotes(val);
            else if (key == "source")   fm.source = stripQuotes(val);
            else if (key == "location") fm.hasLocation = parseLatLon(val, fm.lat, fm.lon);
            else if (key == "tags")     fm.tags = parseFlowArray(val);
        }
        return true;
    }

    // "2026-05-30T01:41:28Z" (UTC) -> local "YYYY:MM:DD HH:MM:SS"
    static string isoUtcToLocal(const string& iso) {
        if (iso.size() < 19) return "";
        tm t = {};
        try {
            t.tm_year = stoi(iso.substr(0, 4)) - 1900;
            t.tm_mon  = stoi(iso.substr(5, 2)) - 1;
            t.tm_mday = stoi(iso.substr(8, 2));
            t.tm_hour = stoi(iso.substr(11, 2));
            t.tm_min  = stoi(iso.substr(14, 2));
            t.tm_sec  = stoi(iso.substr(17, 2));
        } catch (...) {
            return "";
        }
        time_t utc = timegm(&t);   // interpret fields as UTC
        tm local{};
        localtime_r(&utc, &local);
        char buf[20];
        strftime(buf, sizeof(buf), "%Y:%m:%d %H:%M:%S", &local);
        return string(buf);
    }

    // "[35.687608, 139.835885]" -> lat, lon. Returns true on success.
    static bool parseLatLon(const string& val, double& lat, double& lon) {
        string s = val;
        size_t lb = s.find('['), rb = s.find(']');
        if (lb != string::npos && rb != string::npos && rb > lb) {
            s = s.substr(lb + 1, rb - lb - 1);
        }
        size_t comma = s.find(',');
        if (comma == string::npos) return false;
        try {
            lat = stod(trim(s.substr(0, comma)));
            lon = stod(trim(s.substr(comma + 1)));
        } catch (...) {
            return false;
        }
        return true;
    }

    // "[a, b, c]" -> {"a","b","c"}. Handles YAML flow arrays.
    static vector<string> parseFlowArray(const string& val) {
        vector<string> out;
        string s = val;
        size_t lb = s.find('['), rb = s.find(']');
        if (lb != string::npos && rb != string::npos && rb > lb) {
            s = s.substr(lb + 1, rb - lb - 1);
        }
        stringstream is(s);
        string item;
        while (getline(is, item, ',')) {
            string t = stripQuotes(trim(item));
            if (!t.empty()) out.push_back(t);
        }
        return out;
    }

    // {"a","b"} -> '["a","b"]' (matches PhotoEntry::tags storage format)
    static string tagsToJson(const vector<string>& tags) {
        if (tags.empty()) return "";
        string out = "[";
        for (size_t i = 0; i < tags.size(); i++) {
            if (i) out += ",";
            out += "\"" + jsonEscape(tags[i]) + "\"";
        }
        out += "]";
        return out;
    }

    static string jsonEscape(const string& s) {
        string out;
        for (char c : s) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out;
    }

    static string trim(const string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    static string ltrimNewlines(const string& s) {
        size_t a = s.find_first_not_of("\r\n");
        return a == string::npos ? "" : s.substr(a);
    }

    static string stripQuotes(const string& s) {
        if (s.size() >= 2 &&
            ((s.front() == '"' && s.back() == '"') ||
             (s.front() == '\'' && s.back() == '\''))) {
            return s.substr(1, s.size() - 2);
        }
        return s;
    }
};
