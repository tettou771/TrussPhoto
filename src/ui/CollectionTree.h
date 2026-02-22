#pragma once

// =============================================================================
// CollectionTree.h - Collection tree sidebar UI component
// =============================================================================

#include <TrussC.h>
#include "Collection.h"
#include "ContextMenu.h"
#include "FolderTree.h"  // for loadJapaneseFont, PlainScrollContainer
using namespace std;
using namespace tc;

// =============================================================================
// CollectionRow - Individual row in the collection tree
// =============================================================================
class CollectionRow : public RectNode {
public:
    using Ptr = shared_ptr<CollectionRow>;

    int collectionId = 0;
    string displayName;
    int photoCount = 0;
    int depth = 0;
    bool expanded = false;
    bool hasChildren = false;
    bool isHeader = false;
    bool selected = false;
    bool dropHighlight = false;
    Collection::Type collType = Collection::Regular;

    Font* font = nullptr;
    Font* fontSmall = nullptr;

    function<void()> onClick;
    function<void()> onToggle;
    function<void(int collectionId)> onRightClick;

    CollectionRow() {
        enableEvents();
    }

    void setup() override {
        setClipping(true);
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();

        // Drop target highlight
        if (dropHighlight) {
            setColor(0.2f, 0.35f, 0.55f);
            fill();
            drawRect(0, 0, w, h);
        }

        if (isHeader) {
            if (selected && !dropHighlight) {
                setColor(0.2f, 0.25f, 0.35f);
                fill();
                drawRect(0, 0, w, h);
            }
            setColor(selected ? Color(0.9f, 0.9f, 0.95f) : Color(0.65f, 0.65f, 0.7f));
            if (font) font->drawString("All Photos", 10, h * 0.5f, Direction::Left, Direction::Center);

            if (fontSmall && photoCount > 0) {
                setColor(0.45f, 0.45f, 0.5f);
                string cnt = to_string(photoCount);
                fontSmall->drawString(cnt, w - 12, h * 0.5f, Direction::Right, Direction::Center);
            }
            return;
        }

        float indent = 14.0f * depth + 10.0f;

        // Selection highlight
        if (selected && !dropHighlight) {
            setColor(0.2f, 0.25f, 0.35f);
            fill();
            drawRect(0, 0, w, h);
        }

        // Toggle triangle (only for groups with children)
        if (hasChildren) {
            setColor(0.6f, 0.6f, 0.65f);
            fill();
            float cx = indent - 7;
            float cy = h * 0.5f;
            if (expanded) {
                drawTriangle(cx - 2.5f, cy - 1.5f, cx + 2.5f, cy - 1.5f, cx, cy + 2.5f);
            } else {
                drawTriangle(cx - 1.5f, cy - 2.5f, cx + 2.5f, cy, cx - 1.5f, cy + 2.5f);
            }
        }

        // Type icon indicator (small colored dot)
        float dotX = indent + 2;
        float dotY = h * 0.5f;
        float dotR = 2.5f;
        fill();
        if (collType == Collection::Smart) {
            setColor(0.55f, 0.45f, 0.7f); // purple dot for smart
        } else if (collType == Collection::Group) {
            setColor(0.5f, 0.55f, 0.6f);  // gray dot for group
        } else {
            setColor(0.4f, 0.55f, 0.7f);  // blue dot for regular
        }
        drawCircle(dotX, dotY, dotR);

        // Collection name
        float nameX = indent + 10;
        if (selected) {
            setColor(0.9f, 0.9f, 0.95f);
        } else {
            setColor(0.7f, 0.7f, 0.75f);
        }

        if (font) {
            string name = displayName;
            float maxNameW = w - nameX - 40;
            if (font->getWidth(name) > maxNameW && name.length() > 5) {
                while (name.length() > 3 && font->getWidth(name + "...") > maxNameW) {
                    name.pop_back();
                }
                name += "...";
            }
            font->drawString(name, nameX, h * 0.5f, Direction::Left, Direction::Center);
        }

        // Photo count (not for groups)
        if (collType != Collection::Group && photoCount > 0 && fontSmall) {
            setColor(0.45f, 0.45f, 0.5f);
            string cnt = to_string(photoCount);
            fontSmall->drawString(cnt, w - 12, h * 0.5f, Direction::Right, Direction::Center);
        }
    }

protected:
    bool onMousePress(Vec2 local, int button) override {
        // Right-click: context menu
        if (button == 1) {
            if (onRightClick) onRightClick(collectionId);
            return true;
        }

        if (button != 0) return false;

        // Toggle expand/collapse for groups
        if (!isHeader && hasChildren) {
            float indent = 14.0f * depth + 10.0f;
            if (local.x >= indent - 14 && local.x < indent) {
                if (onToggle) onToggle();
                return true;
            }
        }

        if (onClick) onClick();
        return true;
    }
};

// =============================================================================
// CollectionTree - Scrollable collection tree sidebar
// =============================================================================
class CollectionTree : public RectNode {
public:
    using Ptr = shared_ptr<CollectionTree>;

    // Callback when a collection is selected (0 = show all)
    function<void(int collectionId)> onCollectionSelected;

    // Right-click context menu request (tcApp handles display)
    Event<ContextMenu::Ptr> contextMenuRequested;

    // Callbacks for rename/create/delete/add photos (wired by tcApp)
    function<void(int collectionId, const string& currentName)> onRenameRequested;
    function<void(int parentId)> onCreateRequested;
    function<void(int collectionId, const string& name)> onDeleteRequested;
    function<void(int collectionId)> onAddSelectedPhotos;

    // Query for whether grid has a selection (set by tcApp)
    function<bool()> hasSelectedPhotos;

    CollectionTree() {
        scrollContainer_ = make_shared<PlainScrollContainer>();
        content_ = make_shared<RectNode>();
        scrollContainer_->setContent(content_);

        scrollBar_ = make_shared<ScrollBar>(scrollContainer_.get(), ScrollBar::Vertical);
        scrollContainer_->addChild(scrollBar_);

        loadJapaneseFont(font_, 12);
        loadJapaneseFont(fontSmall_, 10);
    }

    void setup() override {
        addChild(scrollContainer_);
    }

    void buildTree(const vector<Collection>& collections) {
        nodes_.clear();
        rootNodes_.clear();

        totalPhotoCount_ = 0;
        for (const auto& col : collections) {
            CollectionNode node;
            node.collection = col;
            node.expanded = (col.type == Collection::Group); // groups start expanded
            nodes_.push_back(std::move(node));
            totalPhotoCount_ += col.photoCount;
        }

        buildHierarchy();
        rebuildRows();
        redraw();
    }

    int getSelectedId() const { return selectedId_; }

    void clearSelection() {
        selectedId_ = 0;
        if (onCollectionSelected) onCollectionSelected(0);
        updateSelection();
    }

    void setSize(float w, float h) override {
        RectNode::setSize(w, h);
        scrollContainer_->setRect(0, 0, w, h);
        rebuildRows();
    }

    void draw() override {
        // Background same as FolderTree
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

    // --- Drop target support ---

    // Returns the collection ID at the given local position (for drop target).
    // Only returns Regular collections (not Smart/Group).
    int getDropTargetId(Vec2 localPos) const {
        float scrollY = scrollContainer_->getScrollY();
        for (const auto& row : rows_) {
            if (row->isHeader) continue;
            if (row->collType != Collection::Regular) continue;
            if (!row->getActive()) continue;

            // Row Y is relative to content; adjust for scroll
            float rowY = row->getY() + scrollY;
            float rowH = row->getHeight();
            if (localPos.y >= rowY && localPos.y < rowY + rowH) {
                return row->collectionId;
            }
        }
        return 0;
    }

    void setDropHighlight(int collectionId) {
        bool changed = false;
        for (auto& row : rows_) {
            bool hl = (row->collectionId == collectionId && collectionId > 0 && !row->isHeader);
            if (row->dropHighlight != hl) {
                row->dropHighlight = hl;
                changed = true;
            }
        }
        if (changed) redraw();
    }

    void clearDropHighlight() {
        setDropHighlight(0);
    }

private:
    struct CollectionNode {
        Collection collection;
        int depth = 0;
        bool expanded = false;
        CollectionNode* parent = nullptr;
        vector<CollectionNode*> children;
    };

    ScrollContainer::Ptr scrollContainer_;
    RectNode::Ptr content_;
    ScrollBar::Ptr scrollBar_;
    vector<CollectionRow::Ptr> rows_;

    Font font_;
    Font fontSmall_;

    vector<CollectionNode> nodes_;
    vector<CollectionNode*> rootNodes_;
    int selectedId_ = 0;
    int totalPhotoCount_ = 0;
    bool needsRebuildRows_ = false;

    float rowHeight_ = 20.0f;
    float headerHeight_ = 24.0f;

    bool isNodeVisible(const CollectionNode& node) const {
        const CollectionNode* p = node.parent;
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

        // Build parent-child based on collection.parentId -> collection.id
        unordered_map<int, CollectionNode*> idMap;
        for (auto& node : nodes_) {
            idMap[node.collection.id] = &node;
        }

        for (auto& node : nodes_) {
            if (node.collection.parentId != 0) {
                auto it = idMap.find(node.collection.parentId);
                if (it != idMap.end()) {
                    node.parent = it->second;
                    it->second->children.push_back(&node);
                    continue;
                }
            }
            rootNodes_.push_back(&node);
        }

        for (auto* root : rootNodes_) {
            setDepthRecursive(root, 0);
        }
    }

    void setDepthRecursive(CollectionNode* node, int depth) {
        node->depth = depth;
        for (auto* child : node->children) {
            setDepthRecursive(child, depth + 1);
        }
    }

    void rebuildRows() {
        content_->removeAllChildren();
        rows_.clear();

        float w = getWidth() - 12; // scrollbar space
        if (w <= 0) return;

        float y = 0;

        // "All Photos" header
        auto header = make_shared<CollectionRow>();
        header->isHeader = true;
        header->photoCount = totalPhotoCount_;
        header->selected = (selectedId_ == 0);
        header->font = &font_;
        header->fontSmall = &fontSmall_;
        header->setRect(0, y, w, headerHeight_);
        header->onClick = [this]() {
            selectedId_ = 0;
            if (onCollectionSelected) onCollectionSelected(0);
            updateSelection();
        };
        header->onRightClick = [this](int) {
            buildHeaderContextMenu();
        };
        rows_.push_back(header);
        content_->addChild(header);
        y += headerHeight_;

        // DFS traversal — children appear directly under their parent
        for (auto* root : rootNodes_) {
            addNodeRowDFS(root, w, y);
        }

        content_->setSize(w, y + 10);
        scrollContainer_->updateScrollBounds();
    }

    void addNodeRowDFS(CollectionNode* node, float w, float& y) {
        if (!isNodeVisible(*node)) return;

        auto row = make_shared<CollectionRow>();
        row->collectionId = node->collection.id;
        row->displayName = node->collection.name;
        row->photoCount = node->collection.photoCount;
        row->depth = node->depth;
        row->expanded = node->expanded;
        row->hasChildren = !node->children.empty();
        row->selected = (selectedId_ == node->collection.id);
        row->collType = node->collection.type;
        row->font = &font_;
        row->fontSmall = &fontSmall_;
        row->setRect(0, y, w, rowHeight_);

        CollectionNode* nodePtr = node;
        row->onClick = [this, nodePtr]() {
            int id = nodePtr->collection.id;
            if (selectedId_ == id) {
                selectedId_ = 0;
                if (onCollectionSelected) onCollectionSelected(0);
            } else {
                selectedId_ = id;
                if (onCollectionSelected) onCollectionSelected(id);
            }
            updateSelection();
        };

        row->onToggle = [this, nodePtr]() {
            nodePtr->expanded = !nodePtr->expanded;
            needsRebuildRows_ = true;
            redraw();
        };

        row->onRightClick = [this, nodePtr](int) {
            buildRowContextMenu(nodePtr);
        };

        rows_.push_back(row);
        content_->addChild(row);
        y += rowHeight_;

        // Recurse into children (DFS)
        for (auto* child : node->children) {
            addNodeRowDFS(child, w, y);
        }
    }

    void updateSelection() {
        for (auto& row : rows_) {
            if (row->isHeader) {
                row->selected = (selectedId_ == 0);
            } else {
                row->selected = (selectedId_ == row->collectionId);
            }
        }
        redraw();
    }

    // --- Context menu builders ---

    void buildHeaderContextMenu() {
        auto menu = make_shared<ContextMenu>();
        menu->addChild(make_shared<MenuItem>("New Collection", [this]() {
            if (onCreateRequested) onCreateRequested(0);
        }));
        contextMenuRequested.notify(menu);
    }

    void buildRowContextMenu(CollectionNode* node) {
        auto menu = make_shared<ContextMenu>();
        int id = node->collection.id;
        string name = node->collection.name;
        auto type = node->collection.type;

        // "Add Selected Photos" for Regular collections when grid has selection
        if (type == Collection::Regular && hasSelectedPhotos && hasSelectedPhotos()) {
            menu->addChild(make_shared<MenuItem>("Add Selected Photos", [this, id]() {
                if (onAddSelectedPhotos) onAddSelectedPhotos(id);
            }));
            menu->addChild(make_shared<MenuSeparator>());
        }

        menu->addChild(make_shared<MenuItem>("Rename", [this, id, name]() {
            if (onRenameRequested) onRenameRequested(id, name);
        }));

        menu->addChild(make_shared<MenuItem>("Delete", [this, id, name]() {
            if (onDeleteRequested) onDeleteRequested(id, name);
        }));

        // Groups can have child collections created inside them
        if (type == Collection::Group) {
            menu->addChild(make_shared<MenuSeparator>());
            menu->addChild(make_shared<MenuItem>("New Collection", [this, id]() {
                if (onCreateRequested) onCreateRequested(id);
            }));
        }

        contextMenuRequested.notify(menu);
    }
};
