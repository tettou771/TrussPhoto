#pragma once

// =============================================================================
// GridView.h - ViewContainer wrapper for PhotoGrid
// =============================================================================

#include "ViewContainer.h"
#include "ui/PhotoGrid.h"

using namespace std;
using namespace tc;

class GridView : public ViewContainer {
public:
    using Ptr = shared_ptr<GridView>;

    GridView() : grid_(make_shared<PhotoGrid>()) {}

    void setup() override {
        addChild(grid_);
        grid_->setRect(0, 0, getWidth(), getHeight());
    }

    void update() override {
        if (grid_->getWidth() != getWidth() || grid_->getHeight() != getHeight()) {
            grid_->setRect(0, 0, getWidth(), getHeight());
        }
    }

    void beginView(ViewContext& ctx) override {
        ctx_ = &ctx;
        grid_->setActive(true);
    }

    void endView() override {
        grid_->setActive(false);
        ctx_ = nullptr;
    }

    bool wantsSearchBar() const override { return true; }
    bool wantsLeftSidebar() const override { return true; }

    PhotoGrid::Ptr grid() { return grid_; }

private:
    ViewContext* ctx_ = nullptr;
    PhotoGrid::Ptr grid_;
};
