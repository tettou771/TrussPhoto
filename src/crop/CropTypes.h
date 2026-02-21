#pragma once

// =============================================================================
// CropTypes.h - Shared type definitions for crop module
// =============================================================================

enum class CropAspect { Original, A16_9, A4_3, A3_2, A1_1, A5_4, Free };
constexpr int kCropAspectCount = 7;

inline const char* cropAspectLabel(CropAspect a) {
    constexpr const char* labels[] = {
        "Original", "16:9", "4:3", "3:2", "1:1", "5:4", "Free"
    };
    return labels[(int)a];
}
