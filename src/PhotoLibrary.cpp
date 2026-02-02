#include "PhotoLibrary.h"
#include <algorithm>

const unordered_set<string> PhotoLibrary::supportedExtensions_ = {
    ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".tga", ".psd", ".hdr"
};

bool PhotoLibrary::isSupportedImage(const fs::path& path) const {
    if (!fs::is_regular_file(path)) return false;

    string ext = path.extension().string();
    // Convert to lowercase
    transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    return supportedExtensions_.count(ext) > 0;
}

void PhotoLibrary::scanFolder(const string& folderPath) {
    fs::path folder(folderPath);

    if (!fs::exists(folder) || !fs::is_directory(folder)) {
        logWarning() << "[PhotoLibrary] Not a valid directory: " << folderPath;
        return;
    }

    logNotice() << "[PhotoLibrary] Scanning folder: " << folderPath;

    // Collect all image files
    vector<fs::path> imagePaths;

    for (const auto& entry : fs::directory_iterator(folder)) {
        if (isSupportedImage(entry.path())) {
            imagePaths.push_back(entry.path());
        }
    }

    // Sort by filename
    sort(imagePaths.begin(), imagePaths.end());

    // Create photo entries
    entries_.clear();
    entries_.reserve(imagePaths.size());

    for (const auto& path : imagePaths) {
        entries_.emplace_back(path);
    }

    logNotice() << "[PhotoLibrary] Found " << entries_.size() << " images";
}

void PhotoLibrary::clear() {
    entries_.clear();
}
