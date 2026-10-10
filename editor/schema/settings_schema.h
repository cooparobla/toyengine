/**
 * @file settings_schema.h
 * @brief config.yaml's sections, grouped for the Render Settings and Project Settings tabs.
 *
 * Render settings are organized by FEATURE: each group is one collapsible section holding
 * every key that shapes that feature -- its quality tier, its tunables and its startup sizes --
 * with the feature's master switch (`toggle`) in the section header. Groups sit under a
 * `category` heading (General, Render Features, Stylize, Debug) and may split their rows with
 * sub-headings, some shown only for one value of a mode key (FXAA's rows only when aa_mode is
 * fxaa). Every field has a label and a tooltip.
 *
 * Keys and spellings mirror AppConfig::from_node() (toyengine/core/config.h); every render key it
 * parses appears exactly once here, and each default matches ToyRenderConfig at High quality
 * (editor tests check both). Fields marked startup() are those
 * ToyRenderPipeline::apply_live_config() refuses to change on a live renderer; the tab marks
 * them with * (Restart Renderer applies them).
 */

#ifndef TOYEDITOR_SCHEMA_SETTINGS_SCHEMA_H
#define TOYEDITOR_SCHEMA_SETTINGS_SCHEMA_H

#include "component_schema.h"

#include <string>
#include <utility>
#include <vector>

namespace toy::editor {

/** @brief A sub-heading inside a settings group, drawn before fields[index]. */
struct SettingsSubhead {
    size_t index = 0;
    std::string title;
    std::string show_key;     ///< Non-empty: these rows show only while show_key == show_value.
    std::string show_value;
};

struct SettingsGroup {
    std::string title;
    std::vector<FieldDesc> fields;
    std::string category;                  ///< Render settings: the heading the group sits under.
    std::string tip;                       ///< Header tooltip: what the feature is.
    FieldDesc toggle;                      ///< The feature's on/off switch, in the header (key "" = none).
    std::string needs;                     ///< A bool key the feature also needs on (SSGI needs ssr_enabled).
    std::vector<SettingsSubhead> subheads;
    bool open = false;                     ///< Open the first time it is shown.

    bool has_toggle() const { return !toggle.key.empty(); }
    /** @brief The toggle (if any) and every field: all the keys this group edits. */
    std::vector<FieldDesc> all_fields() const;
};

namespace settings_detail {

/** @brief Builds one group: rows, sub-headings, header toggle. */
class GroupBuilder {
public:
    GroupBuilder(std::string category, std::string title, std::string tip);
    GroupBuilder& toggle(FieldDesc f);
    GroupBuilder& needs(std::string key);
    GroupBuilder& open();
    GroupBuilder& sub(std::string title, std::string show_key = {}, std::string show_value = {});
    GroupBuilder& add(FieldDesc f);
    SettingsGroup done() { return std::move(g_); }

private:
    SettingsGroup g_;
};

/** @brief A labelled field with its tooltip. */
FieldDesc F(FieldDesc f, std::string label, std::string tip);
/** @brief A field a quality tier sets: the tooltip says which, and that editing it overrides the tier. */
FieldDesc tier(FieldDesc f, const std::string& tier_name);
/** @brief A feature's quality tier. */
FieldDesc quality(const std::string& key, std::string tip);

} // namespace settings_detail

const std::vector<SettingsGroup>& render_settings_groups();

/** @brief Every render key the groups above cover (the rest are shown generically). */
std::vector<std::string> render_settings_keys();

const std::vector<SettingsGroup>& project_settings_groups();

/** @brief The config.yaml section each project settings group lives in. */
std::string project_section_key(const std::string& group_title);

} // namespace toy::editor

#endif // TOYEDITOR_SCHEMA_SETTINGS_SCHEMA_H
