#include <toyengine/debug/debug_overlay.h>

namespace toy {
namespace debug {

std::optional<OverlayMode> parse_overlay_mode(std::string_view s) {
    if (s == "off" || s == "false" || s.empty()) return OverlayMode::Off;
    if (s == "fps") return OverlayMode::Fps;
    if (s == "full") return OverlayMode::Full;
    return std::nullopt;
}

const char* overlay_mode_name(OverlayMode m) {
    switch (m) {
        case OverlayMode::Fps:  return "fps";
        case OverlayMode::Full: return "full";
        default:                return "off";
    }
}

OverlayMode next_overlay_mode(OverlayMode m) {
    switch (m) {
        case OverlayMode::Off: return OverlayMode::Fps;
        case OverlayMode::Fps: return OverlayMode::Full;
        default:               return OverlayMode::Off;
    }
}

void FrameTimeRing::push(float ms) {
    values_[head_] = ms;
    head_ = (head_ + 1) % kCapacity;
    count_ = std::min(count_ + 1, kCapacity);
}

float FrameTimeRing::average() const {
    if (empty()) return 0.0f;
    double sum = 0.0;
    for (size_t i = 0; i < count_; ++i) sum += at(i);
    return static_cast<float>(sum / static_cast<double>(count_));
}

float FrameTimeRing::min() const {
    float m = empty() ? 0.0f : at(0);
    for (size_t i = 1; i < count_; ++i) m = std::min(m, at(i));
    return m;
}

float FrameTimeRing::max() const {
    float m = empty() ? 0.0f : at(0);
    for (size_t i = 1; i < count_; ++i) m = std::max(m, at(i));
    return m;
}

float FrameTimeRing::low_1pct_fps() const {
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

GameLines& GameLines::instance() {
    static GameLines lines;
    return lines;
}

void GameLines::set_accepting(bool on) {
    accepting_ = on;
    if (!on) clear();
}

void GameLines::watch(std::string_view name, std::string value) {
    if (!accepting_) return;
    for (WatchLine& w : watches_) {
        if (w.name == name) { w.value = std::move(value); return; }
    }
    watches_.push_back(WatchLine{std::string(name), std::move(value)});
}

void GameLines::clear() {
    watches_.clear();
    texts_.clear();
}

std::string format_watch(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return buf;
}

std::string format_watch(const glm::vec2& v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "(%.3f, %.3f)", v.x, v.y);
    return buf;
}

std::string format_watch(const glm::vec3& v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "(%.3f, %.3f, %.3f)", v.x, v.y, v.z);
    return buf;
}

std::string format_watch(const glm::vec4& v) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "(%.3f, %.3f, %.3f, %.3f)", v.x, v.y, v.z, v.w);
    return buf;
}

} // namespace debug
} // namespace toy
