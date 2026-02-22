#pragma once

// =============================================================================
// ViewContainer.h - Base class for all view modes
// =============================================================================

#include <TrussC.h>
#include "ViewContext.h"

using namespace std;
using namespace tc;

// View mode enum (shared across views)
enum class ViewMode {
    Grid,
    Single,
    Map,
    Related,
    People,
    Crop
};

class ViewContainer : public RectNode {
public:
    using Ptr = shared_ptr<ViewContainer>;

    // View lifecycle
    virtual void beginView(ViewContext& ctx) = 0;   // called when becoming active
    virtual void endView() = 0;                      // full cleanup
    virtual void suspendView() { endView(); }        // temporary deactivation (default = endView)
    virtual bool hasState() const { return false; }  // can restore after suspend?

    // Layout hints for tcApp
    virtual bool wantsSearchBar() const { return false; }
    virtual bool wantsLeftSidebar() const { return false; }
};
