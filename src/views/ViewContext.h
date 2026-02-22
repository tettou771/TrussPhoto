#pragma once

// =============================================================================
// ViewContext.h - Shared state between ViewManager and views
// =============================================================================

#include <TrussC.h>
#include <functional>
#include <memory>

using namespace std;
using namespace tc;

class PhotoProvider;
class PhotoGrid;
class MetadataPanel;

struct ViewContext {
    PhotoProvider* provider = nullptr;
    shared_ptr<PhotoGrid> grid;
    shared_ptr<MetadataPanel> metadataPanel;
    bool* cmdDown = nullptr;
    bool* shiftDown = nullptr;
    function<void(int)> redraw;
};
