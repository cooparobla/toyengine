#include <toyengine/render/frame_profile.h>

namespace toy {
namespace render {

const char* scope_name(CpuScope s) {
    static const char* names[kCpuScopeCount] = {
        "frame", "input", "assets", "scene_update", "late_update", "dynamic_meshes",
        "render", "render.wait_fence", "render.gather", "render.record", "render.submit_present"};
    return names[static_cast<size_t>(s)];
}

const char* scope_name(GpuScope s) {
    static const char* names[kGpuScopeCount] = {
        "skinning", "particles.sim", "shadow.directional", "shadow.local", "gbuffer", "hiz",
        "temporal_history", "contact_shadows", "ssao", "sky", "clouds", "cloud_shadow", "lighting+sky", "scene_color_mips",
        "ssr.trace", "ssr.resolve", "ssr.blur", "ssgi.trace", "ssgi.resolve", "ssgi.blur", "ssr.composite",
        "refraction_mips", "transparent", "underwater", "fog", "volumetrics.inject", "volumetrics.integrate", "volumetrics", "scene_color_history", "motion_blur", "dof", "bloom", "exposure",
        "stylize", "world_ui", "aa", "tilt_shift", "overlay", "present"};
    return names[static_cast<size_t>(s)];
}

FrameProfile::FrameProfile(std::string csv_path) : path_(std::move(csv_path)) {
    const std::filesystem::path p(path_);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    csv_.open(path_, std::ios::trunc);
    csv_ << "frame";
    for (size_t i = 0; i < kCpuScopeCount; ++i) csv_ << ",cpu." << scope_name(static_cast<CpuScope>(i));
    csv_ << ",gpu.total";
    for (size_t i = 0; i < kGpuScopeCount; ++i) csv_ << ",gpu." << scope_name(static_cast<GpuScope>(i));
    csv_ << "\n";
}

void FrameProfile::begin_frame(uint64_t frame) {
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

void FrameProfile::set_gpu(uint64_t frame, const std::array<double, kGpuScopeCount>& scopes, double total) {
    auto it = rows_.find(frame);
    if (it == rows_.end()) return;   // too old (already dropped)
    it->second.gpu       = scopes;
    it->second.gpu_total = total;
    it->second.gpu_done  = true;
    try_emit_(frame);
}

void FrameProfile::prune() {
    for (auto it = rows_.begin(); it != rows_.end();) {
        if (it->first + 8 < current_) {
            if (live_ && it->second.cpu_done && (!has_latest_ || latest_.frame < it->first)) {
                latest_ = it->second;
                latest_.frame = it->first;
                has_latest_ = true;
            }
            it = rows_.erase(it);
        } else {
            ++it;
        }
    }
}

void FrameProfile::print_summary() const {
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

void FrameProfile::try_emit_(uint64_t frame) {
    auto it = rows_.find(frame);
    if (it == rows_.end() || !it->second.cpu_done || !it->second.gpu_done) return;
    Row& r = it->second;
    r.frame = frame;
    latest_     = r;
    has_latest_ = true;
    if (live_) {
        rows_.erase(it);
        return;
    }
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

CpuTimer::~CpuTimer() {
    if (profile_) {
        profile_->add_cpu(scope_, std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - start_).count());
    }
}

} // namespace render
} // namespace toy
