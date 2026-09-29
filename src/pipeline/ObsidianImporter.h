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
#include <cmath>
#include <cctype>
#include <algorithm>
#include <unordered_set>

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
        unordered_set<string> seen;
        vector<string> roots;
        for (const auto& folder : folders) {
            if (folder.empty() || !fs::exists(folder)) continue;
            roots.push_back(folder);
            // Sorted so id-collision suffixes are assigned the same way on
            // every machine that imports the same vault
            vector<fs::path> notes;
            error_code ec;
            for (auto it = fs::recursive_directory_iterator(folder, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                if (it->path().extension() != ".md") continue;
                notes.push_back(it->path());
            }
            sort(notes.begin(), notes.end());
            for (const auto& note : notes) {
                seen.insert(note.string());
                importFile(provider, note, r);
            }
        }
        int missing = provider.markMissingTextEntries(seen, roots);
        logNotice() << "[Obsidian] scanned=" << r.scanned
                    << " added=" << r.added << " updated=" << r.updated
                    << " missing=" << missing;
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

        // Wall clock + UTC offset at the place the memo was written
        string dateTime, offsetTime;
        time_t createdUtc = 0;
        if (!resolveCreated(fm.created, path.stem().string(), dateTime, offsetTime, &createdUtc)) return;
        string tagsJson = tagsToJson(fm.tags);
        double lat = fm.hasLocation ? fm.lat : 0.0;
        double lon = fm.hasLocation ? fm.lon : 0.0;

        PhotoEntry* existing = provider.findTextEntry(path.string(), filename, (int64_t)createdUtc);
        if (existing) {
            if (provider.updateTextEntryContent(*existing, body, tagsJson,
                                                lat, lon, dateTime, offsetTime,
                                                path.string())) {
                r.updated++;
            }
            return;
        }

        PhotoEntry e;
        e.filename = filename;
        e.fileSize = fsize;
        // The catalog-wide filename_size scheme (matches rows already on peers).
        // A same-named note of equal size elsewhere in the vault gets the note's
        // creation instant appended; the sorted scan keeps that deterministic.
        e.id = filename + "_" + to_string(fsize);
        if (provider.getPhoto(e.id)) e.id += "_" + to_string((long long)createdUtc);
        e.entryType = 1;                 // text
        e.isManaged = false;             // external reference (vault owns the file)
        e.localPath = path.string();
        e.memo = body;
        e.memoUpdatedAt = PhotoProvider::nowMs();
        e.tags = tagsJson;
        e.tagsUpdatedAt = PhotoProvider::nowMs();
        e.dateTimeOriginal = dateTime;
        e.offsetTime = offsetTime;
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
        // Obsidian's Properties UI rewrites lists as YAML block lists
        // ("tags:" followed by "  - diary" lines), so accept both forms.
        string listKey;
        vector<string> listItems;
        auto flushList = [&]() {
            if (listKey == "tags") fm.tags = listItems;
            else if (listKey == "location" && listItems.size() >= 2)
                fm.hasLocation = parseLatLon(listItems[0] + "," + listItems[1], fm.lat, fm.lon);
            listKey.clear();
            listItems.clear();
        };
        while (getline(is, line)) {
            string t = trim(line);
            if (!listKey.empty() && !t.empty() && t[0] == '-') {
                listItems.push_back(stripQuotes(trim(t.substr(1))));
                continue;
            }
            flushList();
            size_t colon = line.find(':');
            if (colon == string::npos) continue;
            string key = trim(line.substr(0, colon));
            string val = trim(line.substr(colon + 1));
            if (val.empty() && (key == "tags" || key == "location")) {
                listKey = key;
                continue;
            }
            if (key == "created")       fm.created = stripQuotes(val);
            else if (key == "source")   fm.source = stripQuotes(val);
            else if (key == "location") fm.hasLocation = parseLatLon(val, fm.lat, fm.lon);
            else if (key == "tags")     fm.tags = parseFlowArray(val);
        }
        flushList();
        return true;
    }

    // `created` -> local wall clock "YYYY:MM:DD HH:MM:SS" + UTC offset "+02:00"
    // at the place the memo was written. The instant comes from `created`
    // (UTC "...Z", or an explicit "+02:00" offset). The local offset is taken,
    // in order, from: an explicit offset in `created`; the QuickMemo filename,
    // which records the local wall clock ("20260911_184009", "20260406_0007");
    // the machine's own zone at that instant.
    static bool resolveCreated(const string& iso, const string& stem,
                               string& outDateTime, string& outOffset,
                               time_t* outUtc = nullptr) {
        time_t utc = 0;
        int isoOffset = 0;
        bool hasIsoOffset = false;
        if (!parseIsoInstant(iso, utc, isoOffset, hasIsoOffset)) return false;
        if (outUtc) *outUtc = utc;

        int off = 0;
        if (hasIsoOffset) {
            off = isoOffset;
        } else if (!offsetFromFilename(stem, utc, off)) {
            tm local{};
            localtime_r(&utc, &local);
            off = (int)local.tm_gmtoff;
        }

        time_t wall = utc + off;
        tm w{};
        gmtime_r(&wall, &w);
        char buf[20];
        strftime(buf, sizeof(buf), "%Y:%m:%d %H:%M:%S", &w);
        outDateTime = buf;

        int a = off < 0 ? -off : off;
        char obuf[8];
        snprintf(obuf, sizeof(obuf), "%c%02d:%02d", off < 0 ? '-' : '+', a / 3600, (a % 3600) / 60);
        outOffset = obuf;
        return true;
    }

    // "2026-05-30T01:41:28Z" / "...+02:00" / "...+0200" / no suffix (read as UTC)
    // -> UTC instant, plus the explicit offset when one is written.
    static bool parseIsoInstant(const string& iso, time_t& outUtc,
                                int& outOffset, bool& hasOffset) {
        if (iso.size() < 19) return false;
        tm t = {};
        try {
            t.tm_year = stoi(iso.substr(0, 4)) - 1900;
            t.tm_mon  = stoi(iso.substr(5, 2)) - 1;
            t.tm_mday = stoi(iso.substr(8, 2));
            t.tm_hour = stoi(iso.substr(11, 2));
            t.tm_min  = stoi(iso.substr(14, 2));
            t.tm_sec  = stoi(iso.substr(17, 2));
        } catch (...) {
            return false;
        }
        hasOffset = false;
        outOffset = 0;
        size_t i = 19;
        if (i < iso.size() && iso[i] == '.') {                 // fractional seconds
            while (i + 1 < iso.size() && isdigit((unsigned char)iso[i + 1])) i++;
            i++;
        }
        if (i < iso.size() && (iso[i] == '+' || iso[i] == '-')) {
            string z = iso.substr(i + 1);
            z.erase(remove(z.begin(), z.end(), ':'), z.end());
            if (z.size() >= 4 && all_of(z.begin(), z.begin() + 4,
                                        [](char c) { return isdigit((unsigned char)c); })) {
                int sec = stoi(z.substr(0, 2)) * 3600 + stoi(z.substr(2, 2)) * 60;
                outOffset = iso[i] == '-' ? -sec : sec;
                hasOffset = true;
            }
        }
        // Fields are the wall clock at outOffset (0 for "Z" / no suffix)
        outUtc = timegm(&t) - outOffset;
        return true;
    }

    // QuickMemo names files by local wall clock: "YYYYMMDD_HHMM[SS]...".
    // The name is stamped when recording starts and `created` when the note is
    // saved (after transcription), so name - created(UTC) = offset - delay with
    // delay >= 0. This machine's own zone wins when it explains the name with a
    // delay of up to 45 min (home, long transcriptions); otherwise take the
    // smallest half-hour offset covering the difference (2 min slack for clock
    // skew). Zones on a 45-minute offset (Nepal, Chatham) need an explicit
    // offset in `created`.
    static bool offsetFromFilename(const string& stem, time_t utc, int& outOffset) {
        auto digits = [&](size_t from, size_t n) {
            if (stem.size() < from + n) return false;
            for (size_t k = from; k < from + n; k++)
                if (!isdigit((unsigned char)stem[k])) return false;
            return true;
        };
        if (!digits(0, 8) || stem.size() < 13 || stem[8] != '_' || !digits(9, 4)) return false;
        tm t = {};
        t.tm_year = stoi(stem.substr(0, 4)) - 1900;
        t.tm_mon  = stoi(stem.substr(4, 2)) - 1;
        t.tm_mday = stoi(stem.substr(6, 2));
        t.tm_hour = stoi(stem.substr(9, 2));
        t.tm_min  = stoi(stem.substr(11, 2));
        t.tm_sec  = digits(13, 2) ? stoi(stem.substr(13, 2)) : 0;
        long diff = (long)(timegm(&t) - utc);
        tm local{};
        localtime_r(&utc, &local);
        long machine = local.tm_gmtoff;
        if (machine - diff >= -120 && machine - diff <= 45 * 60) {
            outOffset = (int)machine;
            return true;
        }
        long off = (long)ceil((diff - 120) / 1800.0) * 1800;
        if (labs(off) > 14 * 3600) return false;
        outOffset = (int)off;
        return true;
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
