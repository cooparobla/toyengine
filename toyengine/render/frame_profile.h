/**
 * @file frame_profile.h
 * @brief Profiling mode's data sink: one row of CPU + GPU timings per frame, streamed to a
 *        CSV for graphing (tools/plot_profile.py) and summarised on exit.
 *
 * Enabled by the PROFILE env var (see main.cpp); without it nothing here is constructed and
 * the frame loop pays nothing. The debug overlay's full mode (toyengine/debug/debug_overlay.h)
 * constructs one in LIVE mode instead: no CSV and no summary, only the last completed row,
 * read through latest().
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

/// CPU phases of one frame (Engine::tick and ToyRenderPipeline::render).
enum class CpuScope : uint32_t {
    Frame,          ///< Tick start to next tick start -- the true frame time.
    Input,          ///< Window poll + input.
    Assets,         ///< AssetManager::update (async load finalization).
    SceneUpdate,    ///< Controllers + SceneManager::update (scripts, physics, transforms).
    LateUpdate,     ///< UI canvases + late_update.
    DynamicMeshes,  ///< Cloth / skinned mesh uploads, debug lines.
    Render,         ///< All of ToyRenderPipeline::render().
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
    Skinning,            ///< Compute pre-pass: GPU skinning dispatches (before any pass reads vertices).
    ParticlesSim,        ///< Compute pre-pass: GPU particle emit / simulate / compact / sort.
    ShadowDirectional,
    ShadowLocal,         ///< Every point and spot shadow (the local-light atlas, cache copy included).
    GBuffer,
    HiZ,                 ///< Hi-Z + SSAO depth pyramids.
    TemporalHistory,
    ContactShadows,
    Ssao,
    Sky,                 ///< Physical sky: the atmosphere tables (when changed) + the sky-view table.
    Clouds,              ///< The volumetric cloud march + reconstruction (either sky model).
    CloudShadow,         ///< The cloud shadow map (render cloud_shadows).
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
    Underwater,          ///< UnderwaterPass (a copy when the camera is above water).
    Fog,
    VolumetricsInject,     ///< froxel mode only: density + lighting into the grid
    VolumetricsIntegrate,  ///< froxel mode only: per-column accumulation
    Volumetrics,           ///< raymarch mode: march + composite; froxel mode: the apply
    SceneColorHistory,     ///< pre-DOF HDR copied into the SSR colour chain's mip 0 for next frame
    MotionBlur,            ///< tile max + neighbour max + copy + gather (only when motion_blur records)
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

const char* scope_name(CpuScope s);

const char* scope_name(GpuScope s);

/**
 * @class FrameProfile
 * @brief Collects per-frame timings, writes them to CSV, and summarises them.
 */
class FrameProfile {
public:
    /// Frames excluded from the summary (startup, pipeline creation, first-use costs) --
    /// matches Engine::run()'s frame-time warm-up. The CSV keeps every frame.
    static constexpr uint64_t kWarmupFrames = 30;

    /// Live mode: no file, no summary -- only latest() is kept (the debug overlay's source).
    FrameProfile() : live_(true) {}

    explicit FrameProfile(std::string csv_path);

    ~FrameProfile() { if (!live_) csv_.flush(); }

    const std::string& path() const { return path_; }
    bool ok() const { return live_ || static_cast<bool>(csv_); }
    bool live() const { return live_; }

    /// Starts frame `frame` (call at the top of each tick). Closes the previous frame's CPU half,
    /// whose cpu.frame is the time between the two calls.
    void begin_frame(uint64_t frame);

    uint64_t current_frame() const { return current_; }

    /// Adds `ms` to a CPU scope of the current frame (scopes may be entered more than once).
    void add_cpu(CpuScope s, double ms) {
        if (has_frame_) rows_[current_].cpu[static_cast<size_t>(s)] += ms;
    }

    /// Records a completed frame's GPU timings (from GpuProfiler::collect).
    void set_gpu(uint64_t frame, const std::array<double, kGpuScopeCount>& scopes, double total);

    /// Drops rows that never got their GPU half (a skipped/resized frame) once they are well
    /// behind the current frame, so the pending map cannot grow without bound. In live mode a
    /// dropped row with its CPU half still becomes latest() (GPU columns 0) -- on a device
    /// without timestamps no row ever completes, and the CPU timings are still worth showing.
    void prune();

    /// Writes the end-of-run summary to stdout. Incomplete trailing frames are not included.
    void print_summary() const;

    /// One frame's timings, milliseconds. A scope that did not run reads 0.
    struct Row {
        uint64_t frame = 0;
        std::array<double, kCpuScopeCount> cpu{};
        std::array<double, kGpuScopeCount> gpu{};
        double gpu_total = 0.0;
        bool   cpu_done = false, gpu_done = false;
    };

    /// The most recently completed frame (both halves in -- GPU timings land two frames after
    /// the CPU ones), or null before the first one. Kept in both modes.
    const Row* latest() const { return has_latest_ ? &latest_ : nullptr; }

private:
    void try_emit_(uint64_t frame);

    bool          live_ = false;
    std::string   path_;
    std::ofstream csv_;
    Row           latest_{};
    bool          has_latest_ = false;
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
    ~CpuTimer();
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
