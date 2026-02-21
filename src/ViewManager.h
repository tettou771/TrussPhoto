#pragma once

// =============================================================================
// ViewManager.h - Central view transition manager
// Owns all views as child nodes and manages switchTo/goBack transitions.
// =============================================================================

#include <TrussC.h>
#include "ViewContainer.h"
#include "ViewContext.h"
#include "GridView.h"
#include "SingleView.h"
#include "MapView.h"
#include "RelatedView.h"
#include "PeopleView.h"
#include "CropView.h"

using namespace std;
using namespace tc;

class ViewManager : public RectNode {
public:
    using Ptr = shared_ptr<ViewManager>;

    ViewManager()
        : gridView_(make_shared<GridView>())
        , singleView_(make_shared<SingleView>())
        , mapView_(make_shared<MapView>())
        , relatedView_(make_shared<RelatedView>())
        , peopleView_(make_shared<PeopleView>())
        , cropView_(make_shared<CropView>())
    {
        cropView_->setSingleView(singleView_);
    }

    ViewMode activeView() const { return active_; }
    ViewMode previousView() const { return previous_; }

    void setContext(ViewContext ctx) { ctx_ = ctx; }

    // Core transition: switch to target view mode
    void switchTo(ViewMode target) {
        if (target == active_) return;
        previous_ = active_;

        // Deactivate current view
        auto* prev = containerFor(active_);
        if (prev) {
            // People/Single use suspend (preserves state), others use endView
            if (active_ == ViewMode::People) prev->suspendView();
            else if (active_ == ViewMode::Single && target == ViewMode::Crop) prev->suspendView();
            else prev->endView();
            static_cast<RectNode*>(prev)->setActive(false);
        }

        // Activate target view
        active_ = target;
        auto* next = containerFor(active_);
        if (next) {
            // If it has preserved state, just reactivate without beginView
            if (!next->hasState()) {
                next->beginView(ctx_);
            }
            static_cast<RectNode*>(next)->setActive(true);
            // Sync size with ViewManager
            static_cast<RectNode*>(next)->setRect(0, 0, getWidth(), getHeight());
        }

        if (ctx_.redraw) ctx_.redraw(1);
    }

    // ESC: go back to previous view (with smart restore)
    void goBack() {
        if (active_ == ViewMode::Single && previous_ == ViewMode::People) {
            switchTo(ViewMode::People);
        } else if (active_ != ViewMode::Grid) {
            switchTo(ViewMode::Grid);
        }
    }

    // Shortcut: open single image view
    void showFullImage(int index) {
        if (!singleView_) return;

        // Prepare context
        singleView_->beginView(ctx_);
        singleView_->show(index);

        if (singleView_->selectedIndex() < 0) {
            singleView_->endView();
            return;
        }

        // Deactivate current view
        auto* prev = containerFor(active_);
        if (prev) {
            if (active_ == ViewMode::People) prev->suspendView();
            else prev->endView();
            static_cast<RectNode*>(prev)->setActive(false);
        }

        previous_ = active_;
        active_ = ViewMode::Single;
        singleView_->setActive(true);
        singleView_->setRect(0, 0, getWidth(), getHeight());

        if (ctx_.redraw) ctx_.redraw(1);
    }

    // View accessors
    GridView::Ptr gridView() { return gridView_; }
    SingleView::Ptr singleView() { return singleView_; }
    MapView::Ptr mapView() { return mapView_; }
    RelatedView::Ptr relatedView() { return relatedView_; }
    PeopleView::Ptr peopleView() { return peopleView_; }
    CropView::Ptr cropView() { return cropView_; }

    void setup() override {
        addChild(gridView_);

        addChild(singleView_);
        singleView_->setActive(false);

        addChild(mapView_);
        mapView_->setActive(false);

        addChild(relatedView_);
        relatedView_->setActive(false);

        addChild(peopleView_);
        peopleView_->setActive(false);

        addChild(cropView_);
        cropView_->setActive(false);
    }

    void draw() override {
        // Transparent: children draw themselves
    }

    // Layout: propagate size to active view
    void layoutViews() {
        float w = getWidth();
        float h = getHeight();
        auto* active = containerFor(active_);
        if (active) {
            static_cast<RectNode*>(active)->setRect(0, 0, w, h);
        }
        // Grid always needs correct size for scroll calculations
        if (gridView_) gridView_->setRect(0, 0, w, h);
    }

    // Shutdown all views (call from tcApp::exit)
    void shutdownAll() {
        if (singleView_) singleView_->joinRawLoadThread();
        if (mapView_) mapView_->shutdown();
        if (relatedView_) relatedView_->shutdown();
        if (peopleView_) peopleView_->shutdown();
    }

private:
    ViewMode active_ = ViewMode::Grid;
    ViewMode previous_ = ViewMode::Grid;
    ViewContext ctx_;

    GridView::Ptr gridView_;
    SingleView::Ptr singleView_;
    MapView::Ptr mapView_;
    RelatedView::Ptr relatedView_;
    PeopleView::Ptr peopleView_;
    CropView::Ptr cropView_;

    ViewContainer* containerFor(ViewMode mode) {
        switch (mode) {
            case ViewMode::Grid:    return gridView_.get();
            case ViewMode::Single:  return singleView_.get();
            case ViewMode::Map:     return mapView_.get();
            case ViewMode::Related: return relatedView_.get();
            case ViewMode::People:  return peopleView_.get();
            case ViewMode::Crop:    return cropView_.get();
        }
        return nullptr;
    }
};
