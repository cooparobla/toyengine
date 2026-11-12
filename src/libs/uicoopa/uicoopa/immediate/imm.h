/**
 * @file imm.h
 * @brief uicoopa's immediate-mode widget layer: tool UIs (the toyengine editor) drawn
 *        through the same DrawList, fonts and UiPass as every retained widget.
 *
 * The retained widgets (Button, Slider, TabView, ...) suit game UI: a fixed layout built
 * once and bound to a model. A tool UI is the opposite -- the hierarchy panel's rows, the
 * inspector's fields and every menu change whenever the document does. Rebuilding
 * SceneObject subtrees for that means destroying nodes mid-dispatch (a known
 * use-after-free shape in this library) and re-wiring signals on every edit. Here the UI
 * is instead re-declared every frame from the model:
 *
 * @code
 * ctx.begin_frame(draw_list, input, canvas_size);
 * if (ctx.begin_menubar({0, 0, w, 24})) {
 *     if (ctx.begin_menu("File")) {
 *         if (ctx.menu_item("Save", "Ctrl+S")) save();
 *         ctx.end_menu();
 *     }
 *     ctx.end_menubar();
 * }
 * ctx.begin_panel("inspector", rect);
 * ctx.drag_float("Roughness", &material.roughness, 0.01f, 0.0f, 1.0f);
 * ctx.end_panel();
 * ctx.end_frame();
 * @endcode
 *
 * Everything is in canvas pixels with a TOP-LEFT origin, +Y down (screen convention) --
 * converted to the canvas's +Y-up space only when geometry reaches the DrawList. Layers
 * (popups, menus, modals, tooltips) are DrawList z-orders, so a menu opened mid-panel still
 * draws over everything after DrawList::finalize_z_order(); hover and clicks resolve against
 * the previous frame's layer rects so a widget under an open popup never reacts.
 *
 * Text goes through TextRenderer, which wraps a uicoopa Font -- or, with no font (headless
 * logic tests), measures with a fixed advance and draws nothing.
 *
 * Host it with ImmediateCanvas (immediate/imm_canvas.h) under a screen-space
 * CanvasComponent, or drive a Context directly with any DrawList.
 */

#ifndef UICOOPA_IMMEDIATE_IMM_H
#define UICOOPA_IMMEDIATE_IMM_H

#include <uicoopa/render/draw_list.h>
#include <uicoopa/render/ui_vertex.h>
#include <uicoopa/text/font.h>
#include <uicoopa/input/ui_input.h>

#include <coopa/input/keys.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coopa {
namespace ui {
namespace imm {

using Id = uint64_t;

// =====================================================================================
// Small value types
// =====================================================================================

/** @brief An axis-aligned rect in imm space (top-left origin, +Y down). */
struct Box {
    float x = 0, y = 0, w = 0, h = 0;

    float right()  const { return x + w; }
    float bottom() const { return y + h; }
    glm::vec2 pos()  const { return {x, y}; }
    glm::vec2 size() const { return {w, h}; }
    glm::vec2 center() const { return {x + w * 0.5f, y + h * 0.5f}; }
    bool contains(glm::vec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    Box shrink(float d) const { return {x + d, y + d, std::max(0.0f, w - 2 * d), std::max(0.0f, h - 2 * d)}; }
    Box intersect(const Box& o) const {
        float x0 = std::max(x, o.x), y0 = std::max(y, o.y);
        float x1 = std::min(right(), o.right()), y1 = std::min(bottom(), o.bottom());
        return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }
};

inline uint32_t pack(const glm::vec4& c) { return UiVertex::pack_color(c.r, c.g, c.b, c.a); }
inline glm::vec4 with_alpha(glm::vec4 c, float a) { c.a = a; return c; }
inline glm::vec4 mix(const glm::vec4& a, const glm::vec4& b, float t) { return a + (b - a) * t; }

/** @brief The display part of a label: everything before "##" (the rest only feeds the id). */
inline std::string_view label_text(std::string_view label) {
    const size_t p = label.find("##");
    return p == std::string_view::npos ? label : label.substr(0, p);
}

/** @brief 64-bit FNV-1a, seeded so nested ids differ from top-level ones. */
inline Id hash_str(std::string_view s, Id seed = 0xcbf29ce484222325ull) {
    Id h = seed;
    for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ull; }
    return h ? h : 1;
}
inline Id hash_int(uint64_t v, Id seed) {
    Id h = seed;
    for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xFF; h *= 0x100000001b3ull; }
    return h ? h : 1;
}

/** @brief HSV (all 0..1) to RGB. */
inline glm::vec3 hsv_to_rgb(float h, float s, float v) {
    h = h - std::floor(h);
    const float i = std::floor(h * 6.0f), f = h * 6.0f - i;
    const float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    switch (static_cast<int>(i) % 6) {
        case 0: return {v, t, p};
        case 1: return {q, v, p};
        case 2: return {p, v, t};
        case 3: return {p, q, v};
        case 4: return {t, p, v};
        default: return {v, p, q};
    }
}

/** @brief RGB (0..1) to HSV (0..1). */
inline glm::vec3 rgb_to_hsv(const glm::vec3& c) {
    const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
    const float d = mx - mn;
    float h = 0.0f;
    if (d > 1e-6f) {
        if (mx == c.r)      h = std::fmod((c.g - c.b) / d, 6.0f);
        else if (mx == c.g) h = (c.b - c.r) / d + 2.0f;
        else                h = (c.r - c.g) / d + 4.0f;
        h /= 6.0f;
        if (h < 0) h += 1.0f;
    }
    return {h, mx > 1e-6f ? d / mx : 0.0f, mx};
}

// =====================================================================================
// Icons (drawn by imm_icons.h, included at the end of this file)
// =====================================================================================

/** @brief The vector icon set; see imm_icons.h for what each looks like. */
enum class Icon : int {
    None = 0,
    // object types
    Cube, Mesh, Sphere, Cylinder, Plane, Empty, Sun, PointLight, SpotLight, EnvLight, Camera, Terrain, Object,
    // data / properties
    Material, Texture, World, Scene, Collection, Physics, Collider, Rigidbody, Component, Script, Render, Output, Tool, Gear,
    // visibility
    Eye, EyeClosed, Monitor, MonitorOff, Check, Lock,
    // tools
    SelectBox, Cursor, Move, Rotate, Scale, Transform, Extrude, Inset, Bevel, Knife, LoopCut,
    // modes
    Vertex, Edge, Face, ObjectMode, EditMode, SculptMode, LocalView,
    // sculpt brushes
    BrushDraw, BrushSmooth, BrushInflate, BrushGrab, BrushFlatten,
    // shading / overlays / view
    ShadeWire, ShadeSolid, ShadeMaterial, ShadeRendered, Overlays, XRay, Grid, Snap, Pivot, Orientation, ViewCamera, Ortho,
    Persp, Zoom, Hand,
    // playback
    Play, Pause, Step, Stop,
    // files
    Folder, File, Image, Search, Filter, Asset, Console, Save, Package,
    // actions / status
    Plus, X, Trash, Duplicate, Link, ArrowRight, ArrowDown, Dots, Undo, Redo, Restart, Info, Warning, Error, Palette,
    MouseLeft, MouseMiddle, MouseRight, Keyboard,
    // paint modes (appended: earlier values stay put)
    VertexPaint, WeightPaint,
    // UI design (appended)
    UiCanvas, UiText, UiButton, UiLayout, UiWidget, UiAnchor,
    // asset browser (appended)
    Tag, Sort,
    // mesh editing (appended)
    Proportional,
    // list reordering (appended)
    Grip,
    // curve authoring (appended)
    Pen,
    // curve anchor modes (appended)
    AnchorCorner, AnchorSmooth, AnchorVector, AnchorAuto,
};

class Context;
/** @brief Draws `icon` fitted (square, centred) into `b`. Defined in imm_icons.h. */
void draw_icon(Context& ctx, Icon icon, const Box& b, const glm::vec4& color);

// =====================================================================================
// Style
// =====================================================================================

/** @brief Sizes and colours for every imm widget. Defaults: Blender 2.8+'s dark theme. */
struct Style {
    // NOTE: the built-in values are only a fallback (a copy of the Blender Dark theme) for hosts
    // that never load a theme; applications load their look from a theme file (imm_theme.h).
    // Every field here is visited by visit_style() there -- add new fields to it too.
    float font_size   = 12.0f;
    float row_height  = 21.0f;
    float padding     = 6.0f;
    float spacing     = 3.0f;
    float indent      = 16.0f;
    float label_ratio = 0.46f;   ///< Property rows: fraction of the row width given to the label.
    float label_align = 1.0f;    ///< Property rows: label placement in its column, 0 = left edge, 1 = against the widget (Blender).
    float scrollbar   = 7.0f;
    float rounding    = 4.0f;    ///< Widget corner radius.
    float tooltip_delay = 0.6f;  ///< Seconds an item must be hovered, with the mouse resting, before its tooltip shows.

    glm::vec4 window_bg     {0.114f, 0.114f, 0.114f, 1.0f};   ///< #1d1d1d -- gaps between areas
    glm::vec4 panel_bg      {0.188f, 0.188f, 0.188f, 1.0f};   ///< #303030 -- editor background
    glm::vec4 panel_alt     {0.157f, 0.157f, 0.157f, 1.0f};   ///< #282828 -- regions, headers' base
    glm::vec4 header        {0.239f, 0.239f, 0.239f, 1.0f};   ///< #3d3d3d -- area headers, sub-panel headers
    glm::vec4 header_hover  {0.290f, 0.290f, 0.290f, 1.0f};
    glm::vec4 subpanel      {0.212f, 0.212f, 0.212f, 1.0f};   ///< sub-panel body
    glm::vec4 border        {0.090f, 0.090f, 0.090f, 1.0f};
    glm::vec4 text          {0.902f, 0.902f, 0.902f, 1.0f};   ///< #e6e6e6
    glm::vec4 text_dim      {0.651f, 0.651f, 0.651f, 1.0f};
    glm::vec4 text_disabled {0.440f, 0.440f, 0.440f, 1.0f};
    glm::vec4 accent        {0.278f, 0.447f, 0.702f, 1.0f};   ///< #4772b3 -- Blender's UI blue
    glm::vec4 button        {0.329f, 0.329f, 0.329f, 1.0f};   ///< #545454
    glm::vec4 button_hover  {0.396f, 0.396f, 0.396f, 1.0f};
    glm::vec4 button_active {0.278f, 0.447f, 0.702f, 1.0f};
    glm::vec4 field         {0.114f, 0.114f, 0.114f, 1.0f};   ///< text / number fields #1d1d1d
    glm::vec4 field_hover   {0.157f, 0.157f, 0.157f, 1.0f};
    glm::vec4 number        {0.329f, 0.329f, 0.329f, 1.0f};   ///< number sliders #545454
    glm::vec4 number_hover  {0.396f, 0.396f, 0.396f, 1.0f};
    glm::vec4 selection     {0.278f, 0.447f, 0.702f, 0.85f};  ///< selected rows (active)
    glm::vec4 selection_dim {0.200f, 0.302f, 0.502f, 0.85f};  ///< selected rows (not active)
    glm::vec4 row_hover     {1.0f, 1.0f, 1.0f, 0.06f};
    glm::vec4 popup_bg      {0.094f, 0.094f, 0.094f, 0.97f};  ///< menus #181818
    glm::vec4 scroll_grab   {0.40f, 0.40f, 0.40f, 0.8f};
    glm::vec4 axis_x        {1.000f, 0.200f, 0.322f, 1.0f};   ///< #ff3352
    glm::vec4 axis_y        {0.545f, 0.863f, 0.000f, 1.0f};   ///< #8bdc00
    glm::vec4 axis_z        {0.157f, 0.565f, 1.000f, 1.0f};   ///< #2890ff
    glm::vec4 object_selected {0.945f, 0.345f, 0.000f, 1.0f}; ///< #f15800 -- viewport selection
    glm::vec4 object_active   {1.000f, 0.667f, 0.251f, 1.0f}; ///< #ffaa40 -- viewport active object
    glm::vec4 warning       {0.98f, 0.80f, 0.30f, 1.0f};
    glm::vec4 error         {0.95f, 0.38f, 0.35f, 1.0f};
    glm::vec4 hover_tint     {1.0f, 1.0f, 1.0f, 0.10f};   ///< overlay on hovered flat widgets
    glm::vec4 edge_highlight {1.0f, 1.0f, 1.0f, 0.08f};   ///< bevel line on frames, popups, menus
    glm::vec4 separator      {0.0f, 0.0f, 0.0f, 0.35f};   ///< dividers inside grouped widgets
    glm::vec4 shadow         {0.0f, 0.0f, 0.0f, 0.09f};   ///< one layer of a popup's soft shadow
    glm::vec4 modal_dim      {0.0f, 0.0f, 0.0f, 0.45f};   ///< behind modal dialogs
    glm::vec4 check_mark     {1.0f, 1.0f, 1.0f, 1.0f};
};

// =====================================================================================
// Text
// =====================================================================================

/**
 * @brief Measures and draws text through a uicoopa Font, at 1:1 canvas pixels.
 *
 * Without a font (headless tests) every glyph is 0.55 em wide and nothing is drawn, so
 * layout logic stays testable with no GPU.
 */
class TextRenderer {
public:
    explicit TextRenderer(Font* font = nullptr) : font_(font) {}
    void set_font(Font* font) { font_ = font; }
    Font* font() const { return font_; }

    /**
     * @brief The width of `s` at `size`, in canvas pixels.
     *
     * Measured off the UNSCALED atlas, so layout is stable whatever the canvas's text scale
     * is -- a panel must not reflow because the window moved to a Retina display. The glyph
     * advances stb reports do not depend on oversampling, so this agrees with draw() even
     * when draw() picks the 2x atlas (see the note there).
     */
    float width(std::string_view s, float size) const {
        if (!font_) return static_cast<float>(s.size()) * size * 0.55f;
        FontAtlas& atlas = font_->atlas_for_size(px_(size), 1);
        float w = 0.0f;
        for (unsigned char c : s) {
            if (const GlyphInfo* g = atlas.glyph(c)) w += g->advance;
        }
        return w;
    }

    float line_height(float size) const {
        if (!font_) return size * 1.25f;
        return font_->atlas_for_size(px_(size), 1).line_height();
    }

    /** @brief Index of the gap closest to x (0..s.size()), for caret placement. */
    size_t hit_index(std::string_view s, float size, float x) const {
        float pen = 0.0f;
        for (size_t i = 0; i < s.size(); ++i) {
            const float adv = width(s.substr(i, 1), size);
            if (x < pen + adv * 0.5f) return i;
            pen += adv;
        }
        return s.size();
    }

    /**
     * @brief Draws one line of text with its top-left at `pos` (imm space).
     * @param canvas_h The canvas height, to flip into DrawList space.
     */
    void draw(DrawList& dl, float canvas_h, glm::vec2 pos, std::string_view s, float size, uint32_t color) const {
        if (!font_ || s.empty()) return;
        // Bake at the size the glyphs are actually DRAWN at (canvas pixels times the canvas's
        // text scale -- 2 on a Retina display) and scale the metrics back down, the same
        // supersampling Text::emit() does, so tool UI text is as crisp as the framebuffer.
        const float scale = std::clamp(dl.text_scale(), 1.0f, 4.0f);
        const uint32_t px = px_(size * scale);
        const float inv = size / static_cast<float>(px);
        // On a SCREEN canvas: baked 1:1 and drawn pixel-snapped (below) -- crisp, evenly
        // weighted stems, the way desktop UI text is rendered. An oversampled atlas allows
        // sub-pixel x, but a stem between two pixel columns draws as two grey ones, so one
        // word's letters look unevenly soft and out of line.
        //
        // On a WORLD canvas there is no device-pixel grid to snap to (the host clears
        // DrawList::pixel_snap() for one), so snapping would quantize glyphs to an arbitrary
        // canvas-pixel lattice for nothing. Keep the 2x oversampled atlas built for sub-pixel
        // placement instead -- the same choice Text::emit() makes, for the same reason.
        const bool snap_to_pixels = dl.pixel_snap();
        const uint32_t over = (px > px_(size) || snap_to_pixels) ? 1u : 2u;
        FontAtlas& atlas = font_->atlas_for_size(px, over);
        dl.set_texture(atlas.texture().view_typed());
        // Baseline in imm space (y down): top + ascent. Canvas y = canvas_h - imm y.
        const float snap = 1.0f / scale;
        const float baseline = snap_to_pixels ? std::round((pos.y + atlas.ascent() * inv) / snap) * snap
                                              : pos.y + atlas.ascent() * inv;
        float pen = snap_to_pixels ? std::round(pos.x / snap) * snap : pos.x;
        for (unsigned char c : s) {
            const GlyphInfo* g = atlas.glyph(c);
            if (!g) continue;
            // Each glyph's quad starts on a device pixel: the atlas is baked at the drawn size, so
            // an unsnapped (sub-pixel) origin would make bilinear sampling smear every glyph by a
            // different fraction -- uneven stems and letters that look out of line. The pen
            // itself keeps its exact advance, so spacing doesn't drift.
            const glm::vec2 size_px = (g->quad_max - g->quad_min) * inv;
            const float x_raw = pen + g->quad_min.x * inv;
            const float y_raw = baseline + g->quad_min.y * inv;                               // imm space (y down)
            const float x0 = snap_to_pixels ? std::round(x_raw / snap) * snap : x_raw;
            const float y_top = snap_to_pixels ? std::round(y_raw / snap) * snap : y_raw;
            Rect r;
            r.min = glm::vec2(x0, canvas_h - (y_top + size_px.y));
            r.max = glm::vec2(x0 + size_px.x, canvas_h - y_top);
            dl.add_quad(r, g->uv, color);
            pen += g->advance * inv;
        }
        dl.set_texture(dl.default_texture());
    }

private:
    static uint32_t px_(float size) { return static_cast<uint32_t>(std::max(1.0f, std::round(size))); }
    Font* font_ = nullptr;
};

// =====================================================================================
// Input
// =====================================================================================

/** @brief One frame of input, in imm space. Build with from_ui_input() or by hand (tests). */
struct FrameInput {
    glm::vec2 mouse{-1e6f};
    glm::vec2 mouse_delta{0.0f};
    bool down[3]     = {};
    bool pressed[3]  = {};
    bool released[3] = {};
    glm::vec2 scroll{0.0f};
    std::vector<uint32_t> chars;
    std::vector<coopa::input::KeyEvent> keys;
    coopa::input::Mods mods = coopa::input::Mods::None;
    float dt = 1.0f / 60.0f;
    std::function<bool(coopa::input::Key)> key_down;           ///< Optional level-triggered query.
    std::function<std::string()> get_clipboard;                ///< Optional.
    std::function<void(const std::string&)> set_clipboard;     ///< Optional.

    /** @brief Converts a canvas's UiInput (+Y up) into imm space. */
    static FrameInput from_ui_input(const UiInput& in, float canvas_h, float dt) {
        FrameInput f;
        f.mouse = glm::vec2(in.position().x, canvas_h - in.position().y);
        f.mouse_delta = glm::vec2(in.delta().x, -in.delta().y);
        for (int b = 0; b < 3; ++b) {
            f.down[b] = in.is_button_down(b);
            f.pressed[b] = in.is_button_pressed(b);
            f.released[b] = in.is_button_released(b);
        }
        f.scroll = in.scroll_delta();
        f.chars.assign(in.char_input().begin(), in.char_input().end());
        f.keys = in.key_events();
        f.mods = in.mods();
        f.dt = dt;
        const UiInput* src = &in;
        f.key_down = [src](coopa::input::Key k) { return src->is_key_down(k); };
        if (const coopa::input::Input* raw = in.source()) {
            auto* mutable_raw = const_cast<coopa::input::Input*>(raw);
            f.get_clipboard = [raw] { return raw->clipboard_text(); };
            f.set_clipboard = [mutable_raw](const std::string& s) { mutable_raw->set_clipboard_text(s); };
        }
        return f;
    }
};

/**
 * @struct BarStyle
 * @brief How a resource bar looks (Context::stat_bar()).
 *
 * Colours default to "unset" (a negative red) and are filled from Context::style, so `{}` is
 * a themed bar and a caller overrides only what differs. A project's own roles -- what ITS
 * health bar looks like -- come from its theme's `game:` section (imm_theme.h) and are passed
 * in here, because Theme is the thing that loads YAML and sits above this header.
 */
struct BarStyle {
    glm::vec4 fill{-1.0f};        /**< The filled portion. */
    glm::vec4 track{-1.0f};       /**< The empty portion behind it. */
    glm::vec4 trail{-1.0f};       /**< The damage trail -- where the value just dropped from. */
    glm::vec4 label{-1.0f};       /**< Label and readout text. */
    float rounding = -1.0f;       /**< Corner radius; negative takes the theme's. */
    float trail_hold = 0.35f;     /**< Seconds the trail waits before it starts catching up. */
    float trail_rate = 1.2f;      /**< Fraction of the bar the trail closes per second. */
    bool  show_value = true;      /**< Draw "value / max" against the right edge. */
};

/**
 * @struct SlotStyle
 * @brief How slot cells look (Context::slot_grid()). Unset colours resolve from Context::style.
 */
struct SlotStyle {
    glm::vec4 cell{-1.0f};       /**< A slot's background. */
    glm::vec4 cell_hover{-1.0f};
    glm::vec4 border{-1.0f};
    float     spacing = -1.0f;   /**< Gap between cells; negative takes the theme's. */
    float     rounding = -1.0f;
};

/** @brief What a Context::slot_grid() callback is told about one cell. */
struct SlotInfo {
    int  index = 0;          /**< Cell index, row-major from the top-left. */
    Box  box;                /**< The cell, already inset by the grid's spacing. */
    bool hovered = false;    /**< The pointer is over it and nothing above has claimed it. */
    bool pressed = false;    /**< It was clicked this frame. */
    bool held = false;       /**< The left button is down on it -- the start of a drag. */
    bool right_pressed = false;  /**< Right-clicked: "use", "equip", a context menu. */
    /**
     * @brief A drag of the grid's own payload type is hovering this cell.
     *
     * The highlight an inventory needs while something is being dragged over a destination.
     * Set only while Context::drag_source() has an active drag, so it is false in the common case.
     */
    bool drop_hovered = false;
};

/**
 * @struct ItemRow
 * @brief One line of Context::item_list(): the list-shaped inventory (Fallout/Bethesda).
 *
 * A grid is one way to show a bag; a categorised list with aligned columns is the other, and it
 * is the one that scales to hundreds of items. Everything here is what the CALLER knows -- the
 * widget sorts nothing and owns nothing, so a game with its own item model is not shut out.
 */
struct ItemRow {
    std::string label;
    int         count = 1;        /**< Shown as "(3)" after the name; 1 draws nothing. */
    Icon        icon = Icon::None;
    glm::vec4   tint{1.0f};       /**< The name's colour -- a rarity tier, usually. */
    float       weight = 0.0f;
    int         value = 0;
    bool        equipped = false; /**< Marked, and sorted by the caller if it wants that. */
    bool        favourite = false;
    int         user = 0;         /**< The caller's handle: a slot index, an item id, anything. */
};

/** @brief Which numeric columns Context::item_list() right-aligns. */
struct ItemListColumns {
    bool count = true;
    bool weight = false;
    bool value = false;
};

/**
 * @enum Anchor
 * @brief Which part of the canvas a panel is positioned against (see Context::anchor()).
 *
 * Named for what they look like on screen, in imm's own space: Top is the top edge, and a
 * positive y offset moves DOWN from it. An offset always moves the box rather than the anchor,
 * so a right- or bottom-pinned panel insets with a negative component -- see
 * Context::anchor(). Stretching is Context::stretch() instead, because a stretched axis
 * ignores the size and reads the offset as a margin, and one function meaning two things is
 * how layout bugs start.
 */
enum class Anchor {
    TopLeft,    TopCenter,    TopRight,
    MiddleLeft, Center,       MiddleRight,
    BottomLeft, BottomCenter, BottomRight,
};

/**
 * @enum UiSound
 * @brief What a widget just did, for a host that wants to put a sound on it.
 *
 * Deliberately about the INTERACTION, not the widget: a host maps these to its own named
 * sounds once, and every widget in the layer is covered -- rather than each widget knowing
 * which sound it should make, which is how a UI ends up half-silent.
 */
enum class UiSound {
    Hover,    ///< The pointer moved onto something interactive.
    Press,    ///< A press began on it.
    Release,  ///< A press completed on it (a click).
};

enum class Mouse { Left = 0, Right = 1, Middle = 2 };

// =====================================================================================
// Context
// =====================================================================================

/** @brief Result of tree_node(). */
struct TreeNodeResult {
    bool open = false;            ///< Children should be emitted (then call tree_pop()).
    bool clicked = false;         ///< Row clicked (not the expander) -- select.
    bool double_clicked = false;
    bool right_clicked = false;
    Box  rect;
};

/** @brief Result of a splitter drag. */
struct DragState {
    bool active = false;
    bool changed = false;
};

/**
 * @class Context
 * @brief All immediate-mode state: ids, hover/active tracking, layout, popups, text editing.
 *
 * One Context per window. Call begin_frame()/end_frame() around each frame's declarations.
 */
class Context {
public:
    Style style;
    TextRenderer text;

    // -------------------------------------------------------------------------------
    // Frame
    // -------------------------------------------------------------------------------

    /**
     * @param dl          Destination; must already be begun (DrawList::begin()) with its
     *                    default texture set.
     * @param in          This frame's input (imm space).
     * @param canvas_size Canvas size in canvas pixels.
     */
    void begin_frame(DrawList& dl, const FrameInput& in, glm::vec2 canvas_size) {
        cursor_ = coopa::input::CursorShape::Arrow;
        dl_ = &dl;
        in_ = in;
        size_ = canvas_size;
        time_ += in.dt;
        ++frame_;
        // Where the right / middle buttons went down, for last_clicked(Mouse::Right / Middle).
        // Widgets record the LEFT press themselves (it is also their drag origin); nothing ever
        // recorded the other two, so a right-click on an item could never count as a click.
        for (int b = 1; b < 3; ++b) if (in_.pressed[b]) press_pos_[b] = in_.mouse;

        // Carried across the frame boundary, before hot_id_ is cleared below: it is what lets
        // a hover sound fire on arrival rather than on every frame the pointer rests.
        hot_prev_ = hot_id_;

        // Resolve which layer the mouse is over from LAST frame's layer rects: the highest z
        // containing the mouse, or the open modal (which owns all input while it is up).
        hovered_layer_ = 0;
        int best_z = -1;
        for (const auto& l : layers_prev_) {
            if (l.modal) { hovered_layer_ = l.id; best_z = 1 << 30; continue; }
            if (l.rect.contains(in_.mouse) && l.z > best_z) { best_z = l.z; hovered_layer_ = l.id; }
        }
        // The modal stays the hover layer even off its rect (so nothing below reacts); a
        // popup ABOVE the modal (e.g. a combo inside it) still wins when under the mouse.
        for (const auto& l : layers_prev_) {
            if (!l.modal && l.z > modal_z_prev_ && l.rect.contains(in_.mouse) && modal_z_prev_ >= 0) hovered_layer_ = l.id;
        }
        popup_hovered_ = hovered_layer_ != 0;

        // Click outside every open (non-modal) popup closes the whole popup stack.
        closed_by_click_.clear();
        if (any_pressed_()) suppress_reopen_.clear();
        // Esc closes the open (non-modal) popups, as in Blender, and is consumed so no
        // shortcut sees it the same frame.
        if (!open_popups_.empty() && !open_popups_.back().modal && !text_edit_.active) {
            auto esc = std::find_if(in_.keys.begin(), in_.keys.end(), [](const coopa::input::KeyEvent& e) {
                return e.key == coopa::input::Key::Escape && e.action == coopa::input::KeyAction::Press;
            });
            if (esc != in_.keys.end()) {
                while (!open_popups_.empty() && !open_popups_.back().modal) open_popups_.pop_back();
                in_.keys.erase(esc);
            }
        }
        if (any_pressed_() && !open_popups_.empty()) {
            bool inside = false;
            for (const auto& l : layers_prev_) if (l.rect.contains(in_.mouse)) inside = true;
            if (!inside) {
                while (!open_popups_.empty() && !open_popups_.back().modal) {
                    closed_by_click_.push_back(open_popups_.back().id);
                    // A left click that closed a popup must not re-open it on release when it
                    // landed on the popup's own opener (dropdown buttons fire on release):
                    // clicking an open dropdown closes it. Right clicks may re-open context menus.
                    if (in_.pressed[0]) suppress_reopen_.push_back(open_popups_.back().id);
                    open_popups_.pop_back();
                }
            }
        }

        layers_.clear();
        layer_stack_.clear();
        layer_stack_.push_back({0, 0});
        id_stack_.clear();
        id_stack_.push_back(0x9e3779b97f4a7c15ull);
        clip_stack_.clear();
        clip_stack_.push_back(Box{0, 0, size_.x, size_.y});
        layout_stack_.clear();
        layout_stack_.push_back(Layout{Box{0, 0, size_.x, size_.y}});
        layout_stack_.back().cursor = {0, 0};
        apply_clip_();
        dl_->set_z_order(0);
        hot_id_ = 0;
        last_item_ = {};
        focusables_.clear();
        keyboard_consumed_ = false;
        wheel_consumed_ = false;
        tooltip_.clear();
        tooltip_seen_ = false;
        dl_->set_texture(dl_->default_texture());

        // A text field that was not redeclared this frame loses focus without committing.
        if (text_edit_.active && text_edit_.seen_frame + 1 < frame_) text_edit_ = {};
    }

    void end_frame() {
        advance_focus_();
        if (!any_down_()) suppress_reopen_.clear();   // the closing click is over
        // The tooltip's item was not hovered this frame: the next hover waits the full delay again.
        if (!tooltip_seen_) tooltip_target_ = 0;
        tooltip_prev_mouse_ = in_.mouse;
        // Tooltip, top-most.
        if (!tooltip_.empty() && !drag_.active) {
            // Line 0 is the title (bright); further lines are description (dim) -- Blender's layout.
            std::vector<std::string> lines;
            size_t start = 0;
            while (start <= tooltip_.size()) {
                const size_t nl = tooltip_.find('\n', start);
                lines.push_back(tooltip_.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
            const float pad = style.padding + 2;
            const float lh = text.line_height(style.font_size) + 2;
            float w = 0;
            for (const auto& l : lines) w = std::max(w, text.width(l, style.font_size));
            w += pad * 2;
            const float h = lh * lines.size() + pad * 1.4f;
            Box b{in_.mouse.x + 14, in_.mouse.y + 20, w, h};
            if (b.right() > size_.x) b.x = size_.x - b.w - 2;
            if (b.bottom() > size_.y) b.y = in_.mouse.y - h - 6;
            dl_->set_z_order(kTooltipZ);
            push_clip_raw_(Box{0, 0, size_.x, size_.y});
            shadow(b, style.rounding + 2);
            fill_rounded(b, style.popup_bg, style.rounding + 1);
            outline_rounded(b, style.edge_highlight, style.rounding + 1);
            for (size_t i = 0; i < lines.size(); ++i) {
                draw_text({b.x + pad, b.y + pad * 0.7f + lh * i}, lines[i], i == 0 ? style.text : style.text_dim);
            }
            pop_clip_();
        }
        // Drag payload preview, top-most.
        if (drag_.active && !drag_.label.empty()) {
            dl_->set_z_order(kTooltipZ + 1);
            push_clip_raw_(Box{0, 0, size_.x, size_.y});
            const float w = text.width(drag_.label, style.font_size) + style.padding * 2;
            Box b{in_.mouse.x + 12, in_.mouse.y + 6, w, style.row_height};
            fill_rounded(b, with_alpha(style.header, 0.95f), style.rounding);
            draw_text({b.x + style.padding, b.y + (b.h - text.line_height(style.font_size)) * 0.5f}, drag_.label, style.text);
            pop_clip_();
        }
        // A widget that held the mouse but was not redeclared this frame (or never saw its
        // release) lets go once every button is up.
        if (active_id_ && !any_down_() && !text_edit_.active) active_id_ = 0;
        if (drag_.active && in_.released[0]) drag_ = {};
        if (in_.released[0]) drag_candidate_ = 0;

        layers_prev_ = layers_;
        modal_z_prev_ = -1;
        for (const auto& l : layers_) if (l.modal) modal_z_prev_ = std::max(modal_z_prev_, l.z);
        // Popups not re-declared this frame are closed (their owner stopped drawing them).
        open_popups_.erase(std::remove_if(open_popups_.begin(), open_popups_.end(),
            [this](const PopupState& p) { return p.seen_frame + 1 < frame_ && p.opened_frame + 1 < frame_; }),
            open_popups_.end());
        dl_->set_z_order(0);
        dl_->pop_clip();   // the clip begin_frame()/apply_clip_() left on the shared DrawList
    }

    // -------------------------------------------------------------------------------
    // Ids
    // -------------------------------------------------------------------------------

    Id get_id(std::string_view label) const { return hash_str(label, id_stack_.back()); }
    /**
     * @brief Hashes a C string's CONTENTS, like the string_view overload.
     *
     * Without this, `get_id("row")` binds to the `const void*` overload instead: array-to-
     * pointer decay is a standard conversion and beats string_view's user-defined one, so a
     * literal would silently hash its own ADDRESS. That works by accident within one binary
     * and then fails the moment the same key is also reached through a string_view -- two
     * different ids for the same name, so a widget loses the state it stored last frame.
     */
    Id get_id(const char* s) const { return hash_str(s ? std::string_view(s) : std::string_view{}, id_stack_.back()); }
    Id get_id(const void* ptr) const { return hash_int(reinterpret_cast<uintptr_t>(ptr), id_stack_.back()); }
    Id get_id(int64_t v) const { return hash_int(static_cast<uint64_t>(v) ^ 0x5bd1e995ull, id_stack_.back()); }
    void push_id(std::string_view s) { id_stack_.push_back(get_id(s)); }
    /** @brief See get_id(const char*): keeps a literal off the pointer overload. */
    void push_id(const char* s) { id_stack_.push_back(get_id(s)); }
    void push_id(int64_t v) { id_stack_.push_back(get_id(v)); }
    void push_id(const void* p) { id_stack_.push_back(get_id(p)); }
    void pop_id() { if (id_stack_.size() > 1) id_stack_.pop_back(); }

    // -------------------------------------------------------------------------------
    // Queries
    // -------------------------------------------------------------------------------

    const FrameInput& input() const { return in_; }
    glm::vec2 mouse() const { return in_.mouse; }
    /**
     * @brief Called when any widget is hovered, pressed or clicked, for UI sound.
     *
     * Null by default, so a UI makes no sound until a host asks for one. Fired from the one
     * state machine every interactive widget goes through, which is why a host wires this
     * once rather than per widget.
     */
    std::function<void(UiSound)> on_sound;

    glm::vec2 canvas_size() const { return size_; }

    // --- Anchored placement -------------------------------------------------------------
    //
    // imm's flow layout (next_box, same_line, begin_region) is what a form or a tool panel
    // wants. A game's interface is the other shape: a handful of panels pinned to the edges
    // of a screen whose size it does not control. These resolve a box against the canvas, so
    // a HUD keeps its corners on any resolution.

    /**
     * @brief A box of `size` pinned to `a`, then moved by `offset`.
     *
     * `offset` is in imm space (+Y DOWN) and is added to the anchored position as-is -- it
     * moves the BOX, never the anchor. So TopLeft with {20, 20} sits 20 in from the top-left
     * corner, while a right- or bottom-pinned box insets with a NEGATIVE offset:
     * BottomCenter with {0, -40} floats 40 above the bottom edge, and TopRight with {-24, 24}
     * sits 24 in from the top-right. A positive x on a right anchor pushes the box
     * off-canvas, which is why this is stated rather than left to be discovered.
     */
    Box anchor(Anchor a, glm::vec2 offset, glm::vec2 size) const {
        const glm::vec2 free = size_ - size;
        float x = 0.0f;
        float y = 0.0f;
        switch (a) {
            case Anchor::TopLeft:      x = 0.0f;        y = 0.0f;        break;
            case Anchor::TopCenter:    x = free.x * 0.5f; y = 0.0f;      break;
            case Anchor::TopRight:     x = free.x;      y = 0.0f;        break;
            case Anchor::MiddleLeft:   x = 0.0f;        y = free.y * 0.5f; break;
            case Anchor::Center:       x = free.x * 0.5f; y = free.y * 0.5f; break;
            case Anchor::MiddleRight:  x = free.x;      y = free.y * 0.5f; break;
            case Anchor::BottomLeft:   x = 0.0f;        y = free.y;      break;
            case Anchor::BottomCenter: x = free.x * 0.5f; y = free.y;    break;
            case Anchor::BottomRight:  x = free.x;      y = free.y;      break;
        }
        return Box{x + offset.x, y + offset.y, size.x, size.y};
    }

    /** @brief The whole canvas, inset by `margin` on every side. */
    Box stretch(float margin = 0.0f) const { return stretch(glm::vec2(margin)); }

    /** @brief The whole canvas, inset by `margin.x` left/right and `margin.y` top/bottom. */
    Box stretch(glm::vec2 margin) const {
        return Box{margin.x, margin.y,
                   std::max(0.0f, size_.x - margin.x * 2.0f),
                   std::max(0.0f, size_.y - margin.y * 2.0f)};
    }

    /**
     * @brief A full-width band: `h` tall, its top `y` from the canvas top, inset `margin_x`.
     * @param y Negative measures UP from the BOTTOM edge, so a footer is stretch_h(-48, 40).
     */
    Box stretch_h(float y, float h, float margin_x = 0.0f) const {
        const float top = y < 0.0f ? size_.y + y : y;
        return Box{margin_x, top, std::max(0.0f, size_.x - margin_x * 2.0f), h};
    }

    /**
     * @brief A full-height column: `w` wide, its left edge `x` from the canvas left.
     * @param x Negative measures LEFT from the RIGHT edge, so a sidebar is stretch_v(-240, 240).
     */
    Box stretch_v(float x, float w, float margin_y = 0.0f) const {
        const float left = x < 0.0f ? size_.x + x : x;
        return Box{left, margin_y, w, std::max(0.0f, size_.y - margin_y * 2.0f)};
    }

    // --- Game composites (immediate/imm_game.h) ----------------------------------------
    //
    // The widgets above are tool-shaped: property rows, drag fields, trees. A game's
    // interface is a different vocabulary, declared here and defined in imm_game.h so this
    // header stays about the core. Each takes a Box -- position it with anchor()/stretch(),
    // or with a box read from a tunables file -- and reports back what the player did.

    /** @brief A plain fraction bar: track and fill, no label. `t` is clamped to 0..1. */
    void value_bar(const Box& b, float t, const BarStyle& style = {});

    /**
     * @brief A labelled resource bar with a damage trail.
     *
     * The trail is what makes a hit readable: the fill drops at once, a ghost holds where the
     * value used to be, then slides down to meet it. Without it, a bar that loses a third of
     * its length in one frame just looks like a different bar.
     *
     * @param id  Distinguishes this bar's trail state from every other bar's.
     * @param max Zero or less draws an empty bar rather than dividing by it.
     */
    void stat_bar(std::string_view id, const Box& b, std::string_view label, float value, float max,
                  const BarStyle& style = {});

    /**
     * @brief A grid of uniform cells, each drawn by `fn(Context&, const SlotInfo&)`.
     *
     * The shared body of every slotted interface -- an inventory, a hotbar, a row of worn
     * equipment -- because the only thing that differs between them is what goes IN a cell.
     * Cells divide `b` exactly, so a grid fills its box instead of leaving a ragged margin.
     */
    template <class SlotFn>
    void slot_grid(std::string_view id, const Box& b, int cols, int rows, SlotFn&& fn,
                   const SlotStyle& style = {});

    /**
     * @brief One row of slots with keybind labels drawn in the corner, for an action bar.
     * @param keys One short label per slot ("1", "Q", "LMB"); longer ones crowd a small slot.
     */
    template <class SlotFn>
    void hotbar(std::string_view id, const Box& b, int count, const std::vector<std::string>& keys,
                SlotFn&& fn, const SlotStyle& style = {});

    /**
     * @brief A cooldown wipe over a slot, retreating downward as the ability comes back.
     * @param t Fraction of the cooldown REMAINING; 0 draws nothing, 1 covers the slot.
     */
    void cooldown(const Box& b, float t);

    /**
     * @brief A log of lines, newest last, pinned to the bottom of `b`.
     *
     * Draws only the lines that fit, counting back from the newest, so a thousand-line history
     * costs what ten do. Lines are not wrapped: one entry is one row.
     * @param colors Optional, parallel to `lines`; short or null uses the theme's text colour.
     */
    void message_log(const Box& b, const std::vector<std::string>& lines,
                     const std::vector<glm::vec4>* colors = nullptr, bool background = true);

    /** @brief A strip of "key -- action" hints, laid left to right and clipped to `b`. */
    void prompt_bar(const Box& b, const std::vector<std::pair<std::string, std::string>>& prompts);

    /**
     * @brief A titled frame; returns the box its contents go in.
     *
     * Unlike begin_panel() this pushes no layout and no clip -- a game's window usually places
     * its contents itself. The ones that want flow can begin_region() inside the result.
     * @param close_clicked Set when the corner close button was pressed, if a pointer is given.
     */
    Box panel_frame(std::string_view id, const Box& b, std::string_view title, bool* close_clicked = nullptr);

    /**
     * @brief A vertical list of names, one selected -- an inventory's category column.
     *
     * @param active In/out: which entry is selected.
     * @return The index clicked this frame, or -1.
     */
    int category_list(std::string_view id, const Box& b, const std::vector<std::string>& names,
                      int* active);

    /**
     * @brief A scrolling list of items with right-aligned numeric columns.
     *
     * The list-shaped inventory. Rows are the caller's, in the caller's order: filtering by
     * category and sorting are decisions only the game can make, and doing them here would mean
     * every game got the same ones.
     *
     * @param selected In/out: which row is highlighted; clamped to the list.
     * @return The index activated this frame (a click), or -1.
     */
    int item_list(std::string_view id, const Box& b, const std::vector<ItemRow>& rows,
                  int* selected, const ItemListColumns& cols = {});

    /**
     * @brief The detail card beside an item list: a title, a description and labelled stats.
     *
     * @param stats Name/value pairs, drawn with the value right-aligned.
     */
    void item_detail(const Box& b, std::string_view title, std::string_view description,
                     const std::vector<std::pair<std::string, std::string>>& stats);

    /**
     * @brief A vertical list of full-width buttons -- a main menu, a pause menu.
     * @param disabled Optional, parallel to `items`: those draw dimmed and do not respond.
     * @return The index pressed this frame, or -1.
     */
    int menu_list(std::string_view id, const Box& b, const std::vector<std::string>& items,
                  const std::vector<bool>* disabled = nullptr);

    /**
     * @brief A grab area that moves `offset` with the pointer.
     *
     * What lets a player drag a HUD group where they want it, and a designer drag a tunable
     * box. The caller owns `offset` and decides whether to clamp, snap or persist it -- where a
     * dragged thing may end up is never this function's business.
     * @return True while the drag is in progress, so a caller can save on the frame it ends.
     */
    bool drag_handle(std::string_view id, const Box& b, glm::vec2* offset);

    /**
     * @brief Text centred on a point, for damage numbers and pickup labels.
     *
     * Centred rather than left-aligned so a number that grows a digit stays over the thing it
     * belongs to instead of drifting right.
     */
    void floating_text(glm::vec2 at, std::string_view s, const glm::vec4& color, float size = 0.0f);

    /**
     * @brief A world plate: a name over a resource bar, filling `b`.
     *
     * On a world-space canvas the plate IS the canvas, so pass stretch() and let it own the
     * quad. Keep plates to bars and labels: a popup on a small plate is clamped to the plate
     * and clipped rather than relocated.
     */
    void nameplate(std::string_view id, const Box& b, std::string_view name, float value, float max,
                   const BarStyle& style = {});
    double time() const { return time_; }
    DrawList& draw_list() { return *dl_; }

    /** @brief True while any widget holds the mouse (a drag, a held button) or a popup is under it. */
    bool wants_mouse() const { return active_id_ != 0 || popup_hovered_ || drag_.active; }
    /** @brief True while a text field has keyboard focus. */
    bool wants_keyboard() const { return text_edit_.active; }
    /** @brief True if the mouse is over a popup/menu/modal layer (last frame's rects). */
    bool popup_hovered() const { return popup_hovered_; }
    /** @brief True if any popup or modal is open. */
    bool any_popup_open() const { return !open_popups_.empty(); }

    bool mouse_pressed(Mouse b = Mouse::Left) const { return in_.pressed[static_cast<int>(b)]; }
    bool mouse_down(Mouse b = Mouse::Left) const { return in_.down[static_cast<int>(b)]; }
    bool mouse_released(Mouse b = Mouse::Left) const { return in_.released[static_cast<int>(b)]; }

    /** @brief True if `key` was pressed (or repeated, with `repeat`) this frame with exactly `mods` held. */
    bool key_pressed(coopa::input::Key key, coopa::input::Mods mods = coopa::input::Mods::None, bool repeat = false) const {
        using coopa::input::KeyAction;
        using coopa::input::Mods;
        const auto mask = [](Mods m) {
            return static_cast<uint8_t>(m) & static_cast<uint8_t>(Mods::Shift | Mods::Control | Mods::Alt | Mods::Super);
        };
        for (const auto& e : in_.keys) {
            if (e.key != key) continue;
            if (!(e.action == KeyAction::Press || (repeat && e.action == KeyAction::Repeat))) continue;
            if (mask(e.mods) == mask(mods)) return true;
        }
        return false;
    }

    /** @brief The platform's command modifier: Super on macOS, Control elsewhere. */
    static coopa::input::Mods command_mod() {
#ifdef __APPLE__
        return coopa::input::Mods::Super;
#else
        return coopa::input::Mods::Control;
#endif
    }

    /**
     * @brief A keyboard shortcut: key_pressed() unless a text field owns the keyboard. Also
     *        accepts Control in place of Super on macOS so "Ctrl+S" works on every keyboard.
     */
    bool shortcut(coopa::input::Key key, coopa::input::Mods mods = coopa::input::Mods::None) const {
        if (text_edit_.active || keyboard_consumed_) return false;
        if (key_pressed(key, mods)) return true;
        using coopa::input::Mods;
        if (has(mods, Mods::Super)) {
            const Mods alt = static_cast<Mods>((static_cast<uint8_t>(mods) & ~static_cast<uint8_t>(Mods::Super)) |
                                               static_cast<uint8_t>(Mods::Control));
            return key_pressed(key, alt);
        }
        return false;
    }

    /** @brief Gives the text field `id_str` (declared later this frame) keyboard focus with `value`. */
    void begin_text_edit(std::string_view id_str, const std::string& value) { begin_text_edit_(get_id(id_str), value, false); }

    // --- Keyboard focus ---
    //
    // Every widget that takes a click is focusable, in the order it is declared. Tab moves to
    // the next, Shift+Tab to the previous, and Enter or Space activates the focused one -- so
    // a form is reachable without a mouse. The ring is drawn centrally (set_last_()), not by
    // each widget, so no widget has to opt in.
    //
    // OFF BY DEFAULT, and that is the important part: Tab, Enter and Space are only free keys
    // in a UI that has no keymap of its own. A host that does -- toyengine's editor binds Tab
    // to "leave edit mode" and Enter to "confirm the modal transform" -- would have those
    // swallowed, because taking a key means consuming the keyboard and shortcut() then returns
    // false. So a host asks for focus navigation; it is not imposed. ImmScreen turns it on for
    // game screens, where a form really should be reachable without a mouse.

    /** @brief Whether Tab/Shift+Tab move focus and Enter/Space activate it. Off by default. */
    bool keyboard_focus = false;

    /** @brief The focused widget's id, or 0. */
    Id focus_id() const { return focus_id_; }

    /** @brief Focuses the widget `id_str` (same id scope as the widget itself). */
    void set_focus(std::string_view id_str) { focus_id_ = get_id(id_str); }

    /** @brief Drops focus, so nothing is reachable by Enter until Tab moves again. */
    void clear_focus() { focus_id_ = 0; }

    /** @brief Whether this frame drew a focus ring -- i.e. the keyboard is driving the UI. */
    bool focus_visible() const { return focus_id_ != 0; }

    /** @brief Marks the keyboard as used this frame, so later shortcut() calls return false. */
    void consume_keyboard() { keyboard_consumed_ = true; }
    /** @brief A widget used the keyboard this frame (a text field typed, a list took Enter): code
     *         reading keys directly (a canvas text editor) should leave them alone. */
    bool keyboard_consumed() const { return keyboard_consumed_; }
    /** @brief The text being typed into field `id_str` (same id scope as the field), if it is being edited. */
    std::optional<std::string> editing_text(std::string_view id_str) {
        if (text_edit_.active && text_edit_.id == get_id(id_str)) return text_edit_.buffer;
        return std::nullopt;
    }

    // -------------------------------------------------------------------------------
    // Last-item queries (the widget most recently declared)
    // -------------------------------------------------------------------------------

    Box  last_rect() const { return last_item_.rect; }
    Id   last_id() const { return last_item_.id; }
    bool last_hovered() const { return last_item_.hovered; }
    bool last_active() const { return last_item_.id != 0 && last_item_.id == active_id_; }
    /** @brief The last item stopped being active this frame (end of a drag / edit). */
    bool last_deactivated() const { return last_item_.deactivated; }
    /** @brief The last item was clicked with `b` this frame. */
    bool last_clicked(Mouse b = Mouse::Left) const {
        return last_item_.hovered && in_.released[static_cast<int>(b)] && press_origin_hit_(b, last_item_.rect);
    }

    /**
     * @brief Shows a tooltip if the last item has been hovered for a moment. A '\n' splits a
     *        bright title line from dim description lines, as Blender's tooltips do.
     */
    void tooltip(std::string_view tip) {
        if (!last_item_.hovered) return;
        tooltip_seen_ = true;
        // The delay restarts on a new item, while the mouse is moving (sweeping across a panel
        // shows nothing) and on any press (a click hides the tip until the next rest).
        const glm::vec2 d = in_.mouse - tooltip_prev_mouse_;
        const bool moving = d.x * d.x + d.y * d.y > 4.0f;
        if (tooltip_target_ != last_item_.id || moving || any_down_()) {
            tooltip_target_ = last_item_.id;
            tooltip_start_ = time_;
            return;
        }
        if (time_ - tooltip_start_ >= style.tooltip_delay) tooltip_ = std::string(tip);
    }
    /** @brief The tooltip shown this frame ("" if none) -- valid until the next begin_frame. */
    const std::string& tooltip_text() const { return tooltip_; }
    /** @brief A Blender-style tooltip: "Title  (Shortcut)" then a description line. */
    void tooltip(std::string_view title, std::string_view shortcut_text, std::string_view description) {
        std::string t(title);
        if (!shortcut_text.empty()) t += "   " + std::string(shortcut_text);
        if (!description.empty()) t += "\n" + std::string(description);
        tooltip(t);
    }

    // -------------------------------------------------------------------------------
    // Drawing primitives (imm space)
    // -------------------------------------------------------------------------------

    void fill(const Box& b, const glm::vec4& c) {
        if (b.empty() || c.a <= 0.0f) return;
        dl_->set_texture(dl_->default_texture());
        dl_->add_quad(to_canvas_(b), Rect{{0.5f, 0.5f}, {0.5f, 0.5f}}, pack(c));
    }
    void outline(const Box& b, const glm::vec4& c, float t = 1.0f) {
        fill({b.x, b.y, b.w, t}, c);
        fill({b.x, b.bottom() - t, b.w, t}, c);
        fill({b.x, b.y + t, t, b.h - 2 * t}, c);
        fill({b.right() - t, b.y + t, t, b.h - 2 * t}, c);
    }
    void line(glm::vec2 a, glm::vec2 b, const glm::vec4& c, float t = 1.0f) {
        dl_->set_texture(dl_->default_texture());
        dl_->add_line(flip_(a), flip_(b), t, pack(c));
    }
    void triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c, const glm::vec4& col) {
        dl_->set_texture(dl_->default_texture());
        dl_->add_triangle(flip_(a), flip_(b), flip_(c), pack(col));
    }
    /** @brief Expander arrow centred in `b`: pointing right (closed) or down (open). */
    void arrow(const Box& b, bool open, const glm::vec4& c) {
        const glm::vec2 m = b.center();
        const float r = std::min(b.w, b.h) * 0.28f;
        if (open) triangle({m.x - r, m.y - r * 0.6f}, {m.x + r, m.y - r * 0.6f}, {m.x, m.y + r * 0.7f}, c);
        else      triangle({m.x - r * 0.6f, m.y - r}, {m.x - r * 0.6f, m.y + r}, {m.x + r * 0.7f, m.y}, c);
    }
    /** @brief Filled disc. */
    void circle(glm::vec2 c, float r, const glm::vec4& col, int segments = 0) {
        if (r <= 0.0f || col.a <= 0.0f) return;
        if (segments <= 0) segments = std::clamp(static_cast<int>(r * 1.5f), 10, 40);
        for (int i = 0; i < segments; ++i) {
            const float a0 = 6.2831853f * i / segments, a1 = 6.2831853f * (i + 1) / segments;
            triangle(c, c + glm::vec2(std::cos(a0), std::sin(a0)) * r, c + glm::vec2(std::cos(a1), std::sin(a1)) * r, col);
        }
    }
    /** @brief Circle outline `t` pixels thick. */
    void ring(glm::vec2 c, float r, float t, const glm::vec4& col, int segments = 0) {
        if (r <= 0.0f || col.a <= 0.0f) return;
        if (segments <= 0) segments = std::clamp(static_cast<int>(r * 1.5f), 12, 48);
        for (int i = 0; i < segments; ++i) {
            const float a0 = 6.2831853f * i / segments, a1 = 6.2831853f * (i + 1) / segments;
            line(c + glm::vec2(std::cos(a0), std::sin(a0)) * r, c + glm::vec2(std::cos(a1), std::sin(a1)) * r, col, t);
        }
    }

    /**
     * @brief A connected run of segments.
     *
     * `line()` is a single oriented quad with no caps and no joins, so a chain of them above
     * about two pixels thick shows a wedge-shaped gap on the OUTSIDE of every bend. This fills
     * each interior joint with a disc, which is the same trick `ring()` relies on implicitly and
     * the reason a path belongs here rather than being re-derived by each caller.
     *
     * @param closed Also joins the last point back to the first.
     */
    void polyline(const glm::vec2* pts, std::size_t n, const glm::vec4& c, float t = 1.0f,
                  bool closed = false) {
        if (pts == nullptr || n < 2 || c.a <= 0.0f || t <= 0.0f) return;
        for (std::size_t i = 1; i < n; ++i) line(pts[i - 1], pts[i], c, t);
        if (closed) line(pts[n - 1], pts[0], c, t);
        // Only worth the discs once the gap is visible; below ~2 px the segments already overlap.
        if (t > 2.0f) {
            const float r = t * 0.5f;
            const std::size_t first = closed ? 0 : 1;
            const std::size_t last = closed ? n : n - 1;
            for (std::size_t i = first; i < last; ++i) circle(pts[i], r, c);
        }
    }

    /// @brief polyline() over a vector.
    void polyline(const std::vector<glm::vec2>& pts, const glm::vec4& c, float t = 1.0f,
                  bool closed = false) {
        polyline(pts.data(), pts.size(), c, t, closed);
    }

    /**
     * @brief A cubic bezier, flattened to straight segments.
     *
     * @param segments 0 picks a count from the curve's rough length, so a short curve is not
     *        oversampled and a long one does not come out faceted. Clamped to [4, 64].
     */
    void bezier(glm::vec2 p0, glm::vec2 c0, glm::vec2 c1, glm::vec2 p1, const glm::vec4& c,
                float t = 1.0f, int segments = 0) {
        if (c.a <= 0.0f || t <= 0.0f) return;
        if (segments <= 0) {
            // The control polygon's length bounds the arc length from above and is three cheap
            // distances rather than an integral -- ample for choosing a segment count.
            const float rough = glm::length(c0 - p0) + glm::length(c1 - c0) + glm::length(p1 - c1);
            segments = static_cast<int>(rough / 10.0f);
        }
        segments = std::clamp(segments, 4, 64);

        std::vector<glm::vec2> pts;
        pts.reserve(static_cast<std::size_t>(segments) + 1);
        for (int i = 0; i <= segments; ++i) {
            const float s = static_cast<float>(i) / static_cast<float>(segments);
            const float u = 1.0f - s;
            pts.push_back(u * u * u * p0 + 3.0f * u * u * s * c0 + 3.0f * u * s * s * c1 +
                          s * s * s * p1);
        }
        polyline(pts, c, t);
    }

    /** @brief Rounded-rect corner flags for fill_rounded() (combine with |). */
    enum Corners : int { kTL = 1, kTR = 2, kBR = 4, kBL = 8, kAll = 15, kTop = 3, kBottom = 12, kLeft = 9, kRight = 6 };

    /** @brief A filled rectangle with rounded corners (`r` < 0: the style's radius). */
    void fill_rounded(const Box& b, const glm::vec4& c, float r = -1.0f, int corners = kAll) {
        if (b.empty() || c.a <= 0.0f) return;
        if (r < 0) r = style.rounding;
        r = std::min({r, b.w * 0.5f, b.h * 0.5f});
        if (r < 0.75f || corners == 0) { fill(b, c); return; }
        fill({b.x + r, b.y, b.w - 2 * r, b.h}, c);
        fill({b.x, b.y + r, r, b.h - 2 * r}, c);
        fill({b.right() - r, b.y + r, r, b.h - 2 * r}, c);
        const glm::vec2 cs[4] = {{b.x + r, b.y + r}, {b.right() - r, b.y + r}, {b.right() - r, b.bottom() - r}, {b.x + r, b.bottom() - r}};
        const float start[4] = {3.14159265f, 4.71238898f, 0.0f, 1.57079633f};
        for (int k = 0; k < 4; ++k) {
            if (!(corners & (1 << k))) {
                const float qx = (k == 0 || k == 3) ? b.x : b.right() - r;
                const float qy = (k == 0 || k == 1) ? b.y : b.bottom() - r;
                fill({qx, qy, r, r}, c);
                continue;
            }
            const int n = std::clamp(static_cast<int>(r), 3, 8);
            for (int i = 0; i < n; ++i) {
                const float a0 = start[k] + 1.5707963f * i / n, a1 = start[k] + 1.5707963f * (i + 1) / n;
                triangle(cs[k], cs[k] + glm::vec2(std::cos(a0), std::sin(a0)) * r, cs[k] + glm::vec2(std::cos(a1), std::sin(a1)) * r, c);
            }
        }
    }
    /** @brief A rounded-rect outline. */
    void outline_rounded(const Box& b, const glm::vec4& c, float r = -1.0f, float t = 1.0f, int corners = kAll) {
        if (b.empty() || c.a <= 0.0f) return;
        if (r < 0) r = style.rounding;
        r = std::min({r, b.w * 0.5f, b.h * 0.5f});
        const float h = t * 0.5f;
        const float rl = (corners & kTL) ? r : 0, rr = (corners & kTR) ? r : 0, br = (corners & kBR) ? r : 0, bl = (corners & kBL) ? r : 0;
        line({b.x + rl, b.y + h}, {b.right() - rr, b.y + h}, c, t);
        line({b.x + bl, b.bottom() - h}, {b.right() - br, b.bottom() - h}, c, t);
        line({b.x + h, b.y + rl}, {b.x + h, b.bottom() - bl}, c, t);
        line({b.right() - h, b.y + rr}, {b.right() - h, b.bottom() - br}, c, t);
        auto corner = [&](glm::vec2 cc, float rad, float a) {
            if (rad <= 0) return;
            for (int i = 0; i < 4; ++i) {
                const float a0 = a + 1.5707963f * i / 4, a1 = a + 1.5707963f * (i + 1) / 4;
                line(cc + glm::vec2(std::cos(a0), std::sin(a0)) * (rad - h), cc + glm::vec2(std::cos(a1), std::sin(a1)) * (rad - h), c, t);
            }
        };
        corner({b.x + rl, b.y + rl}, rl, 3.14159265f);
        corner({b.right() - rr, b.y + rr}, rr, 4.71238898f);
        corner({b.right() - br, b.bottom() - br}, br, 0.0f);
        corner({b.x + bl, b.bottom() - bl}, bl, 1.57079633f);
    }
    /** @brief A soft drop shadow under a floating box (menus, tooltips). */
    void shadow(const Box& b, float r = 6.0f) {
        for (int i = 3; i >= 1; --i) {
            const float g = i * 2.0f;
            fill_rounded({b.x - g + 2, b.y - g + 4, b.w + g * 2 - 4, b.h + g * 2 - 4}, style.shadow, r + g);
        }
    }
    /** @brief Draws an icon (see imm_icons.h) fitted into `b`. */
    void icon(Icon i, const Box& b, const glm::vec4& c) { draw_icon(*this, i, b, c); }

    /**
     * @brief Draws a texture into `b`.
     *
     * The UV rectangle is Y-flipped on purpose: canvas space runs +Y up from the bottom-left
     * while an image's first row is its top, so the obvious {{0,0},{1,1}} draws it upside
     * down. Binding a second texture only starts another draw batch -- the default is put back
     * straight away, exactly as the text renderer does, so no later primitive inherits it.
     */
    void image(coopa::gfx::TextureView view, const Box& b, const glm::vec4& tint = glm::vec4(1.0f)) {
        if (!dl_ || b.empty() || !view.valid()) return;
        dl_->set_texture(view);
        dl_->add_quad(to_canvas_(b), Rect{{0.0f, 1.0f}, {1.0f, 0.0f}}, pack(tint));
        dl_->set_texture(dl_->default_texture());
    }

    void gradient(const Box& b, const glm::vec4& tl, const glm::vec4& tr, const glm::vec4& br, const glm::vec4& bl) {
        dl_->set_texture(dl_->default_texture());
        dl_->add_quad_gradient(flip_(b.pos()), flip_({b.right(), b.y}), flip_({b.right(), b.bottom()}),
                               flip_({b.x, b.bottom()}), pack(tl), pack(tr), pack(br), pack(bl));
    }
    void draw_text(glm::vec2 pos, std::string_view s, const glm::vec4& c, float size = 0.0f) {
        text.draw(*dl_, size_.y, pos, s, size > 0 ? size : style.font_size, pack(c));
    }
    /** @brief Text vertically centred in `b`, left-aligned with `pad`, clipped to b. */
    void text_in(const Box& b, std::string_view s, const glm::vec4& c, float pad = -1.0f, bool center = false) {
        if (pad < 0) pad = style.padding;
        const float lh = text.line_height(style.font_size);
        float x = b.x + pad;
        if (center) x = b.x + (b.w - text.width(s, style.font_size)) * 0.5f;
        push_clip(b);
        draw_text({x, b.y + (b.h - lh) * 0.5f}, s, c);
        pop_clip();
    }
    float text_width(std::string_view s) const { return text.width(s, style.font_size); }

    void push_clip(const Box& b) { clip_stack_.push_back(clip_stack_.back().intersect(b)); apply_clip_(); }
    void pop_clip() { pop_clip_(); }
    Box  current_clip() const { return clip_stack_.back(); }

    // -------------------------------------------------------------------------------
    // Layout
    // -------------------------------------------------------------------------------

    /** @brief The current layout region's content box. */
    Box content_region() const { return layout_().region; }
    /** @brief Width left on the current row from the cursor to the region's right edge. */
    float available_width() const {
        const Layout& l = layout_();
        return std::max(0.0f, l.region.right() - cursor_x_());
    }
    glm::vec2 cursor() const { return {cursor_x_(), layout_().cursor.y}; }
    void set_cursor_y(float y) { layout_().cursor.y = y; layout_().same_line = false; }

    /**
     * @brief Reserves the next item's box: on the current row after same_line(), else at the
     *        start of a new row (after `indent`).
     * @param w Width; <= 0 means "the rest of the row" (minus -w).
     */
    Box next_box(float h, float w = 0.0f) {
        Layout& l = layout_();
        const float x = cursor_x_();
        if (w <= 0) w = std::max(1.0f, l.region.right() - x + w);
        Box b{x, l.cursor.y, w, h};
        l.row_h = l.same_line ? std::max(l.row_h, h) : h;
        l.last_x_end = b.right();
        l.line_start_y = l.cursor.y;
        l.same_line = false;
        l.cursor.y = l.line_start_y + l.row_h + style.spacing;
        l.content_bottom = std::max(l.content_bottom, b.bottom());
        return b;
    }
    /** @brief Places the next item to the right of the previous one, on the same row. */
    void same_line(float gap = -1.0f) {
        Layout& l = layout_();
        l.same_line = true;
        l.same_line_x = l.last_x_end + (gap < 0 ? style.spacing : gap);
        l.cursor.y = l.line_start_y;
    }
    void indent(float d = -1.0f) { layout_().indent += d < 0 ? style.indent : d; }
    void unindent(float d = -1.0f) { layout_().indent = std::max(0.0f, layout_().indent - (d < 0 ? style.indent : d)); }
    void spacing(float h = -1.0f) { layout_().cursor.y += h < 0 ? style.spacing * 2 : h; layout_().same_line = false; }
    void separator() {
        Box b = next_box(style.spacing * 2 + 1);
        fill({b.x, b.y + style.spacing, b.w, 1}, style.border);
    }

    /**
     * @brief A fixed-rect layout region, optionally scrollable and filled.
     * @param id     Scroll state key.
     * @param rect   The region on screen.
     * @param scroll Vertical scrolling + scrollbar when the content overflows.
     */
    void begin_region(std::string_view id, const Box& rect, bool scroll = true, const glm::vec4* bg = nullptr) {
        const Id rid = get_id(id);
        if (bg) fill(rect, *bg);
        push_clip(rect);
        Layout l{rect.shrink(style.padding)};
        l.id = rid;
        l.outer = rect;
        l.scroll = scroll;
        if (scroll) {
            l.region.w = std::max(0.0f, l.region.w - style.scrollbar);
            auto& st = state_(rid);
            l.scroll_y = &st.f[0];
            // Clamp BEFORE laying out, against last frame's content height (f[1]) and this
            // frame's view: while a region is being resized, laying out with a stale offset
            // and clamping only at end_region() snapped the content every frame (jitter).
            const float max_scroll = std::max(0.0f, st.f[1] - (rect.h - style.padding));
            *l.scroll_y = std::clamp(*l.scroll_y, 0.0f, max_scroll);
        }
        // Whole pixels: a fractional offset (scrollbar drags) makes text shimmer.
        l.cursor = {l.region.x, l.region.y - (l.scroll_y ? std::round(*l.scroll_y) : 0.0f)};
        l.content_top = l.cursor.y;
        l.content_bottom = l.cursor.y;
        layout_stack_.push_back(l);
        push_id(id);
    }

    void end_region() {
        pop_id();
        Layout l = layout_stack_.back();
        layout_stack_.pop_back();
        if (l.scroll && l.scroll_y) {
            const float content_h = (l.content_bottom - l.content_top) + style.padding;
            const float view_h = l.outer.h - style.padding;
            const float max_scroll = std::max(0.0f, content_h - view_h);
            state_(l.id).f[1] = content_h;
            if (!wheel_consumed_ && in_.scroll.y != 0.0f && l.outer.contains(in_.mouse) && layer_ok_() &&
                current_clip().contains(in_.mouse)) {
                *l.scroll_y -= in_.scroll.y * style.row_height * 2.0f;
                wheel_consumed_ = true;
            }
            *l.scroll_y = std::clamp(*l.scroll_y, 0.0f, max_scroll);   // before the grab is placed
            // Scrollbar.
            if (max_scroll > 0.0f) {
                const Box track{l.outer.right() - style.scrollbar, l.outer.y, style.scrollbar, l.outer.h};
                const float grab_h = std::max(20.0f, track.h * view_h / content_h);
                const float t = *l.scroll_y / max_scroll;
                Box grab{track.x + 1, track.y + (track.h - grab_h) * t, track.w - 2, grab_h};
                const Id sid = hash_int(l.id, 77);
                bool hovered = hoverable_(track);
                if (hovered && in_.pressed[0]) { active_id_ = sid; scroll_grab_offset_ = in_.mouse.y - grab.y; }
                if (active_id_ == sid && in_.down[0] && track.h > grab_h) {
                    const float ny = std::clamp((in_.mouse.y - scroll_grab_offset_ - track.y) / (track.h - grab_h), 0.0f, 1.0f);
                    *l.scroll_y = ny * max_scroll;
                }
                fill_rounded(grab, active_id_ == sid || hovered ? style.text_dim : style.scroll_grab, grab.w * 0.5f);
            }
            *l.scroll_y = std::clamp(*l.scroll_y, 0.0f, max_scroll);
        }
        pop_clip();
    }

    /** @brief A panel: a filled region (scrollable) with an optional title bar. */
    void begin_panel(std::string_view id, const Box& rect, std::string_view title = {}) {
        fill(rect, style.panel_bg);
        Box body = rect;
        if (!title.empty()) {
            Box bar{rect.x, rect.y, rect.w, style.row_height + 3};
            fill(bar, style.header);
            text_in(bar, title, style.text);
            body = {rect.x, rect.y + bar.h, rect.w, rect.h - bar.h};
        }
        begin_region(id, body, true);
    }
    void end_panel() { end_region(); }

    // -------------------------------------------------------------------------------
    // Basic widgets
    // -------------------------------------------------------------------------------

    void label(std::string_view s, const glm::vec4* color = nullptr) {
        Box b = next_box(style.row_height, text_width(label_text(s)) + style.padding);
        text_in(b, label_text(s), color ? *color : style.text, 0.0f);
        set_last_(0, b, false);
    }
    void label_dim(std::string_view s) { label(s, &style.text_dim); }
    /** @brief A full-row heading in the accent colour. */
    void heading(std::string_view s) {
        Box b = next_box(style.row_height);
        text_in(b, label_text(s), style.accent, 0.0f);
        set_last_(0, b, false);
    }
    /** @brief Wrapped paragraph text (word wrap at the region width). */
    void paragraph(std::string_view s, const glm::vec4* color = nullptr) {
        const float w = available_width();
        std::string line, word;
        std::vector<std::string> lines;
        auto flush_word = [&] {
            if (word.empty()) return;
            const std::string candidate = line.empty() ? word : line + " " + word;
            if (!line.empty() && text_width(candidate) > w) { lines.push_back(line); line = word; }
            else line = candidate;
            word.clear();
        };
        for (char c : s) {
            if (c == ' ') flush_word();
            else if (c == '\n') { flush_word(); lines.push_back(line); line.clear(); }
            else word += c;
        }
        flush_word();
        if (!line.empty()) lines.push_back(line);
        const float lh = text.line_height(style.font_size);
        for (const auto& l : lines) {
            Box b = next_box(lh);
            draw_text(b.pos(), l, color ? *color : style.text);
        }
    }

    /** @brief A push button. @return True when clicked. */
    /** @brief A button in the next layout box; `w` negative fits it to its label. */
    bool button(std::string_view lbl, float w = -1.0f, bool enabled = true, Icon ic = Icon::None) {
        const float iw = ic != Icon::None ? style.row_height - 4 : 0.0f;
        if (w < 0) w = text_width(label_text(lbl)) + style.padding * 3 + iw;
        return button_in_(lbl, next_box(style.row_height, w), enabled, ic);
    }

    /**
     * @brief A button at an explicit box.
     *
     * What a game's interface wants: its buttons are placed against the screen (anchor() /
     * stretch(), or a box from a tunables file) rather than flowed down a panel.
     */
    bool button(std::string_view lbl, const Box& b, bool enabled = true, Icon ic = Icon::None) {
        return button_in_(lbl, b, enabled, ic);
    }

    /**
     * @brief A square icon button, Blender-style: transparent until hovered, filled when `on`.
     * @param tip      Tooltip ("Title\nDescription"); shown after a short hover.
     * @param corners  fill_rounded() corner flags, for joined button groups.
     * @param box      Explicit placement; default: the next layout box.
     */
    bool icon_button(std::string_view id_str, Icon ic, std::string_view tip = {}, bool on = false, float size = 0.0f,
                     int corners = kAll, std::optional<Box> box = std::nullopt, bool framed = false) {
        const Id id = get_id(id_str);
        if (size <= 0) size = style.row_height;
        Box b = box ? *box : next_box(size, size);
        bool hovered = false, held = false;
        const bool clicked = behavior_(id, b, hovered, held);
        if (on) fill_rounded(b, held ? with_alpha(style.accent, 0.8f) : style.accent, -1, corners);
        else if (framed) fill_rounded(b, held ? style.button_active : hovered ? style.button_hover : style.button, -1, corners);
        else if (hovered) fill_rounded(b, held ? style.button_active : style.hover_tint, -1, corners);
        const float pad = std::max(2.0f, b.h * 0.16f);
        icon(ic, b.shrink(pad), style.text);
        set_last_(id, b, hovered);
        if (!tip.empty()) tooltip(tip);
        return clicked;
    }

    /**
     * @brief Blender's joined button row (shading modes, select modes): icons share one
     *        rounded frame, the active one highlighted. @return True when the choice changed.
     */
    bool icon_group(std::string_view id_str, const std::vector<std::pair<Icon, std::string>>& items, int* active,
                    std::optional<Box> where = std::nullopt, float size = 0.0f) {
        if (size <= 0) size = style.row_height;
        const float w = size * static_cast<float>(items.size());
        const Box b = where ? *where : next_box(size, w);
        push_id(id_str);
        fill_rounded(b, style.button);
        bool changed = false;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            int corners = 0;
            if (i == 0) corners |= kLeft;
            if (i + 1 == static_cast<int>(items.size())) corners |= kRight;
            const Box cell{b.x + i * size, b.y, size, b.h};
            if (icon_button(items[i].second, items[i].first, items[i].second, *active == i, size, corners, cell)) {
                if (*active != i) { *active = i; changed = true; }
            }
            if (i > 0) fill({cell.x, cell.y + 3, 1, cell.h - 6}, style.separator);
        }
        pop_id();
        return changed;
    }

    /** @brief A toggle-able button (toolbar). @return True when clicked. */
    bool toggle_button(std::string_view lbl, bool on, float w = -1.0f) {
        const Id id = get_id(lbl);
        const std::string_view shown = label_text(lbl);
        if (w < 0) w = text_width(shown) + style.padding * 3;
        Box b = next_box(style.row_height, w);
        bool hovered = false, held = false;
        const bool clicked = behavior_(id, b, hovered, held);
        fill_rounded(b, on ? style.accent : held ? style.button_active : hovered ? style.button_hover : style.button);
        text_in(b, shown, style.text, 0.0f, true);
        set_last_(id, b, hovered);
        return clicked;
    }

    /** @brief A checkbox with a label to its right. @return True when toggled. */
    bool checkbox(std::string_view lbl, bool* value) {
        const Id id = get_id(lbl);
        const std::string_view shown = label_text(lbl);
        const float box = style.row_height - 8;
        Box b = next_box(style.row_height, box + (shown.empty() ? 0 : style.spacing * 2 + text_width(shown)));
        bool hovered = false, held = false;
        const bool clicked = behavior_(id, b, hovered, held);
        if (clicked) *value = !*value;
        Box cb{b.x, b.y + 4, box, box};
        draw_check_(cb, *value, hovered);
        if (!shown.empty()) text_in({cb.right() + style.spacing * 2, b.y, b.w, b.h}, shown, style.text, 0.0f);
        set_last_(id, b, hovered);
        return clicked;
    }

    /** @brief A full-width row that can be selected. @return True when clicked. */
    bool selectable(std::string_view lbl, bool selected, float w = 0.0f, Icon ic = Icon::None) {
        const Id id = get_id(lbl);
        Box b = next_box(style.row_height, w);
        bool hovered = false, held = false;
        const bool clicked = behavior_(id, b, hovered, held);
        if (selected) fill_rounded(b, style.selection, 3);
        else if (hovered) fill_rounded(b, style.row_hover, 3);
        float x = b.x;
        if (ic != Icon::None) { icon(ic, {b.x + 3, b.y + 3, b.h - 6, b.h - 6}, style.text); x += b.h; }
        text_in({x, b.y, b.right() - x, b.h}, label_text(lbl), style.text, ic != Icon::None ? 2.0f : -1.0f);
        set_last_(id, b, hovered);
        return clicked;
    }

    /**
     * @brief A large tile (asset browser grid): an icon over a label. @return True when clicked.
     */
    bool tile(std::string_view id_str, Icon ic, std::string_view lbl, bool selected, float w, float h, const glm::vec4* tint = nullptr) {
        const Id id = get_id(id_str);
        Box b = next_box(h, w);
        bool hovered = false, held = false;
        const bool clicked = behavior_(id, b, hovered, held);
        if (selected) fill_rounded(b, style.selection_dim, 4);
        else if (hovered) fill_rounded(b, style.row_hover, 4);
        const float is = std::min(b.w - 16, b.h - 26);
        fill_rounded({b.x + (b.w - is) * 0.5f - 2, b.y + 4, is + 4, is + 4}, with_alpha(style.separator, style.separator.a * 0.5f), 4);
        icon(ic, {b.x + (b.w - is) * 0.5f + is * 0.15f, b.y + 6 + is * 0.15f, is * 0.7f, is * 0.7f}, tint ? *tint : style.text);
        std::string shown(label_text(lbl));
        while (shown.size() > 3 && text_width(shown) > b.w - 6) shown = shown.substr(0, shown.size() - 4) + "..";
        text_in({b.x, b.bottom() - 20, b.w, 18}, shown, selected ? style.text : style.text_dim, 0, true);
        set_last_(id, b, hovered);
        return clicked;
    }

    /** @brief A horizontal progress/value bar. */
    void progress(float t, std::string_view overlay = {}) {
        Box b = next_box(style.row_height - 4);
        fill_rounded(b, style.field);
        fill_rounded({b.x, b.y, b.w * std::clamp(t, 0.0f, 1.0f), b.h}, style.accent);
        if (!overlay.empty()) text_in(b, overlay, style.text, 0.0f, true);
    }

    // -------------------------------------------------------------------------------
    // Property rows: "Label | widget" with a shared label column
    // -------------------------------------------------------------------------------

    /**
     * @brief Draws a property label in the left column and returns the widget box on the
     *        right. Everything in the inspector goes through this so columns line up.
     */
    Box property_row(std::string_view lbl, float h = 0.0f) {
        if (h <= 0) h = style.row_height;
        Box row = next_box(h);
        const float lw = std::floor(row.w * style.label_ratio);
        const std::string_view shown = label_text(lbl);
        if (!shown.empty()) {
            // Blender right-aligns property labels against the widget column (label_align 1);
            // forms flush them left (0).
            const float tw = text_width(shown);
            const float x = row.x + std::max(0.0f, lw - 8 - tw) * std::clamp(style.label_align, 0.0f, 1.0f);
            text_in({x, row.y, row.x + lw - 6 - x, style.row_height}, shown, style.text_dim, 0.0f);
        }
        last_label_box_ = {row.x, row.y, lw, h};
        return shown.empty() && lbl.find("##") == 0 ? row : Box{row.x + lw, row.y, row.w - lw, h};
    }

    /**
     * @brief A draggable number: drag horizontally to scrub, click (no drag) to type.
     * @return True on any change (every drag step, and a committed typed value).
     */
    bool drag_float(std::string_view lbl, float* v, float speed = 0.01f, float lo = -1e30f, float hi = 1e30f,
                    const char* fmt = "%.3f") {
        Box b = property_row(lbl);
        return drag_float_in_(get_id(lbl), b, v, speed, lo, hi, fmt, nullptr);
    }
    bool drag_int(std::string_view lbl, int* v, float speed = 0.2f, int lo = -1000000000, int hi = 1000000000) {
        float f = static_cast<float>(*v);
        Box b = property_row(lbl);
        const bool changed = drag_float_in_(get_id(lbl), b, &f, speed, static_cast<float>(lo), static_cast<float>(hi), "%.0f", nullptr);
        if (changed) *v = static_cast<int>(std::lround(f));
        return changed;
    }
    /** @brief N floats in one row (vectors), each with an axis-coloured tick. */
    bool drag_floatn(std::string_view lbl, float* v, int n, float speed = 0.01f, const char* fmt = "%.3f",
                     float lo = -1e30f, float hi = 1e30f) {
        Box b = property_row(lbl);
        push_id(lbl);
        bool changed = false;
        const float gap = 3.0f;
        const float w = (b.w - gap * (n - 1)) / n;
        const glm::vec4* axis[4] = {&style.axis_x, &style.axis_y, &style.axis_z, &style.text_dim};
        bool any_active = false, any_deact = false;
        for (int i = 0; i < n; ++i) {
            Box fb{b.x + i * (w + gap), b.y, w, b.h};
            changed |= drag_float_in_(get_id(static_cast<int64_t>(i)), fb, &v[i], speed, lo, hi, fmt, axis[i]);
            any_active |= last_active();
            any_deact |= last_item_.deactivated;
        }
        pop_id();
        set_last_(get_id(lbl), b, b.contains(in_.mouse));
        last_item_.deactivated = any_deact;
        group_active_ = any_active;
        return changed;
    }
    /**
     * @brief Blender's stacked vector block (Location / Rotation / Scale): the label on the
     *        first row's left column, one field per component below each other, joined into a
     *        single rounded column with "X", "Y", "Z" inside each field.
     */
    bool drag_float_stacked(std::string_view lbl, float* v, int n, float speed = 0.01f, const char* fmt = "%.3f",
                            const char* const* names = nullptr, float lo = -1e30f, float hi = 1e30f) {
        static const char* xyz[] = {"X", "Y", "Z", "W"};
        if (!names) names = xyz;
        push_id(lbl);
        bool changed = false, any_active = false, any_deact = false;
        Box first{};
        for (int i = 0; i < n; ++i) {
            Box b = property_row(i == 0 ? lbl : std::string_view("##"), style.row_height - 1);
            if (i == 0) first = b;
            b.y -= i * (style.spacing - 1);   // join the rows into one block
            int corners = 0;
            if (i == 0) corners |= kTop;
            if (i == n - 1) corners |= kBottom;
            stacked_corners_ = corners;
            stacked_label_ = names[i];
            changed |= drag_float_in_(get_id(static_cast<int64_t>(i)), b, &v[i], speed, lo, hi, fmt, nullptr);
            stacked_corners_ = -1;
            stacked_label_ = nullptr;
            any_active |= last_active();
            any_deact |= last_item_.deactivated;
            if (i + 1 < n) fill({b.x, b.bottom() - 0.5f, b.w, 1}, style.separator);
        }
        layout_().cursor.y -= (n - 1) * (style.spacing - 1);
        pop_id();
        set_last_(get_id(lbl), first, first.contains(in_.mouse));
        last_item_.deactivated = any_deact;
        group_active_ = any_active;
        spacing(3);
        return changed;
    }

    /** @brief True while any sub-field of the last drag_floatn() is being dragged/edited. */
    bool last_group_active() const { return group_active_; }

    bool slider_float(std::string_view lbl, float* v, float lo, float hi, const char* fmt = "%.3f") {
        Box b = property_row(lbl);
        const Id id = get_id(lbl);
        bool hovered = false, held = false;
        behavior_(id, b, hovered, held);
        bool changed = false;
        if (held && b.w > 0) {
            const float t = std::clamp((in_.mouse.x - b.x) / b.w, 0.0f, 1.0f);
            const float nv = lo + (hi - lo) * t;
            if (nv != *v) { *v = nv; changed = true; }
        }
        fill_rounded(b, hovered || held ? style.number_hover : style.number);
        const float t = hi > lo ? std::clamp((*v - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
        if (t > 0) fill_rounded({b.x, b.y, std::min(b.w, std::max(b.h * 0.5f, b.w * t)), b.h}, style.accent, -1, t > 0.98f ? kAll : kLeft);
        char buf[64];
        std::snprintf(buf, sizeof(buf), fmt, *v);
        text_in(b, buf, style.text, 0.0f, true);
        set_last_(id, b, hovered);
        return changed;
    }

    /**
     * @brief A small set of choices as a row of joined buttons: every option readable at a
     *        glance and one click away, where a dropdown hides them behind a popup.
     * @return True when a different item was picked.
     */
    bool segmented(std::string_view lbl, int* index, const std::vector<std::string>& items) {
        Box b = property_row(lbl);
        return segmented_in(get_id(lbl), b, index, items);
    }

    /** @brief segmented() in a box placed by the caller. */
    bool segmented_in(Id id, const Box& b, int* index, const std::vector<std::string>& items) {
        if (items.empty() || b.w <= 0) return false;
        push_id(id);
        bool changed = false;
        bool row_hovered = false;
        const int n = static_cast<int>(items.size());
        const float w = b.w / static_cast<float>(n);
        for (int i = 0; i < n; ++i) {
            const Box cell{b.x + static_cast<float>(i) * w, b.y, w - (i + 1 < n ? 1.0f : 0.0f), b.h};
            bool hovered = false, held = false;
            push_id(static_cast<int64_t>(i));
            const bool clicked = invisible_button("seg", cell, &hovered, &held);
            pop_id();
            row_hovered |= hovered;
            const bool on = i == *index;
            int corners = 0;
            if (n == 1) corners = kAll;
            else if (i == 0) corners = kLeft;
            else if (i + 1 == n) corners = kRight;
            fill_rounded(cell, on ? style.button_active : hovered || held ? style.button_hover : style.button, -1, corners);
            text_in(cell, items[static_cast<size_t>(i)], on ? style.text : style.text_dim, 0.0f, true);
            if (clicked && !on) { *index = i; changed = true; }
        }
        pop_id();
        set_last_(id, b, row_hovered);   // a tooltip after this covers the whole row, not one cell
        return changed;
    }

    /**
     * @brief A low and a high value on one track -- a span, rather than two numbers that happen
     *        to sit next to each other. Each handle stops at the other, so it cannot invert.
     * @return True on any change.
     */
    bool range_slider(std::string_view lbl, float* lo, float* hi, float min, float max, const char* fmt = "%.2f") {
        Box b = property_row(lbl);
        const Id id = get_id(lbl);
        push_id(lbl);
        const float span = max - min;
        auto pos = [&](float v) { return b.x + (span > 0 ? std::clamp((v - min) / span, 0.0f, 1.0f) : 0.0f) * b.w; };
        auto value_at = [&](float x) { return min + span * std::clamp((x - b.x) / std::max(1.0f, b.w), 0.0f, 1.0f); };
        bool changed = false;
        const float grab = std::max(8.0f, b.h * 0.6f);
        bool lo_hot = false, lo_held = false, hi_hot = false, hi_held = false;
        invisible_button("lo", {pos(*lo) - grab * 0.5f, b.y, grab, b.h}, &lo_hot, &lo_held);
        if (lo_held) {
            const float v = std::min(value_at(in_.mouse.x), *hi);
            if (v != *lo) { *lo = v; changed = true; }
        }
        invisible_button("hi", {pos(*hi) - grab * 0.5f, b.y, grab, b.h}, &hi_hot, &hi_held);
        if (hi_held) {
            const float v = std::max(value_at(in_.mouse.x), *lo);
            if (v != *hi) { *hi = v; changed = true; }
        }
        fill_rounded(b, style.number);
        const float x0 = pos(*lo), x1 = pos(*hi);
        fill_rounded({x0, b.y, std::max(2.0f, x1 - x0), b.h}, style.accent);
        fill_rounded({x0 - 2.0f, b.y + 1.0f, 4.0f, b.h - 2.0f}, lo_hot || lo_held ? style.text : style.text_dim, 2.0f);
        fill_rounded({x1 - 2.0f, b.y + 1.0f, 4.0f, b.h - 2.0f}, hi_hot || hi_held ? style.text : style.text_dim, 2.0f);
        char buf[96];
        const std::string both = std::string(fmt) + "  ..  " + fmt;
        std::snprintf(buf, sizeof(buf), both.c_str(), *lo, *hi);
        text_in(b, buf, style.text, 0.0f, true);
        pop_id();
        set_last_(id, b, lo_hot || hi_hot || hoverable_(b));
        return changed;
    }

    /** @brief A checkbox in the property column. */
    bool property_bool(std::string_view lbl, bool* v) {
        Box b = property_row(lbl);
        const Id id = get_id(lbl);
        bool hovered = false, held = false;
        const bool clicked = behavior_(id, {b.x, b.y, b.h, b.h}, hovered, held);
        if (clicked) *v = !*v;
        Box cb{b.x, b.y + 4, b.h - 8, b.h - 8};
        draw_check_(cb, *v, hovered);
        set_last_(id, b, hovered);
        return clicked;
    }

    /**
     * @brief A single-line text field. Edits a private buffer while focused; the value only
     *        changes on commit (Enter, Tab, or clicking away). Escape cancels.
     * @return True when a changed value was committed.
     */
    bool input_text(std::string_view lbl, std::string* value, bool property = true) {
        Box b = property ? property_row(lbl) : next_box(style.row_height);
        return input_text_in_(get_id(lbl), b, value, false);
    }

    /** @brief A text field in an explicit box (toolbars, search fields). */
    /** @brief A number field at an explicit rect (headers, toolbars); drag or click to type. */
    bool drag_float_box(std::string_view id_str, const Box& b, float* v, float speed = 0.01f, float lo = -1e30f, float hi = 1e30f,
                        const char* fmt = "%.3f") {
        const Id id = get_id(id_str);
        const bool changed = drag_float_in_(id, b, v, speed, lo, hi, fmt, nullptr);
        return changed;
    }

    bool input_text_box(std::string_view id_str, const Box& b, std::string* value, std::string_view placeholder = {}) {
        const bool r = input_text_in_(get_id(id_str), b, value, true);
        if (value->empty() && !(text_edit_.active && text_edit_.id == get_id(id_str)) && !placeholder.empty()) {
            text_in(b, placeholder, style.text_disabled);
        }
        return r;
    }

    /**
     * @brief A dropdown. @return True when the selection changed.
     * @param hovered When non-null, set to the item under the mouse in the open list, or -1
     *                (closed, nothing hovered, or the frame an item is chosen) -- for a live
     *                preview of the choice, as a font menu previews the hovered font.
     */
    bool combo(std::string_view lbl, int* index, const std::vector<std::string>& items, int* hovered = nullptr) {
        Box b = property_row(lbl);
        return combo_in_(get_id(lbl), b, index, items, hovered);
    }
    /** @brief combo() in an explicit box. */
    bool combo_box(std::string_view id_str, const Box& b, int* index, const std::vector<std::string>& items,
                   int* hovered = nullptr) {
        return combo_in_(get_id(id_str), b, index, items, hovered);
    }

    /**
     * @brief An RGB(A) colour: a swatch (click for an HSV picker popup) plus numeric fields.
     * @return True on any change.
     */
    bool color_edit(std::string_view lbl, float* rgba, bool alpha = false) {
        Box b = property_row(lbl);
        const Id id = get_id(lbl);
        push_id(lbl);
        bool changed = false;
        const float sw = std::min(44.0f, b.w * 0.3f);
        Box swatch{b.x, b.y + 2, sw, b.h - 4};
        bool hovered = false, held = false;
        const Id sid = get_id("swatch");
        const bool clicked = behavior_(sid, swatch, hovered, held);
        fill_rounded(swatch, glm::vec4(glm::clamp(glm::vec3(rgba[0], rgba[1], rgba[2]), 0.0f, 1.0f), 1.0f));
        outline_rounded(swatch, hovered ? style.text_dim : style.border);
        if (clicked) open_popup("picker", glm::vec2(swatch.x, swatch.bottom() + 2));
        const int n = alpha ? 4 : 3;
        const float gap = 3.0f;
        const float fx = swatch.right() + gap;
        const float w = (b.right() - fx - gap * (n - 1)) / n;
        const glm::vec4* axis[4] = {&style.axis_x, &style.axis_y, &style.axis_z, &style.text_dim};
        bool any_active = false, any_deact = false;
        for (int i = 0; i < n; ++i) {
            Box fb{fx + i * (w + gap), b.y, w, b.h};
            changed |= drag_float_in_(get_id(static_cast<int64_t>(i)), fb, &rgba[i], 0.005f, 0.0f, 64.0f, "%.3f", axis[i]);
            any_active |= last_active();
            any_deact |= last_item_.deactivated;
        }
        if (begin_popup("picker")) {
            changed |= color_picker_(rgba, any_active, any_deact);
            end_popup();
        }
        pop_id();
        set_last_(id, b, b.contains(in_.mouse));
        last_item_.deactivated = any_deact;
        group_active_ = any_active;
        return changed;
    }

    /**
     * @brief A full-width foldout header.
     * @return True while open (emit the section's contents).
     */
    bool collapsing_header(std::string_view lbl, bool default_open = true, bool* remove_clicked = nullptr,
                           Icon ic = Icon::None, bool* enabled = nullptr) {
        const Id id = get_id(lbl);
        State& st = state_(id);
        if (!st.init) { st.init = true; st.b[0] = default_open; }
        Box b = next_box(style.row_height + 3);
        bool hovered = false, held = false;
        Box toggle = b;
        float x = b.x + style.row_height;
        if (enabled) x += style.row_height - 4;
        if (ic != Icon::None) x += style.row_height - 2;
        if (remove_clicked) toggle.w -= style.row_height;
        if (enabled) { toggle.x += style.row_height + style.row_height - 4; toggle.w -= style.row_height + style.row_height - 4; }
        if (behavior_(id, toggle, hovered, held)) st.b[0] = !st.b[0];
        // Blender sub-panel: rounded header, darker body when open.
        fill_rounded(b, hovered ? style.header_hover : style.header, -1, st.b[0] ? kTop : kAll);
        arrow({b.x + 3, b.y, style.row_height - 6, b.h}, st.b[0], style.text_dim);
        float cx = b.x + style.row_height;
        if (enabled) {
            const Box cb{cx, b.y + (b.h - (style.row_height - 8)) * 0.5f, style.row_height - 8, style.row_height - 8};
            bool eh = false, eheld = false;
            if (behavior_(hash_int(id, 9), cb.shrink(-2), eh, eheld)) *enabled = !*enabled;
            draw_check_(cb, *enabled, eh);
            cx += style.row_height - 4;
        }
        if (ic != Icon::None) { icon(ic, {cx, b.y + 4, b.h - 8, b.h - 8}, style.text); cx += style.row_height - 2; }
        text_in({cx, b.y, b.right() - cx, b.h}, label_text(lbl), style.text, 2.0f);
        set_last_(id, b, hovered);
        if (remove_clicked) {
            Box xb{b.right() - style.row_height, b.y + 1, style.row_height - 2, b.h - 2};
            bool xh = false, xheld = false;
            *remove_clicked = behavior_(hash_int(id, 3), xb, xh, xheld);
            if (xh) fill_rounded(xb, style.hover_tint);
            icon(Icon::X, xb.shrink(5), xh ? style.text : style.text_dim);
        }
        (void)x;
        return st.b[0];
    }

    /**
     * @brief A tree row: expander arrow (unless `leaf`), label, selection highlight.
     *        When the result is open, emit children then call tree_pop().
     */
    TreeNodeResult tree_node(Id id, std::string_view lbl, bool leaf, bool selected, bool default_open = false,
                             const glm::vec4* color = nullptr, Icon ic = Icon::None, float reserve_right = 0.0f,
                             bool active = false, const glm::vec4* icon_color = nullptr) {
        State& st = state_(id);
        if (!st.init) { st.init = true; st.b[0] = default_open; }
        Box b = next_box(style.row_height);
        const Layout& l = layout_();
        Box full{l.region.x, b.y, l.region.w, b.h};   // highlight spans the whole row
        const float ax = b.x;
        Box arrow_box{ax, b.y, style.row_height - 6, b.h};
        TreeNodeResult r;
        r.rect = full;
        Box hit = full;
        hit.w -= reserve_right;   // row buttons (eye, camera) at the right handle their own clicks
        const bool hovered = hoverable_(hit, id);
        if (hovered) hot_id_ = id;
        if (selected) fill(full, active ? style.selection : style.selection_dim);
        else if (hovered) fill(full, style.row_hover);
        // Tree guide line from the parent's arrow down to this row (Blender's outliner).
        if (layout_().indent > 0.5f) {
            const float gx = b.x - style.indent * 0.5f - 1;
            fill({gx, b.y - style.spacing, 1, b.h * 0.5f + style.spacing}, with_alpha(style.text_dim, 0.18f));
        }
        if (!leaf) {
            arrow(arrow_box, st.b[0], style.text_dim);
            if (hovered && in_.pressed[0] && arrow_box.contains(in_.mouse)) st.b[0] = !st.b[0];
        }
        const bool on_arrow = !leaf && arrow_box.contains(in_.mouse);
        (void)hit;
        if (hovered && in_.pressed[0] && !on_arrow) {
            active_id_ = id;
            press_pos_[0] = in_.mouse;
        }
        if (hovered && in_.released[0] && !on_arrow && press_origin_hit_(Mouse::Left, full) && !drag_.active) {
            r.clicked = true;
            if (last_click_id_ == id && time_ - last_click_time_ < 0.35) { r.double_clicked = true; last_click_id_ = 0; }
            else { last_click_id_ = id; last_click_time_ = time_; }
        }
        if (hovered && in_.released[1]) r.right_clicked = true;
        float tx = arrow_box.right();
        if (ic != Icon::None) {
            icon(ic, {tx + 1, b.y + 3, b.h - 6, b.h - 6}, icon_color ? *icon_color : color ? *color : style.text);
            tx += b.h;
        }
        text_in({tx, b.y, full.right() - reserve_right - tx, b.h}, lbl, color ? *color : style.text, 2.0f);
        set_last_(id, full, hovered);
        r.open = !leaf && st.b[0];
        if (r.open) indent();
        return r;
    }
    void tree_pop() { unindent(); }
    /** @brief Whether tree node `id` is open (its last state; `default_open` before it was first drawn),
     *         without drawing it -- lets a long list skip rows that are off screen and collapsed. */
    bool tree_is_open(Id id, bool default_open = false) const {
        auto it = states_.find(id);
        return it != states_.end() && it->second.init ? it->second.b[0] : default_open;
    }
    /** @brief Whether a row of height `h` at the cursor would be entirely outside the clip rect. */
    bool next_row_clipped(float h) const {
        const float y = layout_stack_.back().cursor.y;
        const Box c = current_clip();
        return y + h < c.y || y > c.bottom();
    }
    /** @brief Forces a tree node's (or collapsing header's) open state. */
    void set_open(Id id, bool open) { State& st = state_(id); st.init = true; st.b[0] = open; }

    /**
     * @brief A row of tabs across `b`. @return True when the active tab changed.
     */
    bool tab_bar(std::string_view id_str, const Box& b, const std::vector<std::string>& tabs, int* active,
                 const std::vector<Icon>* icons = nullptr, bool fill_bg = true) {
        push_id(id_str);
        if (fill_bg) fill(b, style.panel_alt);
        float x = b.x + 4;
        bool changed = false;
        for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
            const Icon ic = icons && i < static_cast<int>(icons->size()) ? (*icons)[static_cast<size_t>(i)] : Icon::None;
            const float iw = ic != Icon::None ? b.h - 8 : 0.0f;
            const float w = text_width(tabs[i]) + style.padding * 3 + (iw > 0 ? iw + 4 : 0);
            Box t{x, b.y + 3, w, b.h - 3};
            const Id id = get_id(static_cast<int64_t>(i));
            bool hovered = false, held = false;
            if (behavior_(id, t, hovered, held) && *active != i) { *active = i; changed = true; }
            const bool on = *active == i;
            if (on || hovered) fill_rounded(t, on ? style.header : style.row_hover, -1, kTop);
            float tx = t.x + style.padding * 1.5f;
            if (ic != Icon::None) { icon(ic, {tx, t.y + 3, iw, t.h - 6}, on ? style.text : style.text_dim); tx += iw + 4; }
            text_in({tx, t.y, t.right() - tx, t.h}, tabs[i], on ? style.text : style.text_dim, 0.0f);
            x += w + 2;
        }
        pop_id();
        return changed;
    }

    /**
     * @brief Blender's Properties editor tab strip: a vertical column of icon tabs.
     * @return True when the active tab changed.
     */
    bool vertical_tabs(std::string_view id_str, const Box& b, const std::vector<std::pair<Icon, std::string>>& tabs, int* active,
                       const std::vector<int>& group_breaks = {}) {
        push_id(id_str);
        fill(b, style.panel_alt);
        const float s = std::min(b.w - 6, style.row_height + 5);
        float y = b.y + 5;
        bool changed = false;
        for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
            if (std::find(group_breaks.begin(), group_breaks.end(), i) != group_breaks.end()) y += 8;
            const Box t{b.x + (b.w - s) * 0.5f, y, s, s};
            if (icon_button(tabs[i].second, tabs[i].first, tabs[i].second, *active == i, s, kAll, t)) {
                if (*active != i) { *active = i; changed = true; }
            }
            y += s + 2;
        }
        pop_id();
        return changed;
    }

    /**
     * @brief A draggable divider. Drag changes `*value` by the mouse delta (pixels along the
     *        divider's normal), clamped to [lo, hi].
     * @param vertical True for a vertical bar (drag horizontally).
     */
    DragState splitter(std::string_view id_str, const Box& bar, bool vertical, float* value, float lo, float hi) {
        const Id id = get_id(id_str);
        bool hovered = false, held = false;
        behavior_(id, bar, hovered, held);
        DragState r;
        r.active = held;
        if (held) {
            const float d = vertical ? in_.mouse_delta.x : in_.mouse_delta.y;
            const float nv = std::clamp(*value + d, lo, hi);
            if (nv != *value) { *value = nv; r.changed = true; }
        }
        fill(bar, held ? style.accent : hovered ? style.header_hover : style.border);
        // A border you can drag shows the resize arrows (vertical bar: left-right).
        if (hovered || held) set_mouse_cursor(vertical ? coopa::input::CursorShape::ResizeH : coopa::input::CursorShape::ResizeV);
        return r;
    }

    /**
     * @brief The mouse cursor shape the UI wants this frame (Arrow unless a widget asked otherwise
     *        -- splitters ask for resize arrows, text fields for an I-beam). The host applies
     *        it to the window after the frame (e.g. Input::set_cursor_shape()).
     */
    coopa::input::CursorShape mouse_cursor() const { return cursor_; }
    void set_mouse_cursor(coopa::input::CursorShape c) { cursor_ = c; }

    // -------------------------------------------------------------------------------
    // Popups, menus, modals
    // -------------------------------------------------------------------------------

    /** @brief Opens the popup `name` (scoped to the current id stack) at `pos` (default: mouse). */
    void open_popup(std::string_view name, std::optional<glm::vec2> pos = std::nullopt) {
        const Id id = get_id(name);
        open_popup_id_(id, pos.value_or(in_.mouse), false, false);
    }
    /** @brief Opens a popup when the last item is right-clicked. @return True if it opened. */
    bool open_context_popup_on_last(std::string_view name) {
        if (last_item_.hovered && in_.released[1]) { open_popup(name); return true; }
        return false;
    }
    /** @brief The label column of the last property_row() (an inspector field's name). */
    const Box& last_label_box() const { return last_label_box_; }
    /** @brief Opens a popup when `area` is right-clicked over nothing else. */
    bool open_context_popup_in(std::string_view name, const Box& area) {
        if (hoverable_(area) && in_.released[1] && hot_id_ == 0) { open_popup(name); return true; }
        return false;
    }
    bool is_popup_open(std::string_view name) const { return find_popup_(get_id(name)) >= 0; }

    /** @brief Begins drawing popup `name` if open. Pair with end_popup() only when it returns true. */
    bool begin_popup(std::string_view name, float min_w = 160.0f) {
        return begin_popup_id_(get_id(name), min_w);
    }
    void end_popup() { end_popup_(); }
    /** @brief Closes the innermost popup currently being drawn (and its children). */
    void close_current_popup() {
        if (popup_draw_stack_.empty()) return;
        const int idx = find_popup_(popup_draw_stack_.back());
        if (idx >= 0) open_popups_.resize(static_cast<size_t>(idx));
    }
    void close_all_popups() {
        while (!open_popups_.empty() && !open_popups_.back().modal) open_popups_.pop_back();
    }

    /** @brief Opens a modal dialog (centred, blocks everything else) by name. */
    void open_modal(std::string_view name) {
        open_popup_id_(get_id(name), size_ * 0.5f, true, false);
    }
    /**
     * @brief Draws modal `name` if open: dims the screen, centres a titled box of `size`.
     * @return True while open; call end_modal() then.
     */
    /**
     * @param box_size Width x height; a height <= 0 fits the box to its content (measured the
     *                 frame before -- the first frame uses a few rows' guess).
     */
    bool begin_modal(std::string_view name, glm::vec2 box_size) {
        const Id id = get_id(name);
        const int idx = find_popup_(id);
        if (idx < 0) return false;
        PopupState& p = open_popups_[static_cast<size_t>(idx)];
        p.seen_frame = frame_;
        p.fit = box_size.y <= 0.0f;
        if (p.fit) box_size.y = p.fit_h > 0.0f ? p.fit_h : style.row_height * 5.0f;
        const int z = kPopupZ + idx * 10;
        dl_->set_z_order(z);
        push_clip_raw_(Box{0, 0, size_.x, size_.y});
        fill(Box{0, 0, size_.x, size_.y}, style.modal_dim);
        Box b{std::floor((size_.x - box_size.x) * 0.5f), std::floor((size_.y - box_size.y) * 0.5f), box_size.x, box_size.y};
        shadow(b, 8);
        fill_rounded(b, style.panel_bg, 6);
        outline_rounded(b, style.edge_highlight, 6);
        Box bar{b.x, b.y, b.w, style.row_height + 2};
        fill_rounded(bar, style.header, 6, kTop);
        text_in(bar, label_text(name), style.text);
        layer_stack_.push_back({id, z});
        layers_.push_back({id, b, z, true});
        popup_draw_stack_.push_back(id);
        Layout l{Box{b.x, bar.bottom(), b.w, b.h - bar.h}.shrink(style.padding)};
        l.cursor = l.region.pos();
        l.content_top = l.content_bottom = l.cursor.y;
        layout_stack_.push_back(l);
        clip_stack_.push_back(b);
        apply_clip_();
        push_id(name);
        return true;
    }
    void end_modal() {
        pop_id();
        {
            const Layout& l = layout_stack_.back();
            const int idx = find_popup_(layer_stack_.back().id);
            if (idx >= 0 && open_popups_[static_cast<size_t>(idx)].fit) {
                const float content = std::max(0.0f, l.content_bottom - l.content_top);
                open_popups_[static_cast<size_t>(idx)].fit_h = std::ceil(content + style.padding * 2.0f + style.row_height + 2.0f);
            }
        }
        layout_stack_.pop_back();
        pop_clip_();
        pop_clip_();
        popup_draw_stack_.pop_back();
        layer_stack_.pop_back();
        dl_->set_z_order(layer_stack_.back().z);
    }
    /** @brief Closes the modal currently being drawn. */
    void close_modal() { close_current_popup(); }

    /** @brief A menu bar across `b`. Always returns true; pair with end_menubar(). */
    bool begin_menubar(const Box& b, bool background = true) {
        if (background) {
            fill(b, style.panel_alt);
            fill({b.x, b.bottom() - 1, b.w, 1}, style.border);
        }
        menubar_ = MenuBarState{};
        menubar_.active = true;
        menubar_.rect = b;
        menubar_.x = b.x + 4;
        return true;
    }
    void end_menubar() { menubar_.active = false; }
    /** @brief Where the last menubar's headers ended (x in imm space) -- for widgets after it. */
    float menubar_end() const { return menubar_.x; }

    /**
     * @brief A menu: a header in a menu bar, or a submenu row inside another menu.
     * @return True while its popup is open; pair with end_menu() then.
     */
    bool begin_menu(std::string_view lbl, bool enabled = true, Icon ic = Icon::None) {
        const Id id = get_id(lbl);
        const std::string_view shown = label_text(lbl);
        if (menubar_.active && popup_draw_stack_.empty()) {
            const float w = text_width(shown) + style.padding * 3;
            Box h{menubar_.x, menubar_.rect.y, w, menubar_.rect.h - 1};
            menubar_.x += w;
            const bool hovered = enabled && hoverable_(h);
            const bool open = find_popup_(id) == 0;
            const bool any_menubar_open = !open_popups_.empty() && open_popups_.front().from_menubar;
            if (hovered && in_.pressed[0]) {
                const bool just_closed = std::find(closed_by_click_.begin(), closed_by_click_.end(), id) != closed_by_click_.end();
                if (open) open_popups_.clear();
                else if (!just_closed) { open_popups_.clear(); open_popup_id_(id, {h.x, h.bottom()}, false, true); }
            } else if (hovered && any_menubar_open && !open) {
                open_popups_.clear();
                open_popup_id_(id, {h.x, h.bottom()}, false, true);
            }
            const bool now_open = find_popup_(id) == 0;
            fill_rounded(h.shrink(2), now_open ? style.header_hover : hovered ? style.hover_tint : glm::vec4(0));
            text_in(h, shown, enabled ? style.text : style.text_disabled, 0.0f, true);
            set_last_(id, h, hovered);
            if (!now_open) return false;
            return begin_popup_id_(id, 200.0f);
        }
        // Submenu row inside a menu.
        Box row = next_box(style.row_height);
        const bool hovered = enabled && hoverable_(row);
        const int depth = static_cast<int>(popup_draw_stack_.size());
        if (hovered) {
            const int idx = find_popup_(id);
            if (idx < 0) {
                open_popups_.resize(static_cast<size_t>(std::min<int>(depth, static_cast<int>(open_popups_.size()))));
                open_popup_id_(id, {row.right() + 2, row.y - style.padding}, false, open_popups_.front().from_menubar);
            }
        }
        const bool open = find_popup_(id) >= 0;
        if (open || hovered) fill_rounded(row, style.accent, 3);
        if (ic != Icon::None) icon(ic, {row.x + 4, row.y + 3, row.h - 6, row.h - 6}, enabled ? style.text : style.text_disabled);
        text_in({row.x + style.row_height + 2, row.y, row.w, row.h}, shown, enabled ? style.text : style.text_disabled, 0.0f);
        arrow({row.right() - row.h, row.y, row.h, row.h}, false, style.text_dim);
        set_last_(id, row, hovered);
        if (!open) return false;
        return begin_popup_id_(id, 180.0f);
    }
    void end_menu() { end_popup_(); }

    /**
     * @brief A menu entry. @return True when chosen (the menus then close).
     * @param shortcut Right-aligned hint text, e.g. "Ctrl+S".
     * @param checked  Draws a check mark when non-null and true.
     */
    bool menu_item(std::string_view lbl, std::string_view shortcut_text = {}, const bool* checked = nullptr,
                   bool enabled = true, Icon ic = Icon::None) {
        const Id id = get_id(lbl);
        Box row = next_box(style.row_height);
        const bool hovered = enabled && hoverable_(row);
        if (hovered) {
            // Hovering a plain item closes any sibling submenu.
            const int depth = static_cast<int>(popup_draw_stack_.size());
            if (static_cast<int>(open_popups_.size()) > depth) open_popups_.resize(static_cast<size_t>(depth));
            fill_rounded(row, style.accent, 3);
        }
        const float check_w = style.row_height + 2;
        if (checked) draw_check_({row.x + 5, row.y + 5, row.h - 10, row.h - 10}, *checked, false);
        else if (ic != Icon::None) icon(ic, {row.x + 4, row.y + 3, row.h - 6, row.h - 6}, enabled ? style.text : style.text_disabled);
        text_in({row.x + check_w, row.y, row.w - check_w, row.h}, label_text(lbl), enabled ? style.text : style.text_disabled, 0.0f);
        if (!shortcut_text.empty()) {
            const float sw = text_width(shortcut_text);
            text_in({row.right() - sw - style.padding * 2, row.y, sw + style.padding, row.h}, shortcut_text, style.text_dim, 0.0f);
        }
        set_last_(id, row, hovered);
        const bool chosen = hovered && in_.released[0];
        if (chosen) close_all_popups();
        return chosen;
    }
    void menu_separator() {
        Box b = next_box(5);
        fill({b.x + 4, b.y + 2, b.w - 8, 1}, style.edge_highlight);
    }

    // -------------------------------------------------------------------------------
    // Drag and drop
    // -------------------------------------------------------------------------------

    /**
     * @brief Makes the last item a drag source: once it is pressed and the mouse moves a few
     *        pixels, `payload` (of `type`) follows the cursor labelled `label`.
     * @return True while this item's payload is being dragged.
     */
    bool drag_source(std::string_view type, const std::string& payload, std::string_view label = {}) {
        const Id id = last_item_.id;
        if (!id) return false;
        if (drag_.active) return drag_.source == id;
        if (last_item_.hovered && in_.pressed[0]) { drag_candidate_ = id; press_pos_[0] = in_.mouse; }
        if (drag_candidate_ == id && in_.down[0] && glm::distance(in_.mouse, press_pos_[0]) > 5.0f) {
            drag_ = DragPayload{true, id, std::string(type), payload, std::string(label.empty() ? payload : label)};
            drag_candidate_ = 0;
            return true;
        }
        return false;
    }
    /** @brief The payload type being dragged, or empty. */
    std::string_view dragging_type() const { return drag_.active ? std::string_view(drag_.type) : std::string_view(); }

    /**
     * @brief Makes `area` (default: the last item) a drop target for payloads of `type`.
     * @return The payload when dropped here this frame.
     */
    std::optional<std::string> drop_target(std::string_view type, std::optional<Box> area = std::nullopt) {
        if (!drag_.active || drag_.type != type) return std::nullopt;
        const Box b = area.value_or(last_item_.rect);
        if (!b.contains(in_.mouse) || !current_clip().contains(in_.mouse) || !layer_ok_()) return std::nullopt;
        outline(b, style.accent, 2.0f);
        if (in_.released[0]) {
            std::string payload = drag_.payload;
            drag_ = {};
            return payload;
        }
        return std::nullopt;
    }

    // -------------------------------------------------------------------------------
    // Free-form interaction (custom widgets, the editor viewport)
    // -------------------------------------------------------------------------------

    /**
     * @brief Registers `b` as an interactive area with button behaviour.
     * @return True when clicked (pressed and released inside with the left button).
     */
    bool invisible_button(std::string_view id_str, const Box& b, bool* hovered_out = nullptr, bool* held_out = nullptr) {
        const Id id = get_id(id_str);
        bool hovered = false, held = false;
        const bool clicked = behavior_(id, b, hovered, held);
        if (hovered_out) *hovered_out = hovered;
        if (held_out) *held_out = held;
        set_last_(id, b, hovered);
        return clicked;
    }
    /** @brief True if `b` is under the mouse and nothing (popup, active widget) blocks it. */
    bool is_hovered(const Box& b) const { return hoverable_(b); }

    /**
     * @brief A number a widget keeps between frames, under its own id -- which stop of a
     *        gradient is selected, how far a strip has scrolled.
     *
     * Four slots per id. Use it for presentation state only: anything the document should
     * remember belongs in the document.
     */
    float& persistent(std::string_view id_str, int slot = 0) {
        return state_(get_id(id_str)).f[std::clamp(slot, 0, 3)];
    }


private:
    // --- constants ---
    static constexpr int kPopupZ = 1000;
    static constexpr int kTooltipZ = 100000;

    // --- per-id persistent state ---
    struct State {
        bool  init = false;
        bool  b[4] = {};
        float f[4] = {};
    };
    State& state_(Id id) { return states_[id]; }

    struct Layout {
        Box   region;
        Box   outer;
        glm::vec2 cursor{0.0f};
        float indent = 0.0f;
        float row_h = 0.0f;
        float line_start_y = 0.0f;
        float last_x_end = 0.0f;
        float same_line_x = 0.0f;
        bool  same_line = false;
        bool  scroll = false;
        float* scroll_y = nullptr;
        float content_top = 0.0f;
        float content_bottom = 0.0f;
        float popup_max_w = 0.0f;
        Id    id = 0;

        Layout() = default;
        explicit Layout(const Box& r) : region(r), outer(r), cursor(r.pos()), line_start_y(r.y),
                                        content_top(r.y), content_bottom(r.y) {}
    };
    Layout& layout_() { return layout_stack_.back(); }
    const Layout& layout_() const { return layout_stack_.back(); }
    float cursor_x_() const {
        const Layout& l = layout_();
        return l.same_line ? l.same_line_x : l.region.x + l.indent;
    }

    struct LayerRect { Id id; Box rect; int z; bool modal = false; };
    struct LayerFrame { Id id; int z; };
    struct PopupState {
        Id id = 0;
        glm::vec2 pos{0.0f};
        glm::vec2 size{0.0f};
        bool modal = false;
        bool from_menubar = false;
        uint64_t seen_frame = 0;
        uint64_t opened_frame = 0;
        bool fit = false;      ///< A modal sized to its content (begin_modal height <= 0).
        float fit_h = 0.0f;    ///< That content's box height, measured last frame.
    };
    struct MenuBarState { bool active = false; Box rect; float x = 0.0f; };
    struct LastItem { Id id = 0; Box rect; bool hovered = false; bool deactivated = false; };
    struct DragPayload { bool active = false; Id source = 0; std::string type, payload, label; };
    struct TextEdit {
        bool active = false;
        Id id = 0;
        std::string buffer;
        std::string original;
        size_t caret = 0;
        bool select_all = false;
        float scroll_x = 0.0f;
        uint64_t seen_frame = 0;
        bool numeric = false;
    };

    // --- helpers ---
    Rect to_canvas_(const Box& b) const {
        return Rect{{b.x, size_.y - b.bottom()}, {b.right(), size_.y - b.y}};
    }
    glm::vec2 flip_(glm::vec2 p) const { return {p.x, size_.y - p.y}; }
    void apply_clip_() { if (dl_) { dl_->pop_clip(); dl_->push_clip(to_canvas_(clip_stack_.back())); } }
    void push_clip_raw_(const Box& b) { clip_stack_.push_back(b); apply_clip_(); }
    void pop_clip_() { if (clip_stack_.size() > 1) clip_stack_.pop_back(); apply_clip_(); }

    bool any_pressed_() const { return in_.pressed[0] || in_.pressed[1] || in_.pressed[2]; }
    bool any_down_() const { return in_.down[0] || in_.down[1] || in_.down[2]; }
    bool layer_ok_() const { return layer_stack_.back().id == hovered_layer_; }
    bool hoverable_(const Box& b, Id self = 0) const {
        return layer_ok_() && b.contains(in_.mouse) && clip_stack_.back().contains(in_.mouse) &&
               (active_id_ == 0 || (self != 0 && active_id_ == self));
    }
    bool press_origin_hit_(Mouse b, const Box& r) const { return r.contains(press_pos_[static_cast<int>(b)]); }

    /** @brief Button behaviour: returns true on click (press + release inside). */
    bool behavior_(Id id, const Box& b, bool& hovered, bool& held) {
        hovered = layer_ok_() && b.contains(in_.mouse) && clip_stack_.back().contains(in_.mouse) &&
                  (active_id_ == 0 || active_id_ == id);
        // Hover fires on the frame the pointer ARRIVES, not every frame it stays: comparing
        // against the previous frame's hot id is what keeps a tick from becoming a drone.
        if (hovered) {
            if (hot_id_ != id && hot_prev_ != id && on_sound) on_sound(UiSound::Hover);
            hot_id_ = id;
        }
        bool clicked = false;
        // Declaration order IS tab order: every widget that can be clicked registers here, so
        // a new widget is reachable by keyboard without having to say so.
        if (keyboard_focus && id && layer_ok_()) focusables_.push_back(id);
        // Enter/Space on the focused widget is a click. Guarded on the text editor, which owns
        // Enter while it is open, and on keyboard_consumed_ so a shortcut wins.
        if (keyboard_focus && id && id == focus_id_ && !text_edit_.active && !keyboard_consumed_) {
            if (key_pressed(coopa::input::Key::Enter) || key_pressed(coopa::input::Key::Space)) {
                clicked = true;
                keyboard_consumed_ = true;
                if (on_sound) on_sound(UiSound::Press);
            }
        }
        if (hovered && in_.pressed[0]) {
            active_id_ = id;
            press_pos_[0] = in_.mouse;
            activated_id_ = id;
            if (keyboard_focus) focus_id_ = id;   // Tab continues from where you were
            if (on_sound) on_sound(UiSound::Press);
        }
        held = active_id_ == id && in_.down[0];
        if (active_id_ == id && in_.released[0]) {
            clicked = hovered && !drag_.active;
            active_id_ = 0;
            deactivated_id_ = id;
            // Only a completed click: a press dragged off its widget is a cancel, and should
            // sound like nothing.
            if (clicked && on_sound) on_sound(UiSound::Release);
        }
        return clicked;
    }

    /**
     * @brief Moves focus on Tab / Shift+Tab, using the ids this frame declared.
     *
     * Run at END of frame on purpose: the list is only complete once every widget has been
     * declared, and a UI whose shape changes between frames then gets a tab order that follows
     * it rather than one captured when it was first built.
     */
    void advance_focus_() {
        using coopa::input::Key;
        using coopa::input::Mods;
        if (!keyboard_focus) return;                           // the host has its own keymap
        if (text_edit_.active || keyboard_consumed_) return;   // a field owns Tab while open
        const bool fwd = key_pressed(Key::Tab, Mods::None, true);
        const bool back = key_pressed(Key::Tab, Mods::Shift, true);
        if (!fwd && !back) return;
        if (focusables_.empty()) {
            focus_id_ = 0;
            return;
        }
        keyboard_consumed_ = true;
        auto it = std::find(focusables_.begin(), focusables_.end(), focus_id_);
        if (it == focusables_.end()) {
            // Nothing focused (or the focused widget is gone this frame): start at an end.
            focus_id_ = fwd ? focusables_.front() : focusables_.back();
            return;
        }
        const size_t n = focusables_.size();
        const size_t i = static_cast<size_t>(it - focusables_.begin());
        focus_id_ = focusables_[fwd ? (i + 1) % n : (i + n - 1) % n];
    }

    /** @brief The shared body of both button() overloads. */
    bool button_in_(std::string_view lbl, const Box& b, bool enabled, Icon ic) {
        const Id id = get_id(lbl);
        const std::string_view shown = label_text(lbl);
        const float iw = ic != Icon::None ? style.row_height - 4 : 0.0f;
        bool hovered = false, held = false;
        const bool clicked = enabled && behavior_(id, b, hovered, held);
        fill_rounded(b, !enabled ? style.panel_alt : held ? style.button_active : hovered ? style.button_hover : style.button);
        const glm::vec4 tc = enabled ? style.text : style.text_disabled;
        if (ic != Icon::None) {
            const float tw = text_width(shown);
            const float total = iw + (tw > 0 ? tw + 4 : 0);
            const float x0 = b.x + (b.w - total) * 0.5f;
            icon(ic, {x0, b.y + 2, iw, b.h - 4}, tc);
            if (!shown.empty()) text_in({x0 + iw + 4, b.y, b.w, b.h}, shown, tc, 0.0f);
        } else {
            text_in(b, shown, tc, 0.0f, true);
        }
        set_last_(id, b, hovered);
        return clicked;
    }

    void set_last_(Id id, const Box& b, bool hovered) {
        last_item_.id = id;
        last_item_.rect = b;
        last_item_.hovered = hovered;
        last_item_.deactivated = id != 0 && deactivated_id_ == id;
        if (last_item_.deactivated) deactivated_id_ = 0;
        // Drawn here rather than in each widget: every widget calls this after painting
        // itself, so the ring lands on top and no widget has to opt in.
        if (keyboard_focus && id != 0 && id == focus_id_) {
            outline_rounded(b.shrink(-1.0f), style.accent, -1.0f, 1.5f);
        }
    }

    int find_popup_(Id id) const {
        for (size_t i = 0; i < open_popups_.size(); ++i) if (open_popups_[i].id == id) return static_cast<int>(i);
        return -1;
    }

    void open_popup_id_(Id id, glm::vec2 pos, bool modal, bool from_menubar) {
        if (!modal && std::find(suppress_reopen_.begin(), suppress_reopen_.end(), id) != suppress_reopen_.end()) return;
        const int existing = find_popup_(id);
        if (existing >= 0) { open_popups_[static_cast<size_t>(existing)].pos = pos; return; }
        // Opened from inside popup N: nests at depth N+1; opened from the base layer: replaces
        // every non-modal popup.
        const size_t depth = popup_draw_stack_.size();
        if (open_popups_.size() > depth) open_popups_.resize(depth);
        PopupState p;
        p.id = id;
        p.pos = pos;
        p.modal = modal;
        p.from_menubar = from_menubar;
        p.seen_frame = frame_;
        p.opened_frame = frame_;
        open_popups_.push_back(p);
    }

    bool begin_popup_id_(Id id, float min_w) {
        const int idx = find_popup_(id);
        if (idx < 0) return false;
        PopupState& p = open_popups_[static_cast<size_t>(idx)];
        p.seen_frame = frame_;
        const int z = kPopupZ + (idx + 1) * 10;
        glm::vec2 sz = glm::max(p.size, glm::vec2(min_w, style.row_height));
        glm::vec2 pos = p.pos;
        if (pos.x + sz.x > size_.x) pos.x = std::max(0.0f, size_.x - sz.x - 2);
        if (pos.y + sz.y > size_.y) pos.y = std::max(0.0f, size_.y - sz.y - 2);
        Box frame{pos.x, pos.y, sz.x, sz.y};
        dl_->set_z_order(z + 1);
        layer_stack_.push_back({id, z});
        popup_draw_stack_.push_back(id);
        clip_stack_.push_back(Box{0, 0, size_.x, size_.y}.intersect(frame));
        apply_clip_();
        Layout l{Box{frame.x + 4, frame.y + 4, std::max(min_w, p.size.x) - 8, 1e6f}};
        l.cursor = l.region.pos();
        l.content_top = l.cursor.y;
        l.content_bottom = l.cursor.y;
        l.id = id;
        layout_stack_.push_back(l);
        push_id(static_cast<int64_t>(id));
        return true;
    }

    void end_popup_() {
        if (popup_draw_stack_.empty()) return;
        pop_id();
        const Layout l = layout_stack_.back();
        layout_stack_.pop_back();
        const Id id = popup_draw_stack_.back();
        popup_draw_stack_.pop_back();
        const int idx = find_popup_(id);
        const int z = layer_stack_.back().z;
        layer_stack_.pop_back();
        pop_clip_();
        if (idx >= 0) {
            PopupState& p = open_popups_[static_cast<size_t>(idx)];
            const glm::vec2 measured{std::max(l.region.w + 8, p.size.x), (l.content_bottom - l.content_top) + 8};
            p.size = glm::vec2(std::max(l.region.w + 8, 0.0f), measured.y);
            glm::vec2 pos = p.pos;
            if (pos.x + p.size.x > size_.x) pos.x = std::max(0.0f, size_.x - p.size.x - 2);
            if (pos.y + p.size.y > size_.y) pos.y = std::max(0.0f, size_.y - p.size.y - 2);
            Box frame{pos.x, pos.y, p.size.x, p.size.y};
            dl_->set_z_order(z);
            push_clip_raw_(Box{0, 0, size_.x, size_.y});
            shadow(frame, style.rounding + 2);
            fill_rounded(frame, style.popup_bg, style.rounding + 1);
            outline_rounded(frame, style.edge_highlight, style.rounding + 1);
            pop_clip_();
            layers_.push_back({id, frame, z, false});
        }
        dl_->set_z_order(layer_stack_.back().z == 0 ? 0 : layer_stack_.back().z + 1);
    }

    /** @brief Blender's checkbox: rounded box, blue with a white tick when on. */
    void draw_check_(const Box& cb, bool on, bool hovered) {
        if (on) {
            fill_rounded(cb, style.accent, 3);
            icon(Icon::Check, cb.shrink(1), style.check_mark);
        } else {
            fill_rounded(cb, hovered ? style.button_hover : style.button, 3);
        }
    }

    /// Lists longer than this get a search field at the top of their popup.
    static constexpr size_t kComboSearchMin = 15;

    bool combo_in_(Id id, const Box& b, int* index, const std::vector<std::string>& items, int* hovered_item = nullptr) {
        using coopa::input::Key;
        using coopa::input::KeyAction;
        if (hovered_item) *hovered_item = -1;
        bool hovered = false, held = false;
        const bool clicked = behavior_(id, b, hovered, held);
        fill_rounded(b, hovered ? style.button_hover : style.button);
        const std::string cur = (*index >= 0 && *index < static_cast<int>(items.size())) ? items[*index] : std::string("-");
        text_in({b.x, b.y, b.w - b.h, b.h}, cur, style.text);
        arrow({b.right() - b.h + 3, b.y + 3, b.h - 6, b.h - 6}, true, style.text_dim);
        const Id pid = hash_int(id, 11);
        const Id sid = hash_int(id, 12);   // the search field
        const bool searchable = items.size() > kComboSearchMin;
        bool opened = false;
        if (clicked) {
            if (find_popup_(pid) >= 0) open_popups_.resize(static_cast<size_t>(find_popup_(pid)));
            else {
                open_popup_id_(pid, {b.x, b.bottom() + 1}, false, false);
                opened = true;
            }
        }
        set_last_(id, b, hovered);
        bool changed = false;
        if (opened && searchable) {
            // Type straight away: the field has the keyboard from the first frame.
            combo_search_ = {pid, {}, -1, false, true};
            begin_text_edit_(sid, std::string(), false);
        }
        if (begin_popup_id_(pid, b.w)) {
            const float pitch = style.row_height + style.spacing;
            // Which items show: all, or those whose name contains the query (any case).
            std::vector<int> shown;
            ComboSearch* cs = searchable && combo_search_.popup == pid ? &combo_search_ : nullptr;
            bool pick_highlight = false;
            if (cs) {
                const bool editing = text_edit_.active && text_edit_.id == sid;
                if (editing) {
                    for (const auto& e : in_.keys) {
                        if (e.action == KeyAction::Release) continue;
                        if (e.key == Key::Down) { ++cs->highlight; cs->keyboard = true; cs->scroll_to = true; }
                        if (e.key == Key::Up) { cs->highlight = std::max(0, cs->highlight - 1); cs->keyboard = true; cs->scroll_to = true; }
                        if (e.key == Key::Enter || e.key == Key::KpEnter) pick_highlight = true;
                        if (e.key == Key::Escape) open_popups_.resize(static_cast<size_t>(find_popup_(pid)));
                        keyboard_consumed_ = true;
                    }
                    if (text_edit_.buffer != cs->query) { cs->query = text_edit_.buffer; cs->highlight = cs->query.empty() ? -1 : 0; cs->keyboard = true; cs->scroll_to = true; }
                }
                if (in_.mouse_delta != glm::vec2(0.0f)) cs->keyboard = false;   // the mouse takes over the preview
                const Box sb = next_box(style.row_height);
                std::string q = cs->query;
                input_text_in_(sid, sb, &q, true);
                if (cs->query.empty()) text_in({sb.x + 2, sb.y, sb.w - 2, sb.h}, "Search", style.text_disabled);
                auto lower = [](std::string v) {
                    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    return v;
                };
                const std::string lq = lower(cs->query);
                for (int i = 0; i < static_cast<int>(items.size()); ++i)
                    if (lq.empty() || lower(items[static_cast<size_t>(i)]).find(lq) != std::string::npos) shown.push_back(i);
                cs->highlight = std::min(cs->highlight, static_cast<int>(shown.size()) - 1);
                if (opened) {
                    // Start with the current item in view.
                    for (size_t k = 0; k < shown.size(); ++k) if (shown[k] == *index) state_(get_id("combo_list")).f[0] = std::max(0.0f, (static_cast<float>(k) - 3.0f) * pitch);
                }
                if (cs->scroll_to && cs->highlight >= 0) {
                    float& sy = state_(get_id("combo_list")).f[0];
                    const float top = static_cast<float>(cs->highlight) * pitch;
                    const float view = static_cast<float>(std::min<size_t>(shown.size(), 18)) * pitch - style.padding;
                    if (top < sy) sy = top;
                    if (top + pitch > sy + view) sy = top + pitch - view;
                }
                cs->scroll_to = false;
                if (pick_highlight && !shown.empty()) {
                    const int i = shown[static_cast<size_t>(std::max(0, cs->highlight))];
                    if (*index != i) { *index = i; changed = true; }
                    close_all_popups();
                }
            } else {
                shown.resize(items.size());
                for (int i = 0; i < static_cast<int>(items.size()); ++i) shown[static_cast<size_t>(i)] = i;
            }
            const size_t visible = std::min<size_t>(shown.size(), 18);
            Box list = next_box(std::max<size_t>(visible, 1) * pitch);
            begin_region("combo_list", list, shown.size() > visible);
            if (shown.empty()) text_in(next_box(style.row_height), "No matches", style.text_disabled);
            for (size_t k = 0; k < shown.size() && !changed; ++k) {
                const int i = shown[k];
                push_id(static_cast<int64_t>(i));
                if (selectable(items[static_cast<size_t>(i)], i == *index)) {
                    if (*index != i) { *index = i; changed = true; }
                    close_all_popups();
                } else {
                    if (cs && static_cast<int>(k) == cs->highlight) outline_rounded(last_item_.rect, style.accent, 3);
                    if (hovered_item && last_hovered() && !(cs && cs->keyboard)) *hovered_item = i;
                    if (hovered_item && cs && cs->keyboard && static_cast<int>(k) == cs->highlight) *hovered_item = i;
                }
                pop_id();
            }
            end_region();
            end_popup_();
        }
        if (changed && hovered_item) *hovered_item = -1;
        return changed;
    }

    bool drag_float_in_(Id id, const Box& b, float* v, float speed, float lo, float hi, const char* fmt,
                        const glm::vec4* tick) {
        bool pending_changed = false;
        if (pending_commit_.id == id) {
            char* end = nullptr;
            const float parsed = std::strtof(pending_commit_.value.c_str(), &end);
            if (end != pending_commit_.value.c_str()) {
                const float nv = std::clamp(parsed, lo, hi);
                pending_changed = nv != *v;
                *v = nv;
            }
            pending_commit_ = {};
        }
        // Typed entry mode.
        if (text_edit_.active && text_edit_.id == id) {
            std::string s;
            char buf[64];
            std::snprintf(buf, sizeof(buf), fmt, *v);
            s = buf;
            const float before = *v;
            if (input_text_in_(id, b, &s, true)) {
                char* end = nullptr;
                const float parsed = std::strtof(s.c_str(), &end);
                if (end != s.c_str()) *v = std::clamp(parsed, lo, hi);
            }
            set_last_(id, b, b.contains(in_.mouse));
            if (!text_edit_.active || text_edit_.id != id) last_item_.deactivated = true;
            return *v != before;
        }
        bool hovered = layer_ok_() && b.contains(in_.mouse) && clip_stack_.back().contains(in_.mouse) &&
                       (active_id_ == 0 || active_id_ == id);
        if (hovered) hot_id_ = id;
        bool changed = false;
        if (hovered && in_.pressed[0]) {
            active_id_ = id;
            press_pos_[0] = in_.mouse;
            drag_moved_ = false;
            drag_accum_ = 0.0f;
        }
        const bool held = active_id_ == id && in_.down[0];
        if (held && in_.mouse_delta.x != 0.0f) {
            drag_accum_ += std::abs(in_.mouse_delta.x);
            if (drag_accum_ > 2.0f) drag_moved_ = true;
            if (drag_moved_) {
                float step = speed;
                if (has(in_.mods, coopa::input::Mods::Shift)) step *= 10.0f;
                if (has(in_.mods, coopa::input::Mods::Alt)) step *= 0.1f;
                const float nv = std::clamp(*v + in_.mouse_delta.x * step, lo, hi);
                if (nv != *v) { *v = nv; changed = true; }
            }
        }
        bool deactivated = false;
        if (active_id_ == id && in_.released[0]) {
            active_id_ = 0;
            deactivated = drag_moved_;
            if (!drag_moved_ && hovered) {
                if (b.w > 60 && in_.mouse.x < b.x + 14) {          // left arrow: step down
                    *v = std::clamp(*v - speed * 10.0f, lo, hi); changed = true; deactivated = true;
                } else if (b.w > 60 && in_.mouse.x > b.right() - 14) {   // right arrow: step up
                    *v = std::clamp(*v + speed * 10.0f, lo, hi); changed = true; deactivated = true;
                } else {
                    // A click without a drag: type a value.
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), fmt, *v);
                    begin_text_edit_(id, buf, true);
                }
            }
        }
        const int corners = stacked_corners_ >= 0 ? stacked_corners_ : kAll;
        fill_rounded(b, held || hovered ? style.number_hover : style.number, -1, corners);
        if (tick) fill({b.x + 1, b.y + 3, 2, b.h - 6}, *tick);
        char buf[64];
        std::snprintf(buf, sizeof(buf), fmt, *v);
        if (stacked_label_) {
            // Stacked vector rows: "X" at the left, value right-of-centre (Blender's Location block).
            text_in({b.x + 8, b.y, 20, b.h}, stacked_label_, style.text_dim, 0.0f);
            text_in(b, buf, style.text, 0, true);
        } else {
            text_in(b, buf, style.text, 0, true);
        }
        // Blender's step arrows at both ends while hovered: click to nudge.
        if (hovered && !held && b.w > 60) {
            const glm::vec4 ac = with_alpha(style.text, 0.6f);
            const float m = b.y + b.h * 0.5f;
            triangle({b.x + 9, m - 4}, {b.x + 9, m + 4}, {b.x + 5, m}, ac);
            triangle({b.right() - 9, m - 4}, {b.right() - 9, m + 4}, {b.right() - 5, m}, ac);
        }
        set_last_(id, b, hovered);
        last_item_.deactivated = deactivated || pending_changed;
        return changed || pending_changed;
    }

    void begin_text_edit_(Id id, const std::string& value, bool numeric) {
        // Focus jumping straight from one field to another: the old field may already have
        // been declared this frame, so park its buffer for it to commit when next declared.
        if (text_edit_.active && text_edit_.id != id && text_edit_.buffer != text_edit_.original) {
            pending_commit_ = {text_edit_.id, text_edit_.buffer};
        }
        text_edit_ = TextEdit{};
        text_edit_.active = true;
        text_edit_.id = id;
        text_edit_.buffer = value;
        text_edit_.original = value;
        text_edit_.caret = value.size();
        text_edit_.select_all = true;
        text_edit_.seen_frame = frame_;
        text_edit_.numeric = numeric;
        active_id_ = 0;
    }

    bool input_text_in_(Id id, const Box& b, std::string* value, bool /*boxed*/) {
        using coopa::input::Key;
        using coopa::input::KeyAction;
        using coopa::input::Mods;
        const bool editing = text_edit_.active && text_edit_.id == id;
        const bool hovered = layer_ok_() && b.contains(in_.mouse) && clip_stack_.back().contains(in_.mouse);
        if (hovered) { hot_id_ = id; set_mouse_cursor(coopa::input::CursorShape::IBeam); }
        bool committed = false;

        if (!editing) {
            if (pending_commit_.id == id) {
                // (value applied below)
                committed = *value != pending_commit_.value;
                *value = pending_commit_.value;
                pending_commit_ = {};
            }
            if (hovered && in_.pressed[0]) begin_text_edit_(id, *value, false);
            fill_rounded(b, hovered ? style.field_hover : style.field);
            text_in(b, *value, style.text);
            set_last_(id, b, hovered);
            if (committed) last_item_.deactivated = true;
            return committed;
        }

        TextEdit& te = text_edit_;
        te.seen_frame = frame_;
        bool finish = false, cancel = false;
        // Click outside commits.
        if (any_pressed_() && !hovered) finish = true;
        else if (hovered && in_.pressed[0]) {
            te.select_all = false;
            te.caret = text.hit_index(te.buffer, style.font_size, in_.mouse.x - (b.x + style.padding) + te.scroll_x);
        }
        auto erase_selection = [&] {
            if (te.select_all) { te.buffer.clear(); te.caret = 0; te.select_all = false; return true; }
            return false;
        };
        const Mods cmd = command_mod();
        for (const auto& e : in_.keys) {
            if (e.action == KeyAction::Release) continue;
            const bool command = has(e.mods, cmd) || has(e.mods, Mods::Control);
            switch (e.key) {
                case Key::Enter: case Key::KpEnter: case Key::Tab: finish = true; break;
                case Key::Escape: cancel = true; break;
                case Key::Backspace:
                    if (!erase_selection() && te.caret > 0) { te.buffer.erase(te.caret - 1, 1); --te.caret; }
                    break;
                case Key::Delete:
                    if (!erase_selection() && te.caret < te.buffer.size()) te.buffer.erase(te.caret, 1);
                    break;
                case Key::Left:  te.select_all = false; if (te.caret > 0) --te.caret; break;
                case Key::Right: te.select_all = false; if (te.caret < te.buffer.size()) ++te.caret; break;
                case Key::Home:  te.select_all = false; te.caret = 0; break;
                case Key::End:   te.select_all = false; te.caret = te.buffer.size(); break;
                case Key::A: if (command) te.select_all = true; break;
                case Key::C: if (command && in_.set_clipboard) in_.set_clipboard(te.buffer); break;
                case Key::X:
                    if (command && in_.set_clipboard) { in_.set_clipboard(te.buffer); te.buffer.clear(); te.caret = 0; te.select_all = false; }
                    break;
                case Key::V:
                    if (command && in_.get_clipboard) {
                        erase_selection();
                        std::string clip = in_.get_clipboard();
                        clip.erase(std::remove_if(clip.begin(), clip.end(), [](char c) { return c == '\n' || c == '\r'; }), clip.end());
                        te.buffer.insert(te.caret, clip);
                        te.caret += clip.size();
                    }
                    break;
                default: break;
            }
        }
        const bool command_held = has(in_.mods, cmd) || has(in_.mods, Mods::Control);
        if (!command_held) {
            for (uint32_t c : in_.chars) {
                if (c < 32 || c > 126) continue;
                erase_selection();
                te.buffer.insert(te.buffer.begin() + static_cast<long>(te.caret), static_cast<char>(c));
                ++te.caret;
            }
        }
        keyboard_consumed_ = true;

        // Draw.
        fill_rounded(b, style.field);
        outline_rounded(b, style.accent);
        const float pad = style.padding;
        const float caret_x = text.width(std::string_view(te.buffer).substr(0, te.caret), style.font_size);
        const float inner_w = b.w - pad * 2;
        if (caret_x - te.scroll_x > inner_w) te.scroll_x = caret_x - inner_w;
        if (caret_x - te.scroll_x < 0) te.scroll_x = caret_x;
        push_clip(b);
        const float lh = text.line_height(style.font_size);
        if (te.select_all && !te.buffer.empty()) {
            fill({b.x + pad - te.scroll_x, b.y + (b.h - lh) * 0.5f, text.width(te.buffer, style.font_size), lh}, style.selection);
        }
        draw_text({b.x + pad - te.scroll_x, b.y + (b.h - lh) * 0.5f}, te.buffer, style.text);
        if (std::fmod(time_, 1.0) < 0.6) fill({b.x + pad + caret_x - te.scroll_x, b.y + 4, 1, b.h - 8}, style.text);
        pop_clip();

        if (cancel) {
            text_edit_ = {};
        } else if (finish) {
            if (te.buffer != *value) { *value = te.buffer; committed = true; }
            text_edit_ = {};
        }
        set_last_(id, b, hovered);
        if (committed || cancel) last_item_.deactivated = true;
        return committed;
    }

    bool color_picker_(float* rgba, bool& any_active, bool& any_deact) {
        bool changed = false;
        glm::vec3 rgb = glm::clamp(glm::vec3(rgba[0], rgba[1], rgba[2]), 0.0f, 1.0f);
        State& st = state_(get_id("hsv"));
        // Keep hue/saturation stable while the colour is grey/black (they are undefined there).
        glm::vec3 hsv = rgb_to_hsv(rgb);
        if (st.init && (hsv.y < 1e-4f || hsv.z < 1e-4f)) { hsv.x = st.f[0]; if (hsv.z < 1e-4f) hsv.y = st.f[1]; }
        st.init = true;
        Box sv = next_box(150, 150);
        same_line();
        Box hue = next_box(150, 18);
        const glm::vec3 pure = hsv_to_rgb(hsv.x, 1, 1);
        gradient(sv, glm::vec4(1), glm::vec4(pure, 1), glm::vec4(pure, 1), glm::vec4(1));
        gradient(sv, glm::vec4(0, 0, 0, 0), glm::vec4(0, 0, 0, 0), glm::vec4(0, 0, 0, 1), glm::vec4(0, 0, 0, 1));
        for (int i = 0; i < 6; ++i) {
            const float y0 = hue.y + hue.h * i / 6.0f, y1 = hue.y + hue.h * (i + 1) / 6.0f;
            gradient({hue.x, y0, hue.w, y1 - y0}, glm::vec4(hsv_to_rgb(i / 6.0f, 1, 1), 1), glm::vec4(hsv_to_rgb(i / 6.0f, 1, 1), 1),
                     glm::vec4(hsv_to_rgb((i + 1) / 6.0f, 1, 1), 1), glm::vec4(hsv_to_rgb((i + 1) / 6.0f, 1, 1), 1));
        }
        bool h1 = false, held1 = false, h2 = false, held2 = false;
        behavior_(get_id("sv"), sv, h1, held1);
        behavior_(get_id("hue"), hue, h2, held2);
        if (held1) {
            hsv.y = std::clamp((in_.mouse.x - sv.x) / sv.w, 0.0f, 1.0f);
            hsv.z = 1.0f - std::clamp((in_.mouse.y - sv.y) / sv.h, 0.0f, 1.0f);
            changed = true;
        }
        if (held2) { hsv.x = std::clamp((in_.mouse.y - hue.y) / hue.h, 0.0f, 0.9999f); changed = true; }
        any_active |= held1 || held2;
        if (deactivated_id_ == get_id("sv") || deactivated_id_ == get_id("hue")) { any_deact = true; deactivated_id_ = 0; }
        st.f[0] = hsv.x;
        st.f[1] = hsv.y;
        const glm::vec2 m{sv.x + hsv.y * sv.w, sv.y + (1 - hsv.z) * sv.h};
        outline({m.x - 4, m.y - 4, 8, 8}, glm::vec4(1));
        fill({hue.x - 2, hue.y + hsv.x * hue.h - 1, hue.w + 4, 2}, glm::vec4(1));
        if (changed) {
            const glm::vec3 c = hsv_to_rgb(hsv.x, hsv.y, hsv.z);
            rgba[0] = c.r; rgba[1] = c.g; rgba[2] = c.b;
        }
        // Hex entry.
        char hex[16];
        const glm::vec3 cur = glm::clamp(glm::vec3(rgba[0], rgba[1], rgba[2]), 0.0f, 1.0f);
        std::snprintf(hex, sizeof(hex), "%02X%02X%02X", int(cur.r * 255 + 0.5f), int(cur.g * 255 + 0.5f), int(cur.b * 255 + 0.5f));
        std::string hs = hex;
        if (input_text("Hex", &hs)) {
            unsigned int v = 0;
            if (std::sscanf(hs.c_str(), "%x", &v) == 1) {
                rgba[0] = ((v >> 16) & 0xFF) / 255.0f;
                rgba[1] = ((v >> 8) & 0xFF) / 255.0f;
                rgba[2] = (v & 0xFF) / 255.0f;
                changed = true;
                any_deact = true;
            }
        }
        return changed;
    }

    // --- state ---
    DrawList* dl_ = nullptr;
    FrameInput in_;
    glm::vec2 size_{0.0f};
    double time_ = 0.0;
    uint64_t frame_ = 0;

    std::vector<Id> id_stack_;
    std::vector<Box> clip_stack_;
    std::vector<Layout> layout_stack_;
    std::vector<LayerFrame> layer_stack_;
    std::vector<LayerRect> layers_, layers_prev_;
    int modal_z_prev_ = -1;
    Id hovered_layer_ = 0;
    bool popup_hovered_ = false;

    std::vector<PopupState> open_popups_;
    std::vector<Id> popup_draw_stack_;
    std::vector<Id> closed_by_click_;
    coopa::input::CursorShape cursor_ = coopa::input::CursorShape::Arrow;   ///< See cursor().
    std::vector<Id> suppress_reopen_;   ///< popups closed by the current left click (see begin_frame)
    MenuBarState menubar_;

    Id hot_id_ = 0;
    /// Last frame's hot id, so Hover fires once on arrival. See behavior_().
    Id hot_prev_ = 0;
    Id active_id_ = 0;
    Id activated_id_ = 0;
    Id deactivated_id_ = 0;
    glm::vec2 press_pos_[3] = {};
    bool drag_moved_ = false;
    float drag_accum_ = 0.0f;
    float scroll_grab_offset_ = 0.0f;
    bool group_active_ = false;
    Box last_label_box_;
    LastItem last_item_;
    bool keyboard_consumed_ = false;
    /// The focused widget, or 0. Survives the frame; everything else about focus does not.
    Id focus_id_ = 0;
    /// Every focusable id declared this frame, in order. Rebuilt per frame, which is what lets
    /// Tab follow a UI whose shape changes between frames.
    std::vector<Id> focusables_;
    bool wheel_consumed_ = false;

    Id last_click_id_ = 0;
    double last_click_time_ = 0.0;

    std::string tooltip_;
    Id tooltip_target_ = 0;
    double tooltip_start_ = 0.0;
    bool tooltip_seen_ = false;          ///< tooltip() saw its item hovered this frame
    glm::vec2 tooltip_prev_mouse_{0.0f};  ///< last frame's mouse, to hold the delay while moving

    DragPayload drag_;
    Id drag_candidate_ = 0;

    TextEdit text_edit_;
    struct PendingCommit { Id id = 0; std::string value; } pending_commit_;
    /// The open searchable combo's query and keyboard highlight (one combo popup is open at a time).
    struct ComboSearch {
        Id popup = 0;
        std::string query;
        int highlight = -1;      ///< Index into the filtered list; -1: none
        bool keyboard = false;   ///< The last navigation was the keyboard (its highlight is "hovered")
        bool scroll_to = false;  ///< Bring the highlight into view this frame
    } combo_search_;
    int stacked_corners_ = -1;
    const char* stacked_label_ = nullptr;
    std::unordered_map<Id, State> states_;
};

}  // namespace imm
}  // namespace ui
}  // namespace coopa

#include <uicoopa/immediate/imm_icons.h>
#include <uicoopa/immediate/imm_game.h>

#endif  // UICOOPA_IMMEDIATE_IMM_H
