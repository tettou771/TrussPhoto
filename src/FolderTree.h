#pragma once

// =============================================================================
// FolderTree.h - Folder tree sidebar UI component
// =============================================================================

#include <TrussC.h>
#include "PhotoProvider.h"
using namespace std;
using namespace tc;

// =============================================================================
// FolderRow - Individual row in the folder tree (self-contained RectNode)
// =============================================================================
class FolderRow : public RectNode {
public:
    using Ptr = shared_ptr<FolderRow>;

    string path;
    string displayName;
    int totalCount = 0;
    int depth = 0;
    bool expanded = true;
    bool exists = true;
    bool hasChildren = false;
    bool isHeader = false;
    bool selected = false;

    function<void()> onClick;
    function<void()> onToggle;

    FolderRow() {
        enableEvents();
    }

    void setup() override {
        setClipping(true);
    }

    void draw() override {
        float w = getWidth();

        if (isHeader) {
            if (selected) {
                setColor(0.2f, 0.25f, 0.35f);
                fill();
                drawRect(0, 0, w, getHeight());
            }
            setColor(selected ? Color(0.9f, 0.9f, 0.95f) : Color(0.65f, 0.65f, 0.7f));
            drawBitmapString("All Photos", 14, 10);

            setColor(0.45f, 0.45f, 0.5f);
            string cnt = to_string(totalCount);
            drawBitmapString(cnt, w - (float)cnt.length() * 8 - 20, 10);
            return;
        }

        float indent = 16.0f * depth + 14.0f;

        // Selection highlight
        if (selected) {
            setColor(0.2f, 0.25f, 0.35f);
            fill();
            drawRect(0, 0, w, getHeight());
        }

        // Toggle icon
        if (hasChildren) {
            setColor(0.5f, 0.5f, 0.55f);
            drawBitmapString(expanded ? "v" : ">", indent - 12, 7);
        }

        // Folder name
        if (!exists) {
            setColor(0.6f, 0.25f, 0.25f);
        } else if (selected) {
            setColor(0.9f, 0.9f, 0.95f);
        } else {
            setColor(0.7f, 0.7f, 0.75f);
        }

        string name = displayName;
        float maxNameW = w - indent - 50;
        if ((float)name.length() * 8 > maxNameW && name.length() > 10) {
            int maxChars = max(5, (int)(maxNameW / 8));
            name = name.substr(0, maxChars - 3) + "...";
        }
        drawBitmapString(name, indent, 7);

        // Photo count
        if (totalCount > 0) {
            setColor(0.45f, 0.45f, 0.5f);
            string cnt = to_string(totalCount);
            drawBitmapString(cnt, w - (float)cnt.length() * 8 - 20, 7);
        }
    }

protected:
    bool onMousePress(Vec2 local, int button) override {
        if (button != 0) return false;

        // Toggle expand/collapse icon area
        if (!isHeader && hasChildren) {
            float indent = 16.0f * depth + 14.0f;
            if (local.x >= indent - 16 && local.x < indent) {
                if (onToggle) onToggle();
                return true;
            }
        }

        if (onClick) onClick();
        return true;
    }
};

// ScrollContainer without default background/border (parent draws its own)
#ifndef PLAIN_SCROLL_CONTAINER_DEFINED
#define PLAIN_SCROLL_CONTAINER_DEFINED
class PlainScrollContainer : public ScrollContainer {
public:
    using Ptr = shared_ptr<PlainScrollContainer>;
    void draw() override {}
};
#endif

// =============================================================================
// FolderTree - Scrollable folder tree sidebar
// =============================================================================
class FolderTree : public RectNode {
public:
    using Ptr = shared_ptr<FolderTree>;

    // Callback when a folder is selected (empty path = show all)
    function<void(const string& folderPath)> onFolderSelected;

    FolderTree() {
        // Don't addChild(scrollContainer_) to this here —
        // weak_from_this() returns empty in the constructor.
        scrollContainer_ = make_shared<PlainScrollContainer>();
        content_ = make_shared<RectNode>();
        scrollContainer_->setContent(content_);

        scrollBar_ = make_shared<ScrollBar>(scrollContainer_.get(), ScrollBar::Vertical);
        scrollContainer_->addChild(scrollBar_);
    }

    void setup() override {
        addChild(scrollContainer_);
    }

    void buildTree(const vector<PhotoProvider::FolderInfo>& folders, const string& rootPath) {
        rootPath_ = rootPath;
        nodes_.clear();
        rootNodes_.clear();

        if (folders.empty()) {
            totalPhotoCount_ = 0;
            rebuildRows();
            return;
        }

        for (const auto& folder : folders) {
            FolderNode node;
            node.path = folder.path;
            node.displayName = folder.displayName;
            node.photoCount = folder.photoCount;
            node.exists = folder.exists;
            nodes_.push_back(std::move(node));
        }

        buildHierarchy();

        totalPhotoCount_ = 0;
        for (auto* root : rootNodes_) {
            totalPhotoCount_ += computeTotalCount(root);
        }

        rebuildRows();
        redraw();
    }

    const string& getSelectedPath() const { return selectedPath_; }

    void clearSelection() {
        selectedPath_.clear();
        if (onFolderSelected) onFolderSelected("");
        updateSelection();
    }

    void setSize(float w, float h) override {
        RectNode::setSize(w, h);
        scrollContainer_->setRect(0, 0, w, h);
        rebuildRows();
    }

    // Background + right border
    void draw() override {
        setColor(0.09f, 0.09f, 0.11f);
        fill();
        drawRect(0, 0, getWidth(), getHeight());

        setColor(0.2f, 0.2f, 0.22f);
        noFill();
        drawLine(getWidth() - 1, 0, getWidth() - 1, getHeight());
    }

    void update() override {
        if (needsRebuildRows_) {
            needsRebuildRows_ = false;
            rebuildRows();
            redraw();
        }
        scrollContainer_->updateScrollBounds();
        scrollBar_->updateFromContainer();
    }

private:
    struct FolderNode {
        string path;
        string displayName;
        int photoCount = 0;
        int totalCount = 0;
        int depth = 0;
        bool expanded = true;
        bool exists = true;
        FolderNode* parent = nullptr;
        vector<FolderNode*> children;
    };

    ScrollContainer::Ptr scrollContainer_;
    RectNode::Ptr content_;
    ScrollBar::Ptr scrollBar_;
    vector<FolderRow::Ptr> rows_;

    vector<FolderNode> nodes_;
    vector<FolderNode*> rootNodes_;
    string rootPath_;
    string selectedPath_;
    int totalPhotoCount_ = 0;
    bool needsRebuildRows_ = false;

    float rowHeight_ = 24.0f;
    float headerHeight_ = 30.0f;

    // --- Tree data helpers ---

    bool isNodeVisible(const FolderNode& node) const {
        const FolderNode* p = node.parent;
        while (p) {
            if (!p->expanded) return false;
            p = p->parent;
        }
        return true;
    }

    void buildHierarchy() {
        rootNodes_.clear();
        for (auto& node : nodes_) {
            node.parent = nullptr;
            node.children.clear();
        }

        for (size_t i = 0; i < nodes_.size(); i++) {
            FolderNode* bestParent = nullptr;
            size_t bestLen = 0;

            for (size_t j = 0; j < nodes_.size(); j++) {
                if (i == j) continue;
                const string& pp = nodes_[j].path;
                const string& cp = nodes_[i].path;

                if (cp.size() > pp.size() &&
                    cp.substr(0, pp.size()) == pp &&
                    cp[pp.size()] == '/' &&
                    pp.size() > bestLen) {
                    bestParent = &nodes_[j];
                    bestLen = pp.size();
                }
            }

            if (bestParent) {
                nodes_[i].parent = bestParent;
                bestParent->children.push_back(&nodes_[i]);
            } else {
                rootNodes_.push_back(&nodes_[i]);
            }
        }

        for (auto* root : rootNodes_) {
            setDepthRecursive(root, 0);
        }
    }

    void setDepthRecursive(FolderNode* node, int depth) {
        node->depth = depth;
        for (auto* child : node->children) {
            setDepthRecursive(child, depth + 1);
        }
    }

    int computeTotalCount(FolderNode* node) {
        node->totalCount = node->photoCount;
        for (auto* child : node->children) {
            node->totalCount += computeTotalCount(child);
        }
        return node->totalCount;
    }

    // --- Row management ---

    void rebuildRows() {
        content_->removeAllChildren();
        rows_.clear();

        float w = getWidth() - 12;  // scrollbar space
        if (w <= 0) return;

        float y = 0;

        // "All Photos" header
        auto header = make_shared<FolderRow>();
        header->isHeader = true;
        header->totalCount = totalPhotoCount_;
        header->selected = selectedPath_.empty();
        header->setRect(0, y, w, headerHeight_);
        header->onClick = [this]() {
            selectedPath_.clear();
            if (onFolderSelected) onFolderSelected("");
            updateSelection();
        };
        rows_.push_back(header);
        content_->addChild(header);
        y += headerHeight_;

        // Folder rows
        for (auto& node : nodes_) {
            if (!isNodeVisible(node)) continue;

            auto row = make_shared<FolderRow>();
            row->path = node.path;
            row->displayName = node.displayName;
            row->totalCount = node.totalCount;
            row->depth = node.depth;
            row->expanded = node.expanded;
            row->exists = node.exists;
            row->hasChildren = !node.children.empty();
            row->selected = (selectedPath_ == node.path);
            row->setRect(0, y, w, rowHeight_);

            FolderNode* nodePtr = &node;
            row->onClick = [this, nodePtr]() {
                if (selectedPath_ == nodePtr->path) {
                    selectedPath_.clear();
                    if (onFolderSelected) onFolderSelected("");
                } else {
                    selectedPath_ = nodePtr->path;
                    if (onFolderSelected) onFolderSelected(nodePtr->path);
                }
                updateSelection();
            };

            // Deferred rebuild — avoids destroying self during callback
            row->onToggle = [this, nodePtr]() {
                nodePtr->expanded = !nodePtr->expanded;
                needsRebuildRows_ = true;
                redraw();
            };

            rows_.push_back(row);
            content_->addChild(row);
            y += rowHeight_;
        }

        content_->setSize(w, y + 10);
        scrollContainer_->updateScrollBounds();
    }

    void updateSelection() {
        for (auto& row : rows_) {
            if (row->isHeader) {
                row->selected = selectedPath_.empty();
            } else {
                row->selected = (selectedPath_ == row->path);
            }
        }
        redraw();
    }
};
