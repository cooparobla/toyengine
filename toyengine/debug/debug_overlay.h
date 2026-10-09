/**
 * @file debug_overlay.h
 * @brief The on-screen stats HUD: frame times always, and in full mode the renderer, scene,
 *        physics, navigation and GPU-memory numbers, plus lines the game publishes itself.
 *
 * Three modes, cycled by F3 (the `debug_overlay` input action, Engine::bind_default_input_())
 * and chosen at startup by config.yaml's `debug.overlay: off|fps|full`:
 *   - off  -- nothing: no overlay scene is ticked or drawn, no profiler runs, and the game
 *             API below returns at once. The frame is byte-identical to one without it.
 *   - fps  -- FPS, frame ms (avg / min / max over the last 240 frames), the 1% low, and a
 *             sparkline of the same ring.
 *   - full -- adds CPU phase and GPU pass timings (a live render::FrameProfile; GPU numbers
 *             land two frames late, see gpu_profiler.h), draw stats, scene counts, physics,
 *             navigation and VMA heap budgets.
 *
 * Game code publishes its own per-frame lines, shown in a "Game" block:
 * @code
 * toy::debug::watch("speed", body.velocity().x);
 * toy::debug::text("state: %s", state_name);
 * @endcode
 * They are cleared at the end of every frame, so a line shows exactly while it is re-published.
 *
 * The overlay draws through an immediate-mode canvas in its own Scene, which the Engine owns
 * as an overlay layer (Engine::add_overlay_layer()) placed over the scene's display rect, so in
 * the editor it sits inside the viewport. The Engine fills OverlayStats; this file knows
 * nothing about the Engine, which keeps it testable without a GPU.
 *
 * A TOY_SHIPPING build compiles the whole thing out (k_overlay_compiled false: the mode stays
 * off and the API is empty) unless it is built with TOY_DEBUG_OVERLAY (cmake
 * -DTOY_SHIPPING_DEBUG_OVERLAY=ON).
 */

#ifndef TOYENGINE_DEBUG_DEBUG_OVERLAY_H
#define TOYENGINE_DEBUG_DEBUG_OVERLAY_H

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <coopa/scene/scene.h>
#include <uicoopa/immediate/imm.h>
#include <uicoopa/immediate/imm_canvas.h>

#include <toyengine/render/frame_profile.h>

namespace toy {
namespace debug {

#if defined(TOY_SHIPPING) && !defined(TOY_DEBUG_OVERLAY)
inline constexpr bool k_overlay_compiled = false;
#else
/// False in a TOY_SHIPPING build without TOY_DEBUG_OVERLAY: the overlay never shows and
/// watch()/text() compile to nothing.
inline constexpr bool k_overlay_compiled = true;
#endif

/// What the overlay shows. F3 cycles Off -> Fps -> Full -> Off.
enum class OverlayMode { Off, Fps, Full };

/** @brief `off` / `fps` / `full`; anything else is nullopt (the caller warns). */
inline std::optional<OverlayMode> parse_overlay_mode(std::string_view s) {
    if (s == "off" || s == "false" || s.empty()) return OverlayMode::Off;
    if (s == "fps") return OverlayMode::Fps;
    if (s == "full") return OverlayMode::Full;
    return std::nullopt;
}

inline const char* overlay_mode_name(OverlayMode m) {
    switch (m) {
        case OverlayMode::Fps:  return "fps";
        case OverlayMode::Full: return "full";
        default:                return "off";
    }
}

inline OverlayMode next_overlay_mode(OverlayMode m) {
    switch (m) {
        case OverlayMode::Off: return OverlayMode::Fps;
        case OverlayMode::Fps: return OverlayMode::Full;
        default:               return OverlayMode::Off;
    }
}

// =====================================================================================
// Frame-time ring
// =====================================================================================

/**
 * @class FrameTimeRing
 * @brief The last kCapacity frame times (ms) and their statistics.
 */
class FrameTimeRing {
public:
    static constexpr size_t kCapacity = 240;

    void push(float ms) {
        values_[head_] = ms;
        head_ = (head_ + 1) % kCapacity;
        count_ = std::min(count_ + 1, kCapacity);
    }
    void clear() { head_ = count_ = 0; }

    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    /** @brief The i-th sample, oldest first. */
    float at(size_t i) const { return values_[(head_ + kCapacity - count_ + i) % kCapacity]; }

    float average() const {
        if (empty()) return 0.0f;
        double sum = 0.0;
        for (size_t i = 0; i < count_; ++i) sum += at(i);
        return static_cast<float>(sum / static_cast<double>(count_));
    }
    float min() const {
        float m = empty() ? 0.0f : at(0);
        for (size_t i = 1; i < count_; ++i) m = std::min(m, at(i));
        return m;
    }
    float max() const {
        float m = empty() ? 0.0f : at(0);
        for (size_t i = 1; i < count_; ++i) m = std::max(m, at(i));
        return m;
    }

    /**
     * @brief The "1% low": the frame rate of the slowest 1% of frames (at least one) -- the
     *        mean of the longest frame times, as FPS. 0 when empty.
     */
    float low_1pct_fps() const {
        if (empty()) return 0.0f;
        std::array<float, kCapacity> sorted{};
        for (size_t i = 0; i < count_; ++i) sorted[i] = at(i);
        const size_t n = std::max<size_t>(1, count_ / 100);
        std::partial_sort(sorted.begin(), sorted.begin() + n, sorted.begin() + count_, std::greater<float>());
        double sum = 0.0;
        for (size_t i = 0; i < n; ++i) sum += sorted[i];
        const double ms = sum / static_cast<double>(n);
        return ms > 0.0 ? static_cast<float>(1000.0 / ms) : 0.0f;
    }

private:
    std::array<float, kCapacity> values_{};
    size_t head_ = 0;
    size_t count_ = 0;
};

// =====================================================================================
// Game-facing lines
// =====================================================================================

/** @brief One toy::debug::watch() value. */
struct WatchLine {
    std::string name;
    std::string value;
};

/**
 * @class GameLines
 * @brief This frame's watch()/text() lines. One process-wide instance (the free functions
 *        below), cleared by the Engine at the end of every frame. Main thread only.
 *
 * Accepts nothing while the overlay is off, so publishing costs a branch then.
 */
class GameLines {
public:
    static GameLines& instance() {
        static GameLines lines;
        return lines;
    }

    bool accepting() const { return accepting_; }
    void set_accepting(bool on) {
        accepting_ = on;
        if (!on) clear();
    }

    /** @brief Sets `name`'s value for this frame; a repeat replaces it in place. */
    void watch(std::string_view name, std::string value) {
        if (!accepting_) return;
        for (WatchLine& w : watches_) {
            if (w.name == name) { w.value = std::move(value); return; }
        }
        watches_.push_back(WatchLine{std::string(name), std::move(value)});
    }
    void text(std::string line) {
        if (accepting_) texts_.push_back(std::move(line));
    }

    const std::vector<WatchLine>& watches() const { return watches_; }
    const std::vector<std::string>& texts() const { return texts_; }
    bool empty() const { return watches_.empty() && texts_.empty(); }
    void clear() {
        watches_.clear();
        texts_.clear();
    }

private:
    bool accepting_ = false;
    std::vector<WatchLine> watches_;
    std::vector<std::string> texts_;
};

/** @brief Formats a watch value the way the overlay prints it. */
inline std::string format_watch(bool v) { return v ? "true" : "false"; }
inline std::string format_watch(std::string_view v) { return std::string(v); }
inline std::string format_watch(const char* v) { return v ? std::string(v) : std::string(); }
inline std::string format_watch(const std::string& v) { return v; }
inline std::string format_watch(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return buf;
}
inline std::string format_watch(float v) { return format_watch(static_cast<double>(v)); }
template <typename T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
inline std::string format_watch(T v) { return std::to_string(v); }
inline std::string format_watch(const glm::vec2& v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "(%.3f, %.3f)", v.x, v.y);
    return buf;
}
inline std::string format_watch(const glm::vec3& v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "(%.3f, %.3f, %.3f)", v.x, v.y, v.z);
    return buf;
}
inline std::string format_watch(const glm::vec4& v) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "(%.3f, %.3f, %.3f, %.3f)", v.x, v.y, v.z, v.w);
    return buf;
}

/**
 * @brief Shows `name: value` in the overlay's Game block this frame. Values: bool, integers,
 *        float/double, strings, glm::vec2/3/4. Free while the overlay is off.
 */
template <typename T>
inline void watch(std::string_view name, const T& value) {
    if constexpr (k_overlay_compiled) {
        GameLines& lines = GameLines::instance();
        if (lines.accepting()) lines.watch(name, format_watch(value));
    } else {
        (void)name;
        (void)value;
    }
}

/** @brief A free-form printf-style line in the Game block this frame. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline void text(const char* fmt, ...) {
    if constexpr (k_overlay_compiled) {
        GameLines& lines = GameLines::instance();
        if (!lines.accepting() || !fmt) return;
        char buf[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        lines.text(buf);
    } else {
        (void)fmt;
    }
}

// =====================================================================================
// The overlay
// =====================================================================================

/**
 * @struct OverlayStats
 * @brief Everything full mode shows beyond frame times. Filled by the Engine (only in full
 *        mode, and only when DebugOverlay::refresh_due() says so).
 */
struct OverlayStats {
    std::optional<render::FrameProfile::Row> profile;   ///< The last completed profile row.

    uint32_t render_width = 0, render_height = 0;
    uint64_t draws = 0, instances = 0, triangles = 0;
    uint64_t shadow_draws = 0, shadow_triangles = 0;
    uint64_t renderers = 0, renderers_visible = 0;

    size_t objects = 0, components = 0, particles = 0, pending_assets = 0;

    bool   has_physics = false;
    size_t bodies = 0, bodies_awake = 0, contacts = 0;
    double physics_ms = 0.0;

    bool     has_nav = false;
    size_t   nav_agents = 0;
    double   nav_ms = 0.0;
    uint32_t nav_paths = 0;

    /// One VMA heap (Allocator::heap_budgets()).
    struct Heap {
        uint64_t usage = 0, budget = 0;
        bool device_local = false;
    };
    std::vector<Heap> heaps;
};

/**
 * @class DebugOverlay
 * @brief The HUD's state and drawing. The Engine owns one, feeds it frame times and stats,
 *        and ticks its scene() as an overlay layer while visible().
 */
class DebugOverlay {
public:
    /// Text refreshes this often (frames): numbers that change every frame cannot be read.
    /// The sparkline still moves every frame.
    static constexpr uint32_t kRefreshFrames = 15;

    /// One line of the panel: a label, an optional right-hand value, or a block heading.
    struct Line {
        std::string label;
        std::string value;
        bool heading = false;
    };

    explicit DebugOverlay(OverlayMode mode = OverlayMode::Off) { set_mode(mode); }
    DebugOverlay(const DebugOverlay&) = delete;
    DebugOverlay& operator=(const DebugOverlay&) = delete;

    OverlayMode mode() const { return mode_; }
    bool visible() const { return mode_ != OverlayMode::Off; }
    bool full() const { return mode_ == OverlayMode::Full; }

    void set_mode(OverlayMode mode) {
        if constexpr (!k_overlay_compiled) mode = OverlayMode::Off;
        if (mode == mode_ && accepting_synced_) return;
        const bool was_visible = visible();
        mode_ = mode;
        accepting_synced_ = true;
        GameLines::instance().set_accepting(visible());
        if (visible() && !was_visible) {
            // A fresh start: frame times from before it was shown say nothing about now.
            ring_.clear();
            frame_ = 0;
            skip_next_ = true;
        }
        refresh_now_ = true;
    }
    void cycle() { set_mode(next_overlay_mode(mode_)); }

    /**
     * @brief One frame's wall-clock time. Call once per frame while visible(). The first frame
     *        after the overlay is shown is dropped: it is usually the hitch that showed it
     *        (startup, a mode switch's GPU wait), not a frame of the game.
     */
    void record_frame(float ms) {
        if (skip_next_) skip_next_ = false;
        else ring_.push(ms);
        ++frame_;
    }
    const FrameTimeRing& ring() const { return ring_; }

    /** @brief True on frames the text should be rebuilt (and full mode's stats gathered). */
    bool refresh_due() const { return refresh_now_ || frame_ % kRefreshFrames == 0; }
    /** @brief Full mode's numbers; the Engine fills them when refresh_due(). */
    OverlayStats& stats() { return stats_; }

    /**
     * @brief The overlay's own scene: one screen-space immediate canvas, built on first use.
     * @param font The panel font (null draws no text, e.g. headless tests without a device).
     */
    coopa::scene::Scene& scene(coopa::ui::Font* font = nullptr) {
        if (!scene_) {
            scene_ = std::make_unique<coopa::scene::Scene>("DebugOverlay");
            canvas_ = coopa::ui::build_immediate_canvas(*scene_, "DebugOverlay", 0);
            canvas_->on_draw = [this](coopa::ui::imm::Context& ctx) { draw(ctx); };
        }
        if (font) canvas_->context().text.set_font(font);
        return *scene_;
    }
    bool has_scene() const { return scene_ != nullptr; }
    /** @brief Destroys the scene (before the fonts and the device it draws with go away). */
    void release_scene() {
        canvas_ = nullptr;
        scene_.reset();
    }

    /**
     * @brief The panel's text for the current mode: the numbers (from the ring and stats())
     *        followed by this frame's GameLines. draw() rebuilds the numbers only when
     *        refresh_due() but the game lines every frame; public for tests.
     */
    std::vector<Line> compose_lines() const {
        std::vector<Line> out = compose_numbers_();
        append_game_lines_(out);
        return out;
    }

    /** @brief The canvas's per-frame draw (also callable directly with any Context). */
    void draw(coopa::ui::imm::Context& ctx) {
        if (!visible()) return;
        if (refresh_due() || numbers_.empty()) {
            numbers_ = compose_numbers_();
            refresh_now_ = false;
        }
        // Game lines are per-frame by contract, so they never wait for a refresh.
        lines_ = numbers_;
        append_game_lines_(lines_);

        using coopa::ui::imm::Box;
        const float fs = 12.0f;
        ctx.style.font_size = fs;
        const float lh = std::max(fs * 1.25f, ctx.text.line_height(fs));
        const float pad = 6.0f;
        const float margin = 8.0f;
        const float spark_h = 36.0f;
        const float spark_w = static_cast<float>(FrameTimeRing::kCapacity);
        // Values line up in one column, just clear of the longest label that has one.
        float value_x = 96.0f;
        for (const Line& l : lines_) {
            if (!l.value.empty()) value_x = std::max(value_x, ctx.text.width(l.label, fs) + 14.0f);
        }
        float col_w = spark_w;
        for (const Line& l : lines_) {
            const float lw = l.value.empty() ? ctx.text.width(l.label, fs) : value_x + ctx.text.width(l.value, fs);
            col_w = std::max(col_w, lw);
        }

        // Layout: the frame-time lines, the sparkline under them, then the rest flowing down
        // and into further columns when the canvas is too short (full mode on a small window).
        const size_t kFpsLines = std::min<size_t>(3, lines_.size());
        const float top = margin + pad;
        const float bottom_limit = std::max(top + spark_h + lh * 4.0f, ctx.canvas_size().y - margin - pad);
        std::vector<glm::vec2> pos(lines_.size());
        float x = margin + pad;
        float y = top;
        float max_y = top;
        Box spark{};
        for (size_t i = 0; i < lines_.size(); ++i) {
            if (i == kFpsLines) {
                spark = Box{x, y + pad * 0.5f, spark_w, spark_h};
                y = spark.bottom() + pad;
            }
            // A heading never ends a column (it would be cut off from its block).
            const float need = lines_[i].heading ? lh * 2.0f : lh;
            if (y + need > bottom_limit && y > top) {
                x += col_w + pad * 3.0f;
                y = top;
            }
            pos[i] = glm::vec2(x, y);
            y += lh;
            max_y = std::max(max_y, y);
        }
        if (lines_.size() <= kFpsLines) {
            spark = Box{margin + pad, y + pad * 0.5f, spark_w, spark_h};
            max_y = spark.bottom();
        }
        max_y = std::max(max_y, spark.bottom());
        const Box panel{margin, margin, x + col_w + pad - margin, max_y + pad - margin};
        ctx.fill(panel, glm::vec4(0.0f, 0.0f, 0.0f, 0.62f));

        const glm::vec4 text_col(0.92f, 0.92f, 0.92f, 1.0f);
        const glm::vec4 dim_col(0.70f, 0.72f, 0.76f, 1.0f);
        const glm::vec4 head_col(0.55f, 0.80f, 1.0f, 1.0f);
        for (size_t i = 0; i < lines_.size(); ++i) {
            const Line& l = lines_[i];
            if (l.heading) {
                ctx.draw_text(pos[i], l.label, head_col, fs);
            } else if (l.value.empty()) {
                ctx.draw_text(pos[i], l.label, text_col, fs);
            } else {
                ctx.draw_text(pos[i], l.label, dim_col, fs);
                ctx.draw_text({pos[i].x + value_x, pos[i].y}, l.value, text_col, fs);
            }
        }

        // Sparkline: one bar per sample, newest on the right, scaled so 33.3 ms (30 FPS) is
        // full height; green under 16.7 ms, amber under 33.3, red above. The faint line is
        // the 60 FPS budget.
        ctx.fill(spark, glm::vec4(1.0f, 1.0f, 1.0f, 0.06f));
        const float scale_ms = 33.333f;
        const size_t n = ring_.size();
        const float x0 = spark.right() - static_cast<float>(n);
        for (size_t i = 0; i < n; ++i) {
            const float ms = ring_.at(i);
            const float h = std::clamp(ms / scale_ms, 0.02f, 1.0f) * spark.h;
            const glm::vec4 c = ms <= 16.7f ? glm::vec4(0.35f, 0.85f, 0.45f, 0.9f)
                              : ms <= 33.4f ? glm::vec4(0.95f, 0.75f, 0.25f, 0.9f)
                                            : glm::vec4(0.95f, 0.35f, 0.30f, 0.9f);
            ctx.fill(Box{x0 + static_cast<float>(i), spark.bottom() - h, 1.0f, h}, c);
        }
        const float budget_y = spark.bottom() - (16.667f / scale_ms) * spark.h;
        ctx.fill(Box{spark.x, budget_y, spark.w, 1.0f}, glm::vec4(1.0f, 1.0f, 1.0f, 0.25f));
    }

private:
    std::vector<Line> compose_numbers_() const {
        std::vector<Line> out;
        if (!visible()) return out;
        char buf[128];
        const float avg = ring_.average();
        std::snprintf(buf, sizeof(buf), "%.0f", avg > 0.0f ? 1000.0f / avg : 0.0f);
        out.push_back({"FPS", buf});
        std::snprintf(buf, sizeof(buf), "%.2f / %.2f / %.2f", avg, ring_.min(), ring_.max());
        out.push_back({"ms avg/min/max", buf});
        std::snprintf(buf, sizeof(buf), "%.0f", ring_.low_1pct_fps());
        out.push_back({"1% low FPS", buf});
        if (full()) compose_full_(out);
        return out;
    }

    void append_game_lines_(std::vector<Line>& out) const {
        const GameLines& game = GameLines::instance();
        if (!visible() || game.empty()) return;
        out.push_back({"Game", "", true});
        for (const WatchLine& w : game.watches()) out.push_back({w.name, w.value});
        for (const std::string& t : game.texts()) out.push_back({t, ""});
    }

    static std::string bytes_(uint64_t b) {
        char buf[32];
        const double mb = static_cast<double>(b) / (1024.0 * 1024.0);
        if (mb >= 1024.0) std::snprintf(buf, sizeof(buf), "%.2f GB", mb / 1024.0);
        else              std::snprintf(buf, sizeof(buf), "%.0f MB", mb);
        return buf;
    }
    static std::string count_(uint64_t n) {
        char buf[32];
        if (n >= 10'000'000)  std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(n) / 1e6);
        else if (n >= 10'000) std::snprintf(buf, sizeof(buf), "%.1fk", static_cast<double>(n) / 1e3);
        else                  std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(n));
        return buf;
    }

    void compose_full_(std::vector<Line>& out) const {
        char buf[128];
        const OverlayStats& s = stats_;

        out.push_back({"Render", "", true});
        std::snprintf(buf, sizeof(buf), "%ux%u", s.render_width, s.render_height);
        out.push_back({"resolution", buf});
        if (s.profile) {
            const render::FrameProfile::Row& r = *s.profile;
            using render::CpuScope;
            auto cpu = [&](CpuScope sc) { return r.cpu[static_cast<size_t>(sc)]; };
            std::snprintf(buf, sizeof(buf), "%.2f ms", cpu(CpuScope::Frame));
            out.push_back({"cpu frame", buf});
            static constexpr CpuScope kShown[] = {CpuScope::Input, CpuScope::Assets, CpuScope::SceneUpdate,
                                                  CpuScope::LateUpdate, CpuScope::DynamicMeshes, CpuScope::Gather,
                                                  CpuScope::Record, CpuScope::WaitFence, CpuScope::SubmitPresent};
            for (CpuScope sc : kShown) {
                std::snprintf(buf, sizeof(buf), "%.2f", cpu(sc));
                out.push_back({std::string("  ") + render::scope_name(sc), buf});
            }
            if (r.gpu_total > 0.0) {
                std::snprintf(buf, sizeof(buf), "%.2f ms", r.gpu_total);
                out.push_back({"gpu frame", buf});
                // The heaviest passes, largest first.
                std::array<size_t, render::kGpuScopeCount> order{};
                for (size_t i = 0; i < order.size(); ++i) order[i] = i;
                std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return r.gpu[a] > r.gpu[b]; });
                for (size_t k = 0; k < 8 && r.gpu[order[k]] > 0.0; ++k) {
                    std::snprintf(buf, sizeof(buf), "%.2f", r.gpu[order[k]]);
                    out.push_back({std::string("  ") + render::scope_name(static_cast<render::GpuScope>(order[k])), buf});
                }
            }
        } else {
            out.push_back({"timings", "(warming up)"});
        }

        out.push_back({"Draws", "", true});
        out.push_back({"draws / inst", count_(s.draws) + " / " + count_(s.instances)});
        out.push_back({"triangles", count_(s.triangles)});
        out.push_back({"shadow draws / tri", count_(s.shadow_draws) + " / " + count_(s.shadow_triangles)});
        out.push_back({"renderers vis / all", count_(s.renderers_visible) + " / " + count_(s.renderers)});

        out.push_back({"Scene", "", true});
        out.push_back({"objects / comps", count_(s.objects) + " / " + count_(s.components)});
        out.push_back({"particles", count_(s.particles)});
        out.push_back({"pending assets", count_(s.pending_assets)});

        if (s.has_physics) {
            out.push_back({"Physics", "", true});
            out.push_back({"bodies awake / all", count_(s.bodies_awake) + " / " + count_(s.bodies)});
            out.push_back({"contacts", count_(s.contacts)});
            std::snprintf(buf, sizeof(buf), "%.2f ms", s.physics_ms);
            out.push_back({"step", buf});
        }
        if (s.has_nav) {
            out.push_back({"Navigation", "", true});
            out.push_back({"agents", count_(s.nav_agents)});
            out.push_back({"paths planned", count_(s.nav_paths)});
            std::snprintf(buf, sizeof(buf), "%.2f ms", s.nav_ms);
            out.push_back({"update", buf});
        }
        if (!s.heaps.empty()) {
            out.push_back({"GPU memory", "", true});
            for (size_t i = 0; i < s.heaps.size(); ++i) {
                const OverlayStats::Heap& h = s.heaps[i];
                if (h.budget == 0 && h.usage == 0) continue;
                std::snprintf(buf, sizeof(buf), "heap %zu%s", i, h.device_local ? " (device)" : "");
                out.push_back({buf, bytes_(h.usage) + " / " + bytes_(h.budget)});
            }
        }
    }

    OverlayMode    mode_ = OverlayMode::Off;
    bool           accepting_synced_ = false;
    FrameTimeRing  ring_;
    uint64_t       frame_ = 0;
    bool           refresh_now_ = true;
    bool           skip_next_ = false;
    OverlayStats   stats_;
    std::vector<Line> numbers_;   ///< compose_numbers_() as of the last refresh.
    std::vector<Line> lines_;     ///< What draw() shows this frame.
    std::unique_ptr<coopa::scene::Scene> scene_;
    coopa::ui::ImmediateCanvas* canvas_ = nullptr;
};

} // namespace debug
} // namespace toy

#endif // TOYENGINE_DEBUG_DEBUG_OVERLAY_H
