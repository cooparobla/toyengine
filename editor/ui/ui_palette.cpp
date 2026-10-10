#include "editor/ui/ui_palette.h"

namespace toy {
namespace editor {
namespace ui {
namespace palette_detail {

Node v2(float x, float y) {
    Node n = Node::mapping();
    n["x"] = make_float(x);
    n["y"] = make_float(y);
    return n;
}

Node rgba(float r, float g, float b, float a) {
    Node n = Node::mapping();
    n["r"] = make_float(r); n["g"] = make_float(g); n["b"] = make_float(b); n["a"] = make_float(a);
    return n;
}

Node rect(glm::vec2 amin, glm::vec2 amax, glm::vec2 pivot, glm::vec2 pos, glm::vec2 size) {
    Node c = Node::mapping();
    c["type"] = Node(std::string("RectTransform"));
    c["anchor_min"] = v2(amin.x, amin.y);
    c["anchor_max"] = v2(amax.x, amax.y);
    c["pivot"] = v2(pivot.x, pivot.y);
    c["anchored_position"] = v2(pos.x, pos.y);
    c["size_delta"] = v2(size.x, size.y);
    return c;
}

Node comp(const std::string& type) {
    Node c = Node::mapping();
    c["type"] = Node(type);
    return c;
}

Node object(const std::string& name, std::vector<Node> comps, std::vector<Node> children) {
    Node o = Node::mapping();
    o["name"] = Node(name);
    o["active"] = Node(true);
    Node cs = Node::sequence();
    for (auto& c : comps) cs.as_seq().push_back(std::move(c));
    o["components"] = cs;
    Node ch = Node::sequence();
    for (auto& c : children) ch.as_seq().push_back(std::move(c));
    o["children"] = ch;
    return o;
}

Node items(std::vector<std::pair<std::string, std::string>> list, const char* first_role) {
    Node seq = Node::sequence();
    for (size_t i = 0; i < list.size(); ++i) {
        Node it = Node::mapping();
        it["name"] = Node(list[i].first);
        it["label"] = Node(list[i].second);
        if (i == 0 && first_role) it["role"] = Node(std::string(first_role));
        seq.as_seq().push_back(it);
    }
    return seq;
}

} // namespace palette_detail
} // namespace ui
} // namespace editor
} // namespace toy

namespace toy {
namespace editor {
namespace ui {

const std::vector<PaletteEntry>& palette() {
    using namespace palette_detail;
    using I = imm::Icon;
    static const std::vector<PaletteEntry> entries = [] {
        std::vector<PaletteEntry> e;
        auto add = [&](PaletteEntry p) { e.push_back(std::move(p)); };

        // --- Basics ---
        add({"panel", "Panel", "Basics", I::UiWidget, "A themed panel background (ThemedPanel)", false, [] {
            Node p = comp("ThemedPanel"); p["style"] = Node(std::string("panel"));
            return object("Panel", {centered(320, 220), p});
        }});
        add({"text", "Text", "Basics", I::UiText, "Themed text: font and colour from the theme (ThemedText)", false, [] {
            Node t = comp("ThemedText"); t["text"] = Node(std::string("New Text")); t["font_role"] = Node(std::string("Body"));
            return object("Text", {centered(220, 40), t});
        }});
        add({"title", "Title", "Basics", I::UiText, "A large heading (ThemedText, Title role)", false, [] {
            Node t = comp("ThemedText"); t["text"] = Node(std::string("Title")); t["font_role"] = Node(std::string("Title"));
            t["align"] = Node(std::string("Center"));
            return object("Title", {centered(420, 60), t});
        }});
        add({"button", "Button", "Basics", I::UiButton, "A themed button; game code hears its clicks by its name (ThemedButton)", false, [] {
            Node b = comp("ThemedButton"); b["label"] = Node(std::string("Button")); b["role"] = Node(std::string("Neutral"));
            return object("Button", {centered(180, 44), b});
        }});
        add({"image", "Image", "Basics", I::Image, "A picture or a flat colour (Image)", false, [] {
            Node i = comp("Image"); i["color"] = rgba(1, 1, 1, 1);
            return object("Image", {centered(128, 128), i});
        }});
        add({"label", "Plain Text", "Basics", I::UiText, "Text with its own font and size (Text) -- not themed", false, [] {
            Node t = comp("Text"); t["text"] = Node(std::string("Text")); t["font_size"] = Node(int64_t(24));
            t["color"] = rgba(1, 1, 1, 1); t["vertical_align"] = Node(std::string("Middle"));
            return object("Label", {centered(220, 40), t});
        }});
        add({"slider", "Slider", "Basics", I::UiWidget, "A raw slider, pre-wired: Track, Fill and Handle", false, [] {
            Node s = comp("Slider");
            s["min"] = make_float(0); s["max"] = make_float(1); s["value"] = make_float(0.5);
            s["fill"] = Node(std::string("Fill")); s["handle"] = Node(std::string("Handle"));
            Node track_img = comp("Image"); track_img["color"] = rgba(0.18f, 0.2f, 0.25f);
            Node fill_img = comp("Image"); fill_img["color"] = rgba(0.95f, 0.55f, 0.15f);
            Node handle_img = comp("Image"); handle_img["color"] = rgba(0.9f, 0.92f, 0.98f);
            Node track_rt = rect({0, 0.5f}, {1, 0.5f}, {0.5f, 0.5f}, {0, 0}, {0, 6}); track_rt["hittable"] = Node(false);
            Node fill_rt = rect({0, 0}, {0, 1}, {0, 0.5f}, {0, 0}, {0, 0}); fill_rt["hittable"] = Node(false);
            Node handle_rt = rect({0, 0.5f}, {0, 0.5f}, {0.5f, 0.5f}, {0, 0}, {12, 18}); handle_rt["hittable"] = Node(false);
            return object("Slider", {centered(220, 24), s},
                          {object("Track", {track_rt, track_img}, {object("Fill", {fill_rt, fill_img})}),
                           object("Handle", {handle_rt, handle_img})});
        }});
        add({"progress", "Progress Bar", "Basics", I::UiWidget, "A raw bar, pre-wired: Fill, Ghost (damage trail) and Label", false, [] {
            Node pb = comp("ProgressBar");
            pb["max"] = make_float(100); pb["value"] = make_float(75);
            pb["fill"] = Node(std::string("Fill")); pb["ghost"] = Node(std::string("Ghost")); pb["label"] = Node(std::string("Label"));
            Node bg = comp("Image"); bg["color"] = rgba(0.1f, 0.11f, 0.14f, 0.85f);
            Node ghost = comp("Image"); ghost["color"] = rgba(0.45f, 0.12f, 0.13f);
            Node fill = comp("Image"); fill["color"] = rgba(0.85f, 0.22f, 0.24f);
            Node label = comp("Text"); label["text"] = Node(std::string("")); label["font_size"] = Node(int64_t(14));
            label["horizontal_align"] = Node(std::string("Center")); label["vertical_align"] = Node(std::string("Middle"));
            Node nohit = stretch(); nohit["hittable"] = Node(false);
            return object("ProgressBar", {centered(240, 20), bg, pb},
                          {object("Ghost", {nohit, ghost}), object("Fill", {nohit, fill}), object("Label", {nohit, label})});
        }});

        // --- Layout ---
        add({"vstack", "Vertical Stack", "Layout", I::UiLayout, "Stacks its children top to bottom (VerticalLayoutGroup)", false, [] {
            Node g = comp("VerticalLayoutGroup"); g["spacing"] = make_float(8); g["child_force_expand_height"] = Node(false);
            return object("Column", {centered(300, 320), g});
        }});
        add({"hstack", "Horizontal Row", "Layout", I::UiLayout, "Lines its children up left to right (HorizontalLayoutGroup)", false, [] {
            Node g = comp("HorizontalLayoutGroup"); g["spacing"] = make_float(8); g["child_force_expand_width"] = Node(false);
            g["child_alignment"] = Node(std::string("MiddleLeft"));
            return object("Row", {centered(420, 56), g});
        }});
        add({"grid", "Grid", "Layout", I::Grid, "Fixed-size cells in rows (GridLayoutGroup)", false, [] {
            Node g = comp("GridLayoutGroup"); g["cell_size"] = v2(64, 64); g["cell_spacing"] = v2(6, 6);
            return object("Grid", {centered(360, 280), g});
        }});
        add({"scroll", "Scroll View", "Layout", I::UiLayout, "A scrolling column: its children stack inside (ScrollView)", false, [] {
            return object("ScrollView", {centered(340, 260), comp("ScrollView")});
        }});
        add({"spacer", "Spacer", "Layout", I::Empty, "Empty room inside a stack (LayoutElement)", false, [] {
            Node le = comp("LayoutElement"); le["preferred_size"] = v2(16, 16);
            return object("Spacer", {centered(16, 16), le});
        }});
        add({"container", "Empty Rect", "Layout", I::Empty, "An empty rect to group and anchor other elements", false, [] {
            return object("Group", {centered(200, 200)});
        }});

        // --- HUD ---
        add({"hud_corner", "HUD Corner", "HUD", I::UiAnchor, "Stacks HUD elements in a screen corner (HudCorner)", false, [] {
            Node h = comp("HudCorner"); h["flow"] = Node(std::string("vertical"));
            return object("HudCorner", {rect({0, 1}, {0, 1}, {0, 1}, {32, -32}, {340, 120}), h});
        }});
        auto bar = [&](const char* id, const char* label, const char* role, const char* name) {
            add({id, label, "HUD", I::UiWidget, std::string(label) + ": icon + bar with a damage trail (StatBar)", false, [=] {
                Node s = comp("StatBar"); s["role"] = Node(std::string(role)); s["max"] = make_float(100); s["value"] = make_float(100);
                return object(name, {centered(280, 20), s});
            }});
        };
        bar("health_bar", "Health Bar", "health", "Health");
        bar("stamina_bar", "Stamina Bar", "stamina", "Stamina");
        bar("mana_bar", "Mana Bar", "mana", "Mana");
        add({"hotbar", "Hotbar", "HUD", I::Asset, "A row of item slots with 1-0 key labels (Hotbar)", false, [] {
            Node h = comp("Hotbar"); h["count"] = Node(int64_t(10));
            return object("Hotbar", {rect({0.5f, 0}, {0.5f, 0}, {0.5f, 0}, {0, 24}, {580, 56}), h});
        }});
        add({"message_log", "Message Log", "HUD", I::Console, "A fading feed of messages (MessageLog)", false, [] {
            Node m = comp("MessageLog"); m["max_lines"] = Node(int64_t(6));
            return object("MessageLog", {rect({0, 0}, {0, 0}, {0, 0}, {32, 32}, {420, 140}), m});
        }});
        add({"prompt_bar", "Button Prompts", "HUD", I::Keyboard, "Controller button hints (PromptBar)", false, [] {
            return object("Prompts", {rect({1, 0}, {1, 0}, {1, 0}, {-32, 24}, {420, 36}), comp("PromptBar")});
        }});

        // --- Windows & Menus ---
        add({"window", "Window", "Windows & Menus", I::UiWidget, "A titled panel; its children stack in its body (Window)", false, [] {
            Node w = comp("Window"); w["title"] = Node(std::string("Window"));
            return object("Window", {centered(440, 340), w});
        }});
        add({"dialog", "Dialog", "Windows & Menus", I::UiWidget, "A window with footer buttons and open/close state (Dialog)", false, [] {
            Node d = comp("Dialog"); d["title"] = Node(std::string("Are you sure?"));
            d["buttons"] = items({{"Cancel", "Cancel"}, {"Confirm", "Confirm"}});
            d["buttons"].as_seq()[1]["role"] = Node(std::string("Primary"));
            return object("Dialog", {centered(440, 220), d});
        }});
        add({"menu", "Menu List", "Windows & Menus", I::UiLayout, "A column of named buttons (MenuList)", false, [] {
            Node m = comp("MenuList"); m["items"] = items({{"Play", "Play"}, {"Options", "Options"}, {"Quit", "Quit"}}, "Primary");
            return object("Menu", {centered(280, 160), m});
        }});
        add({"action_bar", "Action Bar", "Windows & Menus", I::UiLayout, "A row of named buttons (ActionBar)", false, [] {
            Node m = comp("ActionBar"); m["items"] = items({{"Back", "Back"}, {"Accept", "Accept"}});
            return object("Actions", {centered(380, 48), m});
        }});
        add({"tabs", "Tab View", "Windows & Menus", I::UiWidget, "Tabs over pages; children fill the pages in order (TabView)", false, [] {
            Node t = comp("TabView");
            Node tabs = Node::sequence(); tabs.as_seq().push_back(Node(std::string("General"))); tabs.as_seq().push_back(Node(std::string("Advanced")));
            t["tabs"] = tabs;
            return object("Tabs", {centered(480, 320), t});
        }});
        auto setting = [&](const char* id, const char* label, const char* kind, const char* name) {
            add({id, label, "Windows & Menus", I::Gear, std::string(label) + ": a labelled control game code reads by name (SettingRow)", false, [=] {
                Node s = comp("SettingRow"); s["kind"] = Node(std::string(kind)); s["label"] = Node(std::string(name));
                if (std::string(kind) == "slider") { s["min"] = make_float(0); s["max"] = make_float(1); s["value"] = make_float(0.8); }
                if (std::string(kind) == "dropdown") {
                    Node it = Node::sequence();
                    for (const char* o : {"Low", "Medium", "High"}) it.as_seq().push_back(Node(std::string(o)));
                    s["items"] = it;
                }
                return object(name, {centered(440, 34), s});
            }});
        };
        setting("setting_slider", "Setting: Slider", "slider", "Volume");
        setting("setting_toggle", "Setting: Toggle", "toggle", "Fullscreen");
        setting("setting_dropdown", "Setting: Dropdown", "dropdown", "Quality");
        add({"collapsible", "Collapsible", "Windows & Menus", I::ArrowDown, "A section that folds away (Collapsible)", false, [] {
            Node c = comp("Collapsible"); c["title"] = Node(std::string("Section"));
            return object("Section", {centered(360, 220), c});
        }});
        add({"item_grid", "Item Grid", "Windows & Menus", I::Asset, "An inventory grid of item slots (ItemGrid)", false, [] {
            Node g = comp("ItemGrid"); g["rows"] = Node(int64_t(4)); g["cols"] = Node(int64_t(6));
            return object("Items", {centered(380, 260), g});
        }});

        // --- Reactors (components added to the selection) ---
        auto reactor = [&](const char* id, const char* label, const char* type, const char* tip) {
            add({id, label, "Reactors", I::Link, tip, true, [=] {
                Node r = comp(type);
                r["listen_object"] = Node(std::string(""));
                r["listen_signal"] = Node(std::string("click"));
                return r;
            }});
        };
        reactor("show_on_signal", "Show On Signal", "SetActiveOnSignal", "Shows (or hides) this element when another one signals -- e.g. a button opens a panel");
        reactor("color_on_signal", "Color On Signal", "ColorOnSignal", "Recolours this element when another one signals");
        reactor("text_on_signal", "Text On Signal", "TextOnSignal", "Changes this element's text when another one signals");
        return e;
    }();
    return entries;
}

const PaletteEntry* find_palette_entry(const std::string& id) {
    for (const auto& p : palette()) if (p.id == id) return &p;
    return nullptr;
}

const std::vector<std::string>& palette_categories() {
    static const std::vector<std::string> c = {"Basics", "Layout", "HUD", "Windows & Menus", "Reactors"};
    return c;
}

} // namespace ui
} // namespace editor
} // namespace toy
