#pragma once

// =============================================================================
// PhotoLibrary.h - Photo data management (path collection, load state)
// =============================================================================

#include <TrussC.h>
#include <filesystem>
#include <unordered_set>
using namespace std;
using namespace tc;

namespace fs = std::filesystem;

// Photo entry (metadata for one image file)
struct PhotoEntry {
    fs::path path;
    bool loaded = false;        // thumbnail loaded
    int width = 0;              // original image width
    int height = 0;             // original image height

    PhotoEntry() = default;
    PhotoEntry(const fs::path& p) : path(p) {}

    string getFileName() const {
        return path.filename().string();
    }

    string getStem() const {
        return path.stem().string();
    }
};

// Photo library - manages collection of photo entries
class PhotoLibrary {
public:
    PhotoLibrary() = default;

    // Scan folder for images
    void scanFolder(const string& folderPath);

    // Clear all entries
    void clear();

    // Get entry count
    size_t getCount() const { return entries_.size(); }

    // Get entry at index
    PhotoEntry& getEntry(size_t index) { return entries_[index]; }
    const PhotoEntry& getEntry(size_t index) const { return entries_[index]; }

    // Get all entries
    vector<PhotoEntry>& getEntries() { return entries_; }
    const vector<PhotoEntry>& getEntries() const { return entries_; }

private:
    vector<PhotoEntry> entries_;

    // Supported extensions
    static const unordered_set<string> supportedExtensions_;

    bool isSupportedImage(const fs::path& path) const;
};
