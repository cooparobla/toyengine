/**
 * @file frame_profile.h
 * @brief Profiling mode's data sink: one row of CPU + GPU timings per frame, streamed to a
 *        CSV for graphing (tools/plot_profile.py) and summarised on exit.
 *
 * Enabled by the PROFILE env var (see main.cpp); without it nothing here is constructed and
 * the frame loop pays nothing.
 *
 * The scope sets are FIXED enums, not free-form strings: every CSV row then has the same
 * columns whether or not a feature ran that frame (a disabled feature's column reads 0),
 * which is what a plotting tool needs.
 *
 * GPU timings come from GpuProfiler and arrive two frames late (a frame slot's timestamps can
 * only be read once that slot's fence has signalled), so a row is held until both its CPU and
 * GPU halves are in, then written.
 */

#ifndef TOYENGINE_RENDER_FRAME_PROFILE_H
#define TOYENGINE_RENDER_FRAME_PROFILE_H

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace toy {
namespace render {

/// CPU phases of one frame (Engine::tick and PixelRenderPipeline::render).
enum class CpuScope : uint32_t {
    Frame,          ///< Tick start to next tick start -- the true frame time.
    Input,          ///< Window poll + input.
    Assets,         ///< AssetManager::update (async load finalization).
    SceneUpdate,    ///< Controllers + SceneManager::update (scripts, physics, transforms).
    LateUpdate,     ///< UI canvases + late_update.
    DynamicMeshes,  ///< Cloth / skinned mesh uploads, debug lines.
    Render,         ///< All of PixelRenderPipeline::render().
    WaitFence,      ///< Inside Render: waiting for this frame slot's previous GPU work.
    Gather,         ///< Inside Render: scene snapshot, lights, culling/batching, SDF, UI, UBOs.
    Record,         ///< Inside Render: recording the frame's command buffer.
    SubmitPresent,  ///< Inside Render: swapchain acquire + submit + present. Includes blocking
                    ///< on the GPU (image back-pressure), so a GPU-bound frame shows up here.
    Count
};

/// GPU scopes, in record order. Contiguous: each covers the time since the previous one,
/// so together they sum to the whole GPU frame.
enum class GpuScope : uint32_t {
    ShadowDirectional,
    ShadowPoint,
    ShadowSpot,
    GBuffer,
    HiZ,                 ///< Hi-Z + SSAO depth pyramids.
    TransparentCapture,  ///< SSR's second source: capture + its Hi-Z and colour mips.
    TemporalHistory,
    ContactShadows,
    Ssao,
    LightingSky,
    SceneColorMips,
    SsrTrace,            ///< SsrPass sub-passes, via SsrPass::set_stage_hook():
    SsrResolve,
    SsrBlur,
    SsgiTrace,
    SsgiResolve,
    SsgiBlur,
    SsrComposite,        ///< composite + history copies
    RefractionMips,
    Transparent,
    Fog,
    VolumetricsInject,     ///< froxel mode only: density + lighting into the grid
    VolumetricsIntegrate,  ///< froxel mode only: per-column accumulation
    Volumetrics,           ///< raymarch mode: march + composite; froxel mode: the apply
    Dof,
    Bloom,
    Exposure,
    Stylize,
    WorldUi,
    Aa,
    TiltShift,
    Overlay,             ///< UI composite + upscale + screen UI.
    Present,             ///< Final swapchain copy.
    Count
};

inline constexpr size_t kCpuScopeCount = static_cast<size_t>(CpuScope::Count);
inline constexpr size_t kGpuScopeCount = static_cast<size_t>(GpuScope::Count);

inline const char* scope_name(CpuScope s) {
    static const char* names[kCpuScopeCount] = {
        "frame", "input", "assets", "scene_update", "late_update", "dynamic_meshes",
        "render", "render.wait_fence", "render.gather", "render.record", "render.submit_present"};
    return names[static_cast<size_t>(s)];
}

inline const char* scope_name(GpuScope s) {
    static const char* names[kGpuScopeCount] = {
        "shadow.directional", "shadow.point", "shadow.spot", "gbuffer", "hiz", "transparent_capture",
        "temporal_history", "contact_shadows", "ssao", "lighting+sky", "scene_color_mips",
        "ssr.trace", "ssr.resolve", "ssr.blur", "ssgi.trace", "ssgi.resolve", "ssgi.blur", "ssr.composite",
        "refraction_mips", "transparent", "fog", "volumetrics.inject", "volumetrics.integrate", "volumetrics", "dof", "bloom", "exposure",
        "stylize", "world_ui", "aa", "tilt_shift", "overlay", "present"};
    return names[static_cast<size_t>(s)];
}

/**
 * @class FrameProfile
 * @brief Collects per-frame timings, writes them to CSV, and summarises them.
 */
class FrameProfile {
public:
    /// Frames excluded from the summary (startup, pipeline creation, first-use costs) --
    /// matches Engine::run()'s frame-time warm-up. The CSV keeps every frame.
    static constexpr uint64_t kWarmupFrames = 30;

    explicit FrameProfile(std::string csv_path) : path_(std::move(csv_path)) {
        const std::filesystem::path p(path_);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
        csv_.open(path_, std::ios::trunc);
        csv_ << "frame";
        for (size_t i = 0; i < kCpuScopeCount; ++i) csv_ << ",cpu." << scope_name(static_cast<CpuScope>(i));
        csv_ << ",gpu.total";
        for (size_t i = 0; i < kGpuScopeCount; ++i) csv_ << ",gpu." << scope_name(static_cast<GpuScope>(i));
        csv_ << "\n";
    }

    ~FrameProfile() { csv_.flush(); }

    const std::string& path() const { return path_; }
    bool ok() const { return static_cast<bool>(csv_); }

    /// Starts frame `frame` (call at the top of each tick). Closes the previous frame's CPU half,
    /// whose cpu.frame is the time between the two calls.
    void begin_frame(uint64_t frame) {
        const auto now = std::chrono::steady_clock::now();
        if (has_frame_) {
            Row& prev = rows_[current_];
            prev.cpu[static_cast<size_t>(CpuScope::Frame)] =
                std::chrono::duration<double, std::milli>(now - frame_start_).count();
            prev.cpu_done = true;
            try_emit_(current_);
        }
        current_     = frame;
        frame_start_ = now;
        has_frame_   = true;
        rows_[frame];   // create
    }

    uint64_t current_frame() const { return current_; }

    /// Adds `ms` to a CPU scope of the current frame (scopes may be entered more than once).
    void add_cpu(CpuScope s, double ms) {
        if (has_frame_) rows_[current_].cpu[static_cast<size_t>(s)] += ms;
    }

    /// Records a completed frame's GPU timings (from GpuProfiler::collect).
    void set_gpu(uint64_t frame, const std::array<double, kGpuScopeCount>& scopes, double total) {
        auto it = rows_.find(frame);
        if (it == rows_.end()) return;   // too old (already dropped)
        it->second.gpu       = scopes;
        it->second.gpu_total = total;
        it->second.gpu_done  = true;
        try_emit_(frame);
    }

    /// Drops rows that never got their GPU half (a skipped/resized frame) once they are well
    /// behind the current frame, so the pending map cannot grow without bound.
    void prune() {
        for (auto it = rows_.begin(); it != rows_.end();) {
            if (it->first + 8 < current_) it = rows_.erase(it);
            else ++it;
        }
    }

    /// Writes the end-of-run summary to stdout. Incomplete trailing frames are not included.
    void print_summary() const {
        std::vector<const Row*> rows;
        for (const Row& r : done_) {
            if (r.frame >= kWarmupFrames) rows.push_back(&r);
        }
        std::printf("\n[profile] ===== %zu frames profiled (after %llu warm-up) -> %s =====\n",
                    rows.size(), static_cast<unsigned long long>(kWarmupFrames), path_.c_str());
        if (rows.empty()) {
            std::printf("[profile] (no complete frames -- run with MAX_FRAMES > %llu)\n",
                        static_cast<unsigned long long>(kWarmupFrames + 2));
            return;
        }
        const double n = static_cast<double>(rows.size());

        auto stats = [&](auto value) {
            std::vector<double> v;
            v.reserve(rows.size());
            for (const Row* r : rows) v.push_back(value(*r));
            std::sort(v.begin(), v.end());
            double sum = 0.0;
            for (double x : v) sum += x;
            auto pct = [&](double q) { return v[std::min(v.size() - 1, static_cast<size_t>(q * (v.size() - 1) + 0.5))]; };
            struct S { double avg, p50, p95, max; };
            return S{sum / n, pct(0.5), pct(0.95), v.back()};
        };

        const auto frame = stats([](const Row& r) { return r.cpu[static_cast<size_t>(CpuScope::Frame)]; });
        const auto gpu   = stats([](const Row& r) { return r.gpu_total; });
        // Time the CPU spent blocked on the GPU: the explicit fence wait plus the swapchain
        // acquire/present inside submit (where image back-pressure lands).
        const auto wait  = stats([](const Row& r) {
            return r.cpu[static_cast<size_t>(CpuScope::WaitFence)] +
                   r.cpu[static_cast<size_t>(CpuScope::SubmitPresent)];
        });
        std::printf("[profile] frame %.2f ms avg (%.1f fps), p95 %.2f ms | GPU %.2f ms avg | "
                    "CPU waiting on GPU %.2f ms avg -> %s\n",
                    frame.avg, 1000.0 / frame.avg, frame.p95, gpu.avg, wait.avg,
                    wait.avg > 0.25 * frame.avg ? "GPU-bound" : "CPU-bound");

        std::printf("\n[profile] GPU per feature (timestamps at pass boundaries; the total is exact,\n"
                    "[profile] per-scope splits are approximate -- the GPU overlaps adjacent passes)\n");
        std::printf("[profile]   %-22s %9s %7s %9s %9s %9s\n", "scope", "avg ms", "% gpu", "p50", "p95", "max");
        struct Line { const char* name; double avg, p50, p95, max; };
        std::vector<Line> lines;
        for (size_t i = 0; i < kGpuScopeCount; ++i) {
            const auto s = stats([i](const Row& r) { return r.gpu[i]; });
            if (s.max <= 0.0) continue;   // feature never ran
            lines.push_back({scope_name(static_cast<GpuScope>(i)), s.avg, s.p50, s.p95, s.max});
        }
        std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.avg > b.avg; });
        for (const Line& l : lines) {
            std::printf("[profile]   %-22s %9.3f %6.1f%% %9.3f %9.3f %9.3f\n", l.name, l.avg,
                        gpu.avg > 0.0 ? 100.0 * l.avg / gpu.avg : 0.0, l.p50, l.p95, l.max);
        }
        std::printf("[profile]   %-22s %9.3f %6.1f%% %9.3f %9.3f %9.3f\n", "TOTAL", gpu.avg, 100.0,
                    gpu.p50, gpu.p95, gpu.max);

        std::printf("\n[profile] CPU per phase\n");
        std::printf("[profile]   %-22s %9s %7s %9s %9s %9s\n", "phase", "avg ms", "% frame", "p50", "p95", "max");
        for (size_t i = 0; i < kCpuScopeCount; ++i) {
            const auto s = stats([i](const Row& r) { return r.cpu[i]; });
            std::printf("[profile]   %-22s %9.3f %6.1f%% %9.3f %9.3f %9.3f\n",
                        scope_name(static_cast<CpuScope>(i)), s.avg,
                        frame.avg > 0.0 ? 100.0 * s.avg / frame.avg : 0.0, s.p50, s.p95, s.max);
        }
        std::printf("\n");
    }

private:
    struct Row {
        uint64_t frame = 0;
        std::array<double, kCpuScopeCount> cpu{};
        std::array<double, kGpuScopeCount> gpu{};
        double gpu_total = 0.0;
        bool   cpu_done = false, gpu_done = false;
    };

    void try_emit_(uint64_t frame) {
        auto it = rows_.find(frame);
        if (it == rows_.end() || !it->second.cpu_done || !it->second.gpu_done) return;
        Row& r = it->second;
        r.frame = frame;
        csv_ << frame;
        char buf[32];
        for (double v : r.cpu) { std::snprintf(buf, sizeof(buf), ",%.4f", v); csv_ << buf; }
        std::snprintf(buf, sizeof(buf), ",%.4f", r.gpu_total);
        csv_ << buf;
        for (double v : r.gpu) { std::snprintf(buf, sizeof(buf), ",%.4f", v); csv_ << buf; }
        csv_ << "\n";
        if (++rows_since_flush_ >= 60) {
            csv_.flush();
            rows_since_flush_ = 0;
        }
        done_.push_back(r);
        rows_.erase(it);
    }

    std::string   path_;
    std::ofstream csv_;
    std::map<uint64_t, Row> rows_;   ///< Frames still waiting for their CPU or GPU half.
    std::vector<Row>        done_;   ///< Completed frames, for the summary.
    uint64_t current_ = 0;
    bool     has_frame_ = false;
    std::chrono::steady_clock::time_point frame_start_{};
    int      rows_since_flush_ = 0;
};

/// RAII CPU timer: adds its lifetime to `scope` of the profile's current frame. A null profile
/// makes it a no-op, so call sites need no branching.
class CpuTimer {
public:
    CpuTimer(FrameProfile* profile, CpuScope scope)
        : profile_(profile), scope_(scope),
          start_(profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~CpuTimer() {
        if (profile_) {
            profile_->add_cpu(scope_, std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - start_).count());
        }
    }
    CpuTimer(const CpuTimer&) = delete;
    CpuTimer& operator=(const CpuTimer&) = delete;

private:
    FrameProfile* profile_;
    CpuScope      scope_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_FRAME_PROFILE_H
