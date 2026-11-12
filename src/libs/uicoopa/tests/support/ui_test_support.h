#pragma once

/**
 * @file ui_test_support.h
 * @brief Shared, header-only helpers for uicoopa's headless test suites (the tests/ directory).
 *
 * Everything here is used by more than one suite; helpers only one suite needs live in that
 * suite's own file. Nothing here touches a GPU device or opens a window: every suite drives
 * widgets through their real component APIs, SceneLoader, and a hand-fed coopa::input::Input.
 *
 * Global singletons (FocusContext, ModalContext, NavigationContext, ThemeLibrary, IconLibrary,
 * SoundLibrary) outlive a test. Each test that touches one clears it on entry and, where the
 * singleton would otherwise hold a raw pointer into the test's soon-to-be-destroyed scene
 * (FocusContext/ModalContext), on exit too -- see the comments at those call sites. ctest runs
 * every suite in its own process, so a leftover can at worst affect a later test in the SAME
 * suite.
 */

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <coopa/testing/test.h>

#include <uicoopa/layout/rect.h>
#include <uicoopa/layout/rect_transform.h>
#include <uicoopa/layout/canvas_scaler.h>
#include <uicoopa/layout/canvas.h>
#include <uicoopa/layout/layout_element.h>

#include <uicoopa/render/ui_vertex.h>
#include <uicoopa/render/sprite.h>
#include <uicoopa/render/draw_list.h>
#include <uicoopa/render/texture_factory.h>
#include <uicoopa/render/ui_pass.h>
#include <uicoopa/render/sprite_sheet.h>
#include <uicoopa/render/icon_library.h>

#include <uicoopa/text/font_atlas.h>
#include <uicoopa/text/font.h>

#include <uicoopa/input/ui_input.h>


#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_manager.h>

#include <root_directory.h>

using namespace coopa::ui;
using coopa::scene::SceneObject;
using coopa::scene::SceneLoader;
using coopa::scene::SceneManager;

/** @brief A bare UIComponent that always wants raycasts -- a hit target with no widget behaviour. */
struct TestRaycastTarget : public UIComponent {
    std::string type_name() const override { return "TestRaycastTarget"; }
    bool wants_raycast() const override { return true; }
};

/**
 * @brief Mirrors EventSystem's private dispatch_chain_: walks leaf and every
 *        SceneObject::parent() above it, calling fn on each IPointerHandler found,
 *        stopping as soon as a handler calls data.consume().
 *
 * Lets headless tests exercise real widget IPointerHandler overrides (Button, Slider,
 * ScrollRect, InventorySlot) exactly as EventSystem would dispatch to them, without needing a
 * real gfxcoopa Window to drive UiInput/EventSystem::process.
 */
template<typename Fn>
inline void dispatch_chain_for_test(SceneObject* leaf, PointerEventData& data, Fn&& fn) {
    for (SceneObject* obj = leaf; obj != nullptr; obj = obj->parent()) {
        for (auto& comp : obj->components()) {
            if (auto* handler = dynamic_cast<IPointerHandler*>(comp.get())) {
                fn(handler, data);
                if (data.consumed) return;
            }
        }
        if (data.consumed) return;
    }
}

/** @brief A plain key press, as TextEditBase::on_key() receives it. */
inline coopa::input::KeyEvent make_key_(coopa::input::Key key) {
    return coopa::input::KeyEvent{ key, 0, coopa::input::KeyAction::Press, coopa::input::Mods::None };
}

/** @brief A Shift+key press (selection-extending navigation). */
inline coopa::input::KeyEvent make_shift_key_(coopa::input::Key key) {
    return coopa::input::KeyEvent{ key, 0, coopa::input::KeyAction::Press, coopa::input::Mods::Shift };
}

/**
 * @brief Writes yaml_content to `<scratch_dir>/<name>.yaml` and returns its path, so tests can
 *        exercise SceneLoader::load() (a real file path) without checking in fixture files or
 *        writing anywhere near the repo.
 */
inline std::string write_temp_yaml(const std::string& name, const std::string& yaml_content) {
    const std::filesystem::path path = coopa::test::scratch_dir() / (name + ".yaml");
    std::ofstream out(path);
    out << yaml_content;
    return path.string();
}
