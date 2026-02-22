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
        // GUI mode — large screens (5K+) can exceed default 64k vertices
        tc::internal::sglMaxVertices = 262144;  // 256k
        tc::internal::sglMaxCommands = 65536;   // 64k

        tc::WindowSettings settings;
        settings.setHighDpi(true);
        settings.setSize(1500, 900);
        return tc::runApp<tcApp>(settings);
    }
}
