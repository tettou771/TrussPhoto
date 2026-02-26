#pragma once

#include <TrussC.h>
#include <tcxCurl.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include "AppConfig.h"
#include "AppPaths.h"
#include "CatalogSettings.h"
#include "PhotoProvider.h"
#include "ui/FolderTree.h"
#include "ui/CollectionTree.h"
#include "ui/SidebarTabs.h"
#include "ui/MetadataPanel.h"
#include "ui/DevelopPanel.h"
#include "ui/PaneToggle.h"
#include "ui/SearchBar.h"
#include "ui/ContextMenu.h"
#include "ui/NameEditOverlay.h"
#include "ui/StatusBar.h"
#include "views/ViewManager.h"
#include "UploadQueue.h"
#include "ServerConfig.h"
#include "PhotoServer.h"
#include "pipeline/LrcatImporter.h"
#include "pipeline/ExportQueue.h"
using namespace std;
using namespace tc;

class tcApp : public App {
public:
    void setup() override;
    void update() override;
    void draw() override;

    void keyPressed(int key) override;
    void keyReleased(int key) override;

    void mousePressed(Vec2 pos, int button) override;
    void mouseReleased(Vec2 pos, int button) override;
    void mouseMoved(Vec2 pos) override;
    void mouseDragged(Vec2 pos, int button) override;
    void mouseScrolled(Vec2 delta) override;

    void windowResized(int width, int height) override;
    void filesDropped(const vector<string>& files) override;
    void exit() override;

private:
    CatalogSettings catalogSettings_;
    AppBootstrap bootstrap_;
    string catalogPath_;
    PhotoProvider provider_;

    // View management
    ViewManager::Ptr viewManager_;
    ViewContext viewCtx_;

    // UI
    StatusBar::Ptr statusBar_;
    FolderTree::Ptr folderTree_;
    CollectionTree::Ptr collectionTree_;
    SidebarTabs::Ptr sidebarTabs_;
    int sidebarTab_ = 0;  // 0=Folders, 1=Collections
    MetadataPanel::Ptr metadataPanel_;
    DevelopPanel::Ptr developPanel_;
    bool showDevelop_ = false;
    SearchBar::Ptr searchBar_;
    float searchBarHeight_ = 36;
    PaneToggle::Ptr leftToggle_;
    PaneToggle::Ptr rightToggle_;
    bool showSidebar_ = true;
    float sidebarWidth_ = 220;
    bool showMetadata_ = true;
    float metadataWidth_ = 260;

    // Server / sync
    UploadQueue uploadQueue_;
    bool needsServerSync_ = false;
    atomic<bool> syncInProgress_{false};
    atomic<bool> syncCompleted_{false};
    thread syncThread_;
    PhotoServer server_;
    ServerConfig serverConfig_;

    // Layout
    float statusBarHeight_ = 24.0f;
    Tween<float> leftTween_;
    Tween<float> rightTween_;
    float leftPaneWidth_ = 220;
    float rightPaneWidth_ = 260;
    double lastTime_ = 0;

    // Background pipeline flags
    bool spQueued_ = false;
    bool embeddingsQueued_ = false;
    bool wbBackfillQueued_ = false;
    bool visionModelUnloaded_ = false;

    // Background export queue
    ExportQueue exportQueue_;
    EventListener exportThumbnailReadyListener_;
    EventListener exportDoneListener_;

    // Event listeners (RAII — auto-disconnect on destroy)
    EventListener cropDoneListener_;
    EventListener gridClickListener_;
    EventListener gridContextMenuListener_;
    EventListener gridRepairListener_;
    EventListener gridConsolidateListener_;
    EventListener gridDeleteListener_;
    EventListener gridUpdateThumbnailListener_;
    EventListener searchListener_;
    EventListener developListener_;
    EventListener leftToggleListener_;
    EventListener rightToggleListener_;
    EventListener sidebarTabListener_;

    // Context menu
    ContextMenu::Ptr contextMenu_;
    MenuOverlay::Ptr menuOverlay_;
    Vec2 lastRightClickPos_;

    void showContextMenu(ContextMenu::Ptr menu);
    void closeContextMenu();

    // Name edit overlay (shared modal for rename/create)
    shared_ptr<NameEditOverlay> nameOverlay_;

    // Collection tree context menu
    EventListener collectionContextMenuListener_;

    // Drag & drop (Grid -> CollectionTree)
    bool isDraggingToCollection_ = false;
    Vec2 dragStartPos_;
    vector<string> dragPhotoIds_;
    static constexpr float dragThreshold_ = 8.0f;

    // Modifier key tracking
    bool cmdDown_ = false;
    bool shiftDown_ = false;

    // Double-click detection
    chrono::steady_clock::time_point lastClickTime_;
    int lastClickIndex_ = -1;

    // Fonts
    Font font_;
    Font fontSmall_;
    Font nameOverlayFont_;

    // Helper accessors (convenience shortcuts into ViewManager)
    PhotoGrid::Ptr grid() { return viewManager_ ? viewManager_->gridView()->grid() : nullptr; }
    ViewMode viewMode() const { return viewManager_ ? viewManager_->activeView() : ViewMode::Grid; }

    void deleteSelectedPhotos();
    void enqueueLocalOnlyPhotos();
    void configureServer(const string& url, const string& key = "");
    void repairLibrary();
    void relinkMissingPhotos();
    void consolidateLibrary();
    void updateLayout();
    void rebuildFolderTree();
    void updateMetadataPanel();

    // Geo search (Nominatim)
    struct GeoSearchResult {
        bool valid = false;
        double south = 0, north = 0, west = 0, east = 0;
        string textQuery;
    };
    GeoSearchResult geoResult_;
    mutex geoMutex_;

    void searchLocation(const string& location, const string& textQuery);
    void runTextSearch(PhotoGrid::Ptr g, const string& query);

    static string urlEncode(const string& s) {
        string out;
        for (unsigned char c : s) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out += (char)c;
            } else {
                char buf[4];
                snprintf(buf, sizeof(buf), "%%%02X", c);
                out += buf;
            }
        }
        return out;
    }
};
