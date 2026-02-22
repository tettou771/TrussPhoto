#pragma once

// =============================================================================
// Collection.h - Collection data model (manual, smart, group)
// =============================================================================

#include <string>
using namespace std;

struct Collection {
    int id = 0;
    string name;
    int parentId = 0;         // 0 = root level
    enum Type { Regular = 0, Smart = 1, Group = 2 };
    Type type = Regular;
    string rules;             // JSON (smart collection criteria, empty for regular)
    string sortType;
    string sortDirection;
    int64_t createdAt = 0;
    int photoCount = 0;       // runtime cache (populated by loadCollections)
};
