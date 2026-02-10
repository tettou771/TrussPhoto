// =============================================================================
// main.cpp - Entry point (GUI or headless server mode)
// =============================================================================

#include "AppConfig.h"
#include "tcApp.h"

int main(int argc, char* argv[]) {
    AppConfig::parse(argc, argv);

    if (AppConfig::serverMode) {
        // Headless server mode
        tc::HeadlessSettings hs;
        hs.targetFps = 1;
        return tc::runHeadlessApp<tcApp>(hs);
    } else {
        // GUI mode
        tc::WindowSettings settings;
        settings.setHighDpi(true);
        settings.setSize(960, 600);
        return tc::runApp<tcApp>(settings);
    }
}
