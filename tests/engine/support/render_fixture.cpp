/** @file render_fixture.cpp  @brief Definitions for render_fixture.h (see its file doc). */
#include "engine/support/render_fixture.h"

#include <cstdlib>
#include <iostream>

#include <coopa/testing/test.h>
#include <toyengine/core/engine.h>

#include <root_directory.h>

namespace toy::test {

toy::core::AppConfig make_test_config(const std::string& scene, uint32_t win_w, uint32_t win_h,
                                      uint32_t rw, uint32_t rh) {
    toy::core::AppConfig config;
    config.window.title   = "toyengine_tests";
    config.window.width   = win_w;
    config.window.height  = win_h;
    config.window.vsync   = false;
    config.window.visible = false;

    config.scene.default_scene = scene;

    config.render.render_width          = rw;
    config.render.render_height         = rh;
    config.render.ssao_temporal_enabled = false;
    config.render.ssr_temporal_enabled  = false;
    config.render.contact_shadow_temporal_enabled = false;
    // The stochastic ray jitter itself is deliberately NOT disabled here: SsrPass pins its
    // noise seed to frame 0 whenever the temporal resolve is off (see its execute()), so the
    // jittered trace is deterministic frame to frame and this config still exercises the code
    // path that ships.
    // Global fog is on by default; at a test camera's overview distances it washes out the
    // colours the render tests check. A test that wants fog enables it in its scene settings.
    config.render.fog_enabled = false;

    config.output.save_on_exit = false;
    return config;
}

toy::core::AppConfig make_shipped_config(const std::string& scene, uint32_t rw, uint32_t rh) {
    toy::core::AppConfig config =
        toy::core::AppConfig::load(std::string(ROOT_DIR) + "/assets/config.yaml");
    config.window.width   = rw * 2;
    config.window.height  = rh * 2;
    config.window.visible = false;
    config.window.vsync   = false;
    config.render.render_width  = rw;
    config.render.render_height = rh;
    config.scene.default_scene  = scene;
    config.output.save_on_exit  = false;
    return config;
}

void tick_frames(toy::core::Engine& engine, int frames) {
    for (int i = 0; i < frames; ++i) engine.tick();
}

int tick_until(toy::core::Engine& engine, int max_frames, const std::function<bool()>& predicate) {
    for (int i = 0; i < max_frames; ++i) {
        if (predicate()) return i;
        engine.tick();
    }
    return max_frames;
}

void dump_frame(const Frame& frame, std::string_view name) {
    const std::string path = (coopa::test::scratch_dir() / (std::string(name) + ".png")).string();
    coopa::gfx::util::save_image_png(frame, path);
    std::cerr << "         wrote " << path << "\n";
}

bool same_extent(const Frame& a, const Frame& b) {
    return a.width == b.width && a.height == b.height && a.channels == b.channels;
}

long long count_diff(const Frame& a, const Frame& b, int tolerance) {
    if (!same_extent(a, b)) return -1;
    long long n = 0;
    const size_t pixels = static_cast<size_t>(a.width) * a.height;
    for (size_t i = 0; i < pixels; ++i) {
        for (int ch = 0; ch < 3; ++ch) {
            const int d = static_cast<int>(a.pixels[i * a.channels + ch]) -
                          static_cast<int>(b.pixels[i * b.channels + ch]);
            if (std::abs(d) > tolerance) { ++n; break; }
        }
    }
    return n;
}

long long count_near_color(const Frame& f, int r, int g, int b, int tolerance) {
    long long n = 0;
    const size_t pixels = static_cast<size_t>(f.width) * f.height;
    for (size_t i = 0; i < pixels; ++i) {
        const int pr = f.pixels[i * f.channels + 0];
        const int pg = f.pixels[i * f.channels + 1];
        const int pb = f.pixels[i * f.channels + 2];
        if (std::abs(pr - r) <= tolerance && std::abs(pg - g) <= tolerance &&
            std::abs(pb - b) <= tolerance) {
            ++n;
        }
    }
    return n;
}

long long count_nonblack(const Frame& f) {
    long long n = 0;
    const size_t pixels = static_cast<size_t>(f.width) * f.height;
    for (size_t i = 0; i < pixels; ++i) {
        if (f.pixels[i * f.channels + 0] || f.pixels[i * f.channels + 1] ||
            f.pixels[i * f.channels + 2]) {
            ++n;
        }
    }
    return n;
}

double mean_abs_delta(const Frame& a, const Frame& b) {
    if (a.pixels.size() != b.pixels.size() || a.pixels.empty()) return -1.0;
    long long sum = 0;
    for (size_t k = 0; k < a.pixels.size(); k += a.channels) {
        sum += std::abs(int(a.pixels[k]) - int(b.pixels[k]));
    }
    return double(sum) / double(a.width * a.height);
}

long long first_off_palette_pixel(const Frame& f, const uint8_t palette[][3], size_t entries) {
    const size_t pixels = static_cast<size_t>(f.width) * f.height;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t r = f.pixels[i * f.channels + 0];
        const uint8_t g = f.pixels[i * f.channels + 1];
        const uint8_t b = f.pixels[i * f.channels + 2];
        bool found = false;
        for (size_t e = 0; e < entries; ++e) {
            if (palette[e][0] == r && palette[e][1] == g && palette[e][2] == b) { found = true; break; }
        }
        if (!found) return static_cast<long long>(i);
    }
    return -1;
}

glm::vec3 band_mean(const Frame& f, float y0, float y1, float x0, float x1) {
    glm::dvec3 sum(0.0);
    long long n = 0;
    for (uint32_t y = uint32_t(y0 * f.height); y < uint32_t(y1 * f.height); ++y) {
        for (uint32_t x = uint32_t(x0 * f.width); x < uint32_t(x1 * f.width); ++x) {
            const uint8_t* p = &f.pixels[(size_t(y) * f.width + x) * f.channels];
            sum += glm::dvec3(p[0], p[1], p[2]);
            ++n;
        }
    }
    return n ? glm::vec3(sum / double(n)) : glm::vec3(0.0f);
}

long long black_block_pixels(const Frame& f) {
    long long n = 0;
    for (uint32_t by = 0; by + 8 <= f.height; by += 8) {
        for (uint32_t bx = 0; bx + 8 <= f.width; bx += 8) {
            bool black = true;
            for (uint32_t y = by; y < by + 8 && black; ++y)
                for (uint32_t x = bx; x < bx + 8 && black; ++x) {
                    const uint8_t* p = &f.pixels[(size_t(y) * f.width + x) * f.channels];
                    black = p[0] < 2 && p[1] < 2 && p[2] < 2;
                }
            if (black) n += 64;
        }
    }
    return n;
}

} // namespace toy::test
