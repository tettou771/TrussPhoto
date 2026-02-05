#pragma once

// =============================================================================
// Constants.h - Shared constants and type definitions
// =============================================================================

// Thumbnail settings
constexpr int THUMBNAIL_MAX_SIZE = 512;
constexpr int THUMBNAIL_JPEG_QUALITY = 60;  // 0-100, lower = smaller file

// Sync state of a photo
enum class SyncState {
    LocalOnly  = 0,   // Only on local disk (not yet uploaded)
    Syncing    = 1,   // Upload in progress
    Synced     = 2,   // Synced with server
    ServerOnly = 3,   // Only on server (local file missing)
    Missing    = 4    // Registered but file not found
};
