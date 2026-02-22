#pragma once

// =============================================================================
// CropPanel.h - Right sidebar for crop controls (aspect ratio, preview, etc.)
// All UI elements are RectNode children with LayoutMod auto-stacking.
// CropPanel::draw() only renders background + left border (non-scrolling).
// =============================================================================

#include <TrussC.h>
#include "CropTypes.h"
#include "CropWidgets.h"
#include "CropPreview.h"
#include "ui/FolderTree.h"  // for PlainScrollContainer, loadJapaneseFont

using namespace std;
using namespace tc;

class CropPanel : public RectNode {
public:
    using Ptr = shared_ptr<CropPanel>;

    // Events (consumers listen to these)
    Event<CropAspect> aspectChanged;
    Event<bool> orientationChanged;
    Event<float> angleChanged;    // fine angle (radians)
    Event<int> rotate90Event;     // +1 or -1
    Event<void> resetEvent;
    Event<void> doneEvent;
    Event<void> cancelEvent;

    CropPanel() {
        loadJapaneseFont(font_, 12);

        scrollContainer_ = make_shared<PlainScrollContainer>();
        content_ = make_shared<RectNode>();
        scrollContainer_->setContent(content_);

        scrollBar_ = make_shared<ScrollBar>(scrollContainer_.get(), ScrollBar::Vertical);
        scrollContainer_->addChild(scrollBar_);

        // LayoutMod: vertical auto-stacking, children fill width
        contentLayout_ = content_->addMod<LayoutMod>(LayoutDirection::Vertical, 2);
        contentLayout_->setCrossAxis(AxisMode::Fill);
        contentLayout_->setMainAxis(AxisMode::Content);
        contentLayout_->setPadding(9, 0, 12, 0);

        // --- Create all child widgets ---

        previewLabel_ = make_shared<TextLabel>("Preview", &font_);
        previewLabel_->setSize(0, 16);

        preview_ = make_shared<CropPreview>();
        preview_->setSize(0, 100);  // updated in setSize()

        separator1_ = make_shared<Separator>();
        separator1_->setSize(0, 12);

        orientToggle_ = make_shared<OrientationToggle>();
        orientToggle_->setSize(0, 28);
        orientListener_ = orientToggle_->orientationChanged.listen([this](bool& landscape) {
            orientationChanged.notify(landscape);
        });

        aspectLabel_ = make_shared<TextLabel>("Aspect Ratio", &font_);
        aspectLabel_->setSize(0, 16);

        for (int i = 0; i < kCropAspectCount; i++) {
            aspectButtons_[i] = make_shared<AspectButton>((CropAspect)i, &font_);
            aspectButtons_[i]->setSize(0, 26);
            aspectListeners_[i] = aspectButtons_[i]->clicked.listen([this](CropAspect& a) {
                selectAspect(a);
            });
        }
        aspectButtons_[0]->selected = true;

        separator2_ = make_shared<Separator>();
        separator2_->setSize(0, 12);

        rotationLabel_ = make_shared<TextLabel>("Rotation", &font_);
        rotationLabel_->setSize(0, 16);

        rotate90Row_ = make_shared<Rotate90Row>(&font_);
        rotate90Row_->setSize(0, 28);
        rotate90Listener_ = rotate90Row_->rotated.listen([this](int& dir) {
            rotate90Event.notify(dir);
        });

        angleSlider_ = make_shared<AngleSlider>(&font_);
        angleSlider_->setSize(0, 38);
        angleListener_ = angleSlider_->angleChanged.listen([this](float& a) {
            angleChanged.notify(a);
        });

        separator3_ = make_shared<Separator>();
        separator3_->setSize(0, 12);

        outputLabel_ = make_shared<TextLabel>("Output", &font_);
        outputLabel_->setSize(0, 16);

        outputSize_ = make_shared<TextLabel>("", &font_);
        outputSize_->color = Color(0.55f, 0.55f, 0.6f);
        outputSize_->xPad = 22;
        outputSize_->setSize(0, 16);

        buttonRow_ = make_shared<ButtonRow>(&font_);
        buttonRow_->setSize(0, 30);

        resetListener_ = buttonRow_->resetBtn->clicked.listen([this]() { resetEvent.notify(); });
        cancelListener_ = buttonRow_->cancelBtn->clicked.listen([this]() { cancelEvent.notify(); });
        doneListener_ = buttonRow_->doneBtn->clicked.listen([this]() { doneEvent.notify(); });
    }

    void setup() override {
        enableEvents();
        addChild(scrollContainer_);

        content_->addChild(previewLabel_);
        content_->addChild(preview_);
        content_->addChild(separator1_);
        content_->addChild(orientToggle_);
        content_->addChild(aspectLabel_);
        for (int i = 0; i < kCropAspectCount; i++) {
            content_->addChild(aspectButtons_[i]);
        }
        content_->addChild(separator2_);
        content_->addChild(rotationLabel_);
        content_->addChild(rotate90Row_);
        content_->addChild(angleSlider_);
        content_->addChild(separator3_);
        content_->addChild(outputLabel_);
        content_->addChild(outputSize_);
        content_->addChild(buttonRow_);
    }

    CropAspect aspect() const { return currentAspect_; }
    bool isLandscape() const { return orientToggle_->isLandscape; }
    void setOrientation(bool landscape) { orientToggle_->isLandscape = landscape; }

    void setAngle(float radians) {
        angleSlider_->setAngle(radians);
    }

    // Per-vertex UV pairs: TL, TR, BR, BL (supports rotated crop)
    void setPreviewInfo(sg_view view, sg_sampler sampler,
                        float u0, float v0, float u1, float v1,
                        float u2, float v2, float u3, float v3,
                        int outputW, int outputH) {
        preview_->setPreviewInfo(view, sampler, u0, v0, u1, v1, u2, v2, u3, v3,
                                 outputW, outputH);
        outputSize_->text = to_string(outputW) + " x " + to_string(outputH);
    }

    void clearPreview() {
        preview_->clearPreview();
        outputSize_->text = "";
    }

    void setSize(float w, float h) override {
        RectNode::setSize(w, h);
        scrollContainer_->setRect(0, 0, w, h);

        float contentW = w - 12;  // scrollbar space
        content_->setWidth(contentW);

        // Square preview area so portrait/landscape fit at same size
        float previewH = contentW;
        preview_->setHeight(previewH);

        contentLayout_->updateLayout();
    }

    void update() override {
        bool grayed = (currentAspect_ == CropAspect::Free || currentAspect_ == CropAspect::A1_1);
        orientToggle_->grayed = grayed;

        scrollContainer_->updateScrollBounds();
        scrollBar_->updateFromContainer();
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();

        // Background (non-scrolling)
        setColor(0.09f, 0.09f, 0.11f);
        fill();
        drawRect(0, 0, w, h);

        // Left border
        setColor(0.2f, 0.2f, 0.22f);
        noFill();
        drawLine(0, 0, 0, h);
    }

protected:
    bool onMousePress(Vec2 pos, int button) override {
        (void)pos; (void)button;
        return true;  // consume all clicks in panel
    }

private:
    Font font_;
    CropAspect currentAspect_ = CropAspect::Original;
    LayoutMod* contentLayout_ = nullptr;

    // Child widget listeners
    EventListener orientListener_;
    EventListener aspectListeners_[kCropAspectCount];
    EventListener angleListener_, rotate90Listener_;
    EventListener resetListener_, cancelListener_, doneListener_;

    PlainScrollContainer::Ptr scrollContainer_;
    RectNode::Ptr content_;
    ScrollBar::Ptr scrollBar_;

    TextLabel::Ptr previewLabel_;
    CropPreview::Ptr preview_;
    Separator::Ptr separator1_;
    OrientationToggle::Ptr orientToggle_;
    TextLabel::Ptr aspectLabel_;
    AspectButton::Ptr aspectButtons_[kCropAspectCount];
    Separator::Ptr separator2_;
    TextLabel::Ptr rotationLabel_;
    Rotate90Row::Ptr rotate90Row_;
    AngleSlider::Ptr angleSlider_;
    Separator::Ptr separator3_;
    TextLabel::Ptr outputLabel_;
    TextLabel::Ptr outputSize_;
    ButtonRow::Ptr buttonRow_;

    void selectAspect(CropAspect a) {
        currentAspect_ = a;
        for (int i = 0; i < kCropAspectCount; i++) {
            aspectButtons_[i]->selected = (i == (int)a);
        }
        aspectChanged.notify(a);
        redraw();
    }
};
