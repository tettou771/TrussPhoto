#pragma once

// =============================================================================
// MetadataPanel.h - Right sidebar metadata panel
// =============================================================================

#include <TrussC.h>
#include "PhotoEntry.h"
#include "FolderTree.h"  // for PlainScrollContainer, loadJapaneseFont
using namespace std;
using namespace tc;

class MetadataPanel : public RectNode {
public:
    using Ptr = shared_ptr<MetadataPanel>;

    MetadataPanel() {
        scrollContainer_ = make_shared<PlainScrollContainer>();
        content_ = make_shared<RectNode>();
        scrollContainer_->setContent(content_);

        scrollBar_ = make_shared<ScrollBar>(scrollContainer_.get(), ScrollBar::Vertical);
        scrollContainer_->addChild(scrollBar_);

        loadJapaneseFont(font_, 12);
    }

    void setup() override {
        addChild(scrollContainer_);
    }

    // Set photo entry to display (nullptr to clear)
    void setPhoto(const PhotoEntry* entry) {
        if (entry) {
            entry_ = *entry;
            hasPhoto_ = true;
        } else {
            hasPhoto_ = false;
        }
        needsRedraw_ = true;
    }

    // Set single-view info
    void setViewInfo(float zoom, bool profileEnabled, float profileBlend,
                     bool hasProfile, bool lensEnabled, bool isSmartPreview) {
        viewZoom_ = zoom;
        viewProfileEnabled_ = profileEnabled;
        viewProfileBlend_ = profileBlend;
        viewHasProfile_ = hasProfile;
        viewLensEnabled_ = lensEnabled;
        viewIsSmartPreview_ = isSmartPreview;
        hasViewInfo_ = true;
        needsRedraw_ = true;
    }

    void clearViewInfo() {
        hasViewInfo_ = false;
        needsRedraw_ = true;
    }

    void setSize(float w, float h) override {
        RectNode::setSize(w, h);
        scrollContainer_->setRect(0, 0, w, h);
        needsRedraw_ = true;
    }

    void update() override {
        if (needsRedraw_) {
            needsRedraw_ = false;
            updateContentHeight();
            redraw();
        }
        scrollContainer_->updateScrollBounds();
        scrollBar_->updateFromContainer();
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();

        // Background
        setColor(0.09f, 0.09f, 0.11f);
        fill();
        drawRect(0, 0, w, h);

        // Left border
        setColor(0.2f, 0.2f, 0.22f);
        noFill();
        drawLine(0, 0, 0, h);
    }

private:
    PlainScrollContainer::Ptr scrollContainer_;
    RectNode::Ptr content_;
    ScrollBar::Ptr scrollBar_;
    Font font_;

    PhotoEntry entry_;
    bool hasPhoto_ = false;
    bool hasViewInfo_ = false;
    bool needsRedraw_ = false;

    // View info (single view only)
    float viewZoom_ = 1.0f;
    bool viewProfileEnabled_ = false;
    float viewProfileBlend_ = 1.0f;
    bool viewHasProfile_ = false;
    bool viewLensEnabled_ = false;
    bool viewIsSmartPreview_ = false;

    float lineH_ = 18.0f;
    float sectionGap_ = 8.0f;
    float padding_ = 12.0f;

    // Content node that draws all metadata via font
    class ContentDrawer : public RectNode {
    public:
        MetadataPanel* panel = nullptr;
        void draw() override {
            if (!panel || !panel->hasPhoto_) return;
            panel->drawContent();
        }
    };

    shared_ptr<ContentDrawer> drawer_;

    void updateContentHeight() {
        if (!drawer_) {
            drawer_ = make_shared<ContentDrawer>();
            drawer_->panel = this;
            content_->removeAllChildren();
            content_->addChild(drawer_);
        }

        // Estimate content height
        float y = padding_;
        if (hasPhoto_) {
            y = estimateHeight();
        }

        float w = getWidth() - 12;  // scrollbar space
        content_->setSize(w, y);
        drawer_->setSize(w, y);
        scrollContainer_->updateScrollBounds();
    }

    float estimateHeight() const {
        float y = padding_;
        const auto& e = entry_;

        // File section
        y += lineH_; // header
        y += lineH_; // filename
        y += lineH_; // dimensions
        y += lineH_; // file size
        if (!e.dateTimeOriginal.empty()) y += lineH_;
        y += sectionGap_;

        // Camera section
        if (!e.camera.empty() || !e.lens.empty()) {
            y += lineH_; // header
            if (!e.camera.empty()) y += lineH_;
            if (!e.lens.empty()) y += lineH_;
            // shooting params
            if (e.focalLength > 0 || e.aperture > 0 || e.iso > 0) y += lineH_;
            if (!e.creativeStyle.empty()) y += lineH_;
            y += sectionGap_;
        }

        // Location section
        if (e.hasGps()) {
            y += lineH_; // header
            y += lineH_; // coordinates
            if (e.altitude != 0) y += lineH_;
            y += sectionGap_;
        }

        // Metadata section
        if (e.rating > 0 || !e.colorLabel.empty() || e.flag != 0 || !e.tags.empty()) {
            y += lineH_; // header
            if (e.rating > 0) y += lineH_;
            if (!e.colorLabel.empty()) y += lineH_;
            if (e.flag != 0) y += lineH_;
            if (!e.tags.empty()) y += lineH_;
            y += sectionGap_;
        }

        // Memo section
        if (!e.memo.empty()) {
            y += lineH_; // header
            y += lineH_; // memo text (simplified: single line)
            y += sectionGap_;
        }

        // View section
        if (hasViewInfo_) {
            y += lineH_; // header
            y += lineH_; // zoom
            if (viewHasProfile_) y += lineH_;
            y += lineH_; // lens
            if (viewIsSmartPreview_) y += lineH_;
            y += sectionGap_;
        }

        y += padding_;
        return y;
    }

    void drawSectionHeader(const string& title, float& y, float w) {
        // Dim line
        setColor(0.25f, 0.25f, 0.28f);
        noFill();
        drawLine(padding_, y + lineH_ * 0.5f, w - padding_, y + lineH_ * 0.5f);

        // Title
        setColor(0.45f, 0.45f, 0.5f);
        float tw = font_.getWidth(title);
        float tx = (w - tw) * 0.5f;
        // Background behind title text
        fill();
        setColor(0.09f, 0.09f, 0.11f);
        drawRect(tx - 4, y, tw + 8, lineH_);
        setColor(0.45f, 0.45f, 0.5f);
        font_.drawString(title, tx, y + lineH_ * 0.5f, Direction::Left, Direction::Center);
        y += lineH_;
    }

    void drawLabel(const string& label, const string& value, float& y) {
        setColor(0.5f, 0.5f, 0.55f);
        font_.drawString(label, padding_, y + lineH_ * 0.5f, Direction::Left, Direction::Center);

        setColor(0.75f, 0.75f, 0.8f);
        float labelW = font_.getWidth(label);
        font_.drawString(value, padding_ + labelW + 6, y + lineH_ * 0.5f, Direction::Left, Direction::Center);
        y += lineH_;
    }

    void drawValue(const string& value, float& y, const Color& color = Color(0.75f, 0.75f, 0.8f)) {
        setColor(color);
        font_.drawString(value, padding_, y + lineH_ * 0.5f, Direction::Left, Direction::Center);
        y += lineH_;
    }

    void drawContent() {
        if (!hasPhoto_) return;

        const auto& e = entry_;
        float w = getWidth();
        float y = padding_;

        // === File ===
        drawSectionHeader("File", y, w);

        string typeStr = e.isRaw ? "  [RAW]" : "";
        drawValue(e.filename + typeStr, y);

        drawValue(format("{} x {}", e.width, e.height), y, Color(0.6f, 0.6f, 0.65f));

        // File size
        float sizeMB = e.fileSize / (1024.0f * 1024.0f);
        drawValue(format("{:.1f} MB", sizeMB), y, Color(0.6f, 0.6f, 0.65f));

        if (!e.dateTimeOriginal.empty()) {
            drawValue(e.dateTimeOriginal, y, Color(0.6f, 0.6f, 0.65f));
        }
        y += sectionGap_;

        // === Camera ===
        if (!e.camera.empty() || !e.lens.empty()) {
            drawSectionHeader("Camera", y, w);

            if (!e.camera.empty()) {
                drawValue(e.camera, y);
            }

            if (!e.lens.empty()) {
                drawValue(e.lens, y);
            }

            // Shooting params
            string params;
            if (e.focalLength > 0) params += format("{:.0f}mm", e.focalLength);
            if (e.aperture > 0) {
                if (!params.empty()) params += "  ";
                params += format("f/{:.1f}", e.aperture);
            }
            if (e.iso > 0) {
                if (!params.empty()) params += "  ";
                params += format("ISO {:.0f}", e.iso);
            }
            if (!params.empty()) {
                drawValue(params, y, Color(0.6f, 0.6f, 0.65f));
            }

            if (!e.creativeStyle.empty()) {
                drawValue(e.creativeStyle, y, Color(0.6f, 0.6f, 0.65f));
            }
            y += sectionGap_;
        }

        // === Location ===
        if (e.hasGps()) {
            drawSectionHeader("Location", y, w);

            string coords = format("{:.4f}{} {}, {:.4f}{} {}",
                abs(e.latitude), "\xC2\xB0", e.latitude >= 0 ? "N" : "S",
                abs(e.longitude), "\xC2\xB0", e.longitude >= 0 ? "E" : "W");
            drawValue(coords, y, Color(0.65f, 0.75f, 0.85f));

            if (e.altitude != 0) {
                drawValue(format("Alt: {:.0f}m", e.altitude), y, Color(0.6f, 0.6f, 0.65f));
            }
            y += sectionGap_;
        }

        // === Metadata ===
        if (e.rating > 0 || !e.colorLabel.empty() || e.flag != 0 || !e.tags.empty()) {
            drawSectionHeader("Metadata", y, w);

            if (e.rating > 0) {
                string stars;
                for (int i = 0; i < 5; i++) {
                    stars += (i < e.rating) ? '*' : '.';
                }
                drawValue(stars, y, Color(1.0f, 0.85f, 0.2f));
            }

            if (!e.colorLabel.empty()) {
                Color dotColor = getColorLabelColor(e.colorLabel);
                // Draw color dot + label
                setColor(dotColor);
                fill();
                drawCircle(padding_ + 5, y + lineH_ * 0.5f, 4);
                setColor(0.75f, 0.75f, 0.8f);
                font_.drawString(e.colorLabel, padding_ + 14, y + lineH_ * 0.5f,
                    Direction::Left, Direction::Center);
                y += lineH_;
            }

            if (e.flag != 0) {
                string flagStr = e.flag > 0 ? "Pick" : "Reject";
                Color flagColor = e.flag > 0 ? Color(0.3f, 0.8f, 0.4f) : Color(0.8f, 0.3f, 0.3f);
                drawValue(flagStr, y, flagColor);
            }

            if (!e.tags.empty() && e.tags != "[]") {
                // Parse JSON array of tags
                string tagDisplay = "Tags: ";
                try {
                    auto arr = nlohmann::json::parse(e.tags);
                    bool first = true;
                    for (auto& t : arr) {
                        if (!first) tagDisplay += ", ";
                        tagDisplay += t.get<string>();
                        first = false;
                    }
                } catch (...) {
                    tagDisplay += e.tags;
                }
                drawValue(tagDisplay, y, Color(0.6f, 0.6f, 0.65f));
            }
            y += sectionGap_;
        }

        // === Memo ===
        if (!e.memo.empty()) {
            drawSectionHeader("Memo", y, w);
            drawValue(e.memo, y, Color(0.65f, 0.65f, 0.7f));
            y += sectionGap_;
        }

        // === View (single view only) ===
        if (hasViewInfo_) {
            drawSectionHeader("View", y, w);

            drawValue(format("Zoom: {:.0f}%", viewZoom_ * 100), y);

            if (viewHasProfile_) {
                string profileStr = format("Profile: {} {:.0f}%",
                    viewProfileEnabled_ ? "ON" : "OFF", viewProfileBlend_ * 100);
                drawValue(profileStr, y, Color(0.5f, 0.75f, 0.5f));
            }

            drawValue(format("Lens: {}", viewLensEnabled_ ? "ON" : "OFF"), y,
                Color(0.6f, 0.6f, 0.65f));

            if (viewIsSmartPreview_) {
                drawValue("[Smart Preview]", y, Color(0.7f, 0.55f, 0.2f));
            }
            y += sectionGap_;
        }
    }

    static Color getColorLabelColor(const string& label) {
        if (label == "Red")    return Color(0.9f, 0.25f, 0.25f);
        if (label == "Yellow") return Color(0.9f, 0.85f, 0.2f);
        if (label == "Green")  return Color(0.3f, 0.8f, 0.4f);
        if (label == "Blue")   return Color(0.3f, 0.5f, 0.9f);
        if (label == "Purple") return Color(0.7f, 0.35f, 0.8f);
        return Color(0.5f, 0.5f, 0.5f);
    }
};
