// toyengine Hub -- create, list and open toyengine projects (a GUI over tools/toyhub).
//
// Usage:   toyengine_hub
// Headless smoke run (no visible window):
//          HEADLESS=1 MAX_FRAMES=30 HUB_SCREENSHOT=out.png ./build/toyengine_hub
//          HUB_PAGE=projects|engine|settings|new|add picks the page (new / add: those dialogs)
// macOS:   `tools/toyhub app` writes ~/Applications/toyengine Hub.app, a launcher for this binary.

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include <toyengine/core/caml_codec.h>
#include <toyengine/core/config.h>
#include <toyengine/core/engine.h>

#include "hub_app.h"

#include <root_directory.h>

namespace {

bool env_flag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::string(v) != "0";
}

}  // namespace

int main() {
    toy::core::install_caml_codec();

    const long max_frames = std::getenv("MAX_FRAMES") ? std::atol(std::getenv("MAX_FRAMES")) : 0;
    const char* screenshot = std::getenv("HUB_SCREENSHOT");

    using toy::hub::HubApp;
    toy::core::Engine engine(HubApp::engine_config(!env_flag("HEADLESS")), HubApp::engine_options());
    {
        HubApp app(engine);
        using Page = HubApp::Page;
        const std::string page = std::getenv("HUB_PAGE") ? std::getenv("HUB_PAGE") : "";
        if (page == "engine") app.set_page(Page::Engine);
        else if (page == "settings") app.set_page(Page::Settings);
        else if (page == "new") app.open_new_project_dialog();
        else if (page == "add") app.open_add_project_dialog();
        long frames = 0;
        while (engine.tick()) {
            ++frames;
            if (app.quit_requested()) break;
            if (max_frames > 0 && frames >= max_frames) break;
        }
        if (screenshot) engine.save_screenshot(screenshot, /*low_res=*/false);
    }
    return 0;
}
