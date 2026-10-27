/**
 * @file panel_screen.h
 * @brief Draws an authored UI panel asset. The component that makes YAML screens work.
 *
 * @code{.yaml}
 * - type: PanelScreen
 *   panel: ui/settings          # the asset
 *   theme_ref: ui/themes/game_dark
 *   hidden: true                # for a screen something else opens
 * @endcode
 *
 * That is a whole working settings screen with no C++: the rows read and write
 * toy::core::UserSettings through their `bind` keys, which already persists.
 *
 * A game that wants a row to drive something of its own, or a button to do something, reaches
 * the same object a C++ screen would:
 *
 * @code
 * auto* s = object->get_component<toy::ui::PanelScreen>();
 * s->bindings().provide("player.fov", [&]{ return cam.fov; }, [&](float v){ cam.fov = v; });
 * s->bindings().on_action("close", [&]{ s->hidden = true; });
 * @endcode
 *
 * The built-in "close" action hides the screen when nothing else claims it, so an authored
 * panel with a Close button works before any game code exists.
 */

#ifndef TOYENGINE_UI_PANEL_SCREEN_H
#define TOYENGINE_UI_PANEL_SCREEN_H

#include <string>

#include <coopa/asset/asset_handle.h>

#include <toyengine/ui/imm_screen.h>
#include <toyengine/ui/ui_panel.h>

namespace toy::ui {

/**
 * @class PanelScreen
 * @brief An ImmScreen whose contents come from a UiPanel asset.
 */
class PanelScreen : public ImmScreen {
public:
    std::string type_name() const override { return "PanelScreen"; }

    /** @brief The panel asset ("ui/settings"; extension optional). */
    std::string panel;

    /** @brief Bindings for this screen's rows and buttons. */
    PanelBindings& bindings() { return bindings_; }

    /** @brief The loaded panel, or null if it has not loaded (or failed). */
    const UiPanel* asset() const { return panel_ ? panel_.get() : handle_.get(); }

    /**
     * @brief Shows `panel` instead of loading `this->panel`.
     *
     * For a host holding a panel in memory rather than on disk -- the editor previewing one
     * as it is edited, or a game building one at runtime. Null reverts to the asset.
     */
    void set_panel(std::shared_ptr<UiPanel> p) { panel_ = std::move(p); load_failed_ = false; }

    /**
     * @brief The action a button fired since this was last called, and CLEARS it.
     *
     * Consumed rather than peeked, because a held value is handled every frame: the built-in
     * Close handler hides the panel, draw() then stops running, and a sticky value would stay
     * "close" for the rest of the process. Prefer bindings().on_action() -- this is for a host
     * that would rather poll than register.
     */
    std::string take_action() {
        std::string a;
        a.swap(last_action_);
        return a;
    }

    void draw(coopa::ui::imm::Context& ctx) override;

protected:
    void on_ready() override;

private:
    PanelBindings bindings_;
    /// Held while the asset system owns the panel, so the slot stays alive and a hot reload is
    /// visible through it (AssetHandle::get() starts returning the new payload).
    coopa::asset::AssetHandle<UiPanel> handle_;
    std::shared_ptr<UiPanel> panel_;
    std::string last_action_;
    int tab_ = 0;
    bool load_failed_ = false;
};

}  // namespace toy::ui

#endif  // TOYENGINE_UI_PANEL_SCREEN_H
