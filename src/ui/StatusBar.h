#pragma once

// =============================================================================
// StatusBar.h - Bottom status bar (server state, photo count, task progress)
// =============================================================================

#include <TrussC.h>
#include <mach/mach.h>
#include "FolderTree.h"  // for loadJapaneseFont
using namespace std;
using namespace tc;

class StatusBar : public RectNode {
public:
    using Ptr = shared_ptr<StatusBar>;

    void setup() override {
        loadJapaneseFont(font_, 12);
    }

    // --- Setters (called from tcApp::update) ---

    void setServerStatus(const string& label, Color color) {
        if (serverLabel_ == label) return;
        serverLabel_ = label;
        serverColor_ = color;
        redraw();
    }

    void setPhotoCount(int count) {
        if (photoCount_ == count) return;
        photoCount_ = count;
        redraw();
    }

    void setUploadPending(size_t count) {
        if (uploadPending_ == count) return;
        uploadPending_ = count;
        redraw();
    }

    void setTaskProgress(const string& name, int completed, int total) {
        auto& t = tasks_[name];
        if (t.completed == completed && t.total == total) return;
        t.completed = completed;
        t.total = total;
        redraw();
    }

    void clearTask(const string& name) {
        if (tasks_.erase(name)) redraw();
    }

    void setFps(float fps) {
        fps_ = fps;
    }

    void setRamGiB(double gib) {
        ramGiB_ = gib;
    }

    void draw() override {
        float w = getWidth();
        float h = getHeight();

        // Background
        setColor(0.1f, 0.1f, 0.12f);
        fill();
        drawRect(0, 0, w, h);

        float textX = 10;
        float textY = h / 2;

        // Server indicator (circle + label)
        if (!serverLabel_.empty() && serverLabel_ != "Local") {
            fill();
            setColor(serverColor_);
            drawCircle(textX + 4, textY, 4);
            textX += 14;
        }

        // Build status text
        string status = serverLabel_;
        status += format("  Photos: {}", photoCount_);

        if (uploadPending_ > 0) {
            status += format("  Upload: {}", uploadPending_);
        }

        for (auto& [name, t] : tasks_) {
            status += format("  {}: {}/{}", name, t.completed, t.total);
        }

        status += format("  FPS: {:.0f}  RAM: {:.1f}GiB", fps_, ramGiB_);

        setColor(0.55f, 0.55f, 0.6f);
        font_.drawString(status, textX, textY, Direction::Left, Direction::Center);
    }

private:
    struct TaskInfo {
        int completed = 0;
        int total = 0;
    };

    Font font_;
    string serverLabel_ = "Local";
    Color serverColor_{0.5f, 0.5f, 0.55f};
    int photoCount_ = 0;
    size_t uploadPending_ = 0;
    std::map<string, TaskInfo> tasks_;  // std::map to avoid tc::map collision
    float fps_ = 0;
    double ramGiB_ = 0;

    // RAM measurement (macOS)
public:
    static double measureRamGiB() {
        mach_task_basic_info_data_t taskInfo;
        mach_msg_type_number_t infoCount = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                (task_info_t)&taskInfo, &infoCount) == KERN_SUCCESS) {
            return taskInfo.resident_size / (1024.0 * 1024.0 * 1024.0);
        }
        return 0;
    }
};
