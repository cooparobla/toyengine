#include <toyengine/audio/audio_demo_panel.h>

#include <vector>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <sfxcoopa/components/audio_source.h>
#include <uicoopa/audio/ui_audio.h>

#include <toyengine/audio/audio_components.h>
#include <toyengine/audio/audio_system.h>
#include <toyengine/audio/music_player.h>

namespace toy::audio {
namespace {

using coopa::ui::imm::Anchor;
using coopa::ui::imm::Box;
using coopa::ui::imm::Context;

/// A row of equal buttons across `b`; returns the index pressed, or -1.
int button_row(Context& ctx, std::string_view id, const Box& b,
               const std::vector<std::string>& labels) {
    if (labels.empty()) return -1;
    int hit = -1;
    const float gap = ctx.style.spacing;
    const float w = (b.w - gap * static_cast<float>(labels.size() - 1)) /
                    static_cast<float>(labels.size());
    ctx.push_id(id);
    for (size_t i = 0; i < labels.size(); ++i) {
        const Box cell{b.x + static_cast<float>(i) * (w + gap), b.y, w, b.h};
        ctx.push_id(static_cast<int64_t>(i));
        if (ctx.button(labels[i], cell)) hit = static_cast<int>(i);
        ctx.pop_id();
    }
    ctx.pop_id();
    return hit;
}

}  // namespace

const std::vector<AudioDemoPanel::Entry>& AudioDemoPanel::sources() {
    static const std::vector<Entry> v{
        {"North beacon (front)", "beacon_north"}, {"East beacon (right)", "beacon_east"},
        {"South beacon (behind)", "beacon_south"}, {"West beacon (left)", "beacon_west"},
        {"Orbiting drone (doppler)", "drone"},    {"Crickets", "crickets"},
        {"Chimes", "chimes"},                     {"Campfire", "campfire"},
        {"Music box", "music_box"},
    };
    return v;
}

const std::vector<AudioDemoPanel::Entry>& AudioDemoPanel::pings() {
    static const std::vector<Entry> v{
        {"Left", "ping_left"},     {"Right", "ping_right"}, {"Front", "ping_front"},
        {"Behind", "ping_behind"}, {"Above", "ping_above"},
    };
    return v;
}

const std::vector<AudioDemoPanel::Entry>& AudioDemoPanel::sounds() {
    static const std::vector<Entry> v{
        {"Confirm", "confirm"},     {"Denied", "denied"},   {"Coin", "coin"},
        {"Level up", "level_up"},   {"Error", "error"},     {"Notify", "notify"},
        {"Page turn", "page_turn"}, {"Powerup", "powerup"}, {"Whoosh", "whoosh_short"},
    };
    return v;
}

const std::vector<AudioDemoPanel::Entry>& AudioDemoPanel::buses() {
    static const std::vector<Entry> v{
        {"Master", "master"}, {"Music", "music"}, {"Effects", "sfx"},
    };
    return v;
}

void AudioDemoPanel::draw(Context& ctx) {
    using coopa::ui::imm::Box;

    // Left side, full height minus a margin: the demo is about walking around, so the panel
    // keeps the middle of the screen clear.
    const Box frame = tune().box(ctx, "panel", Anchor::MiddleLeft, {24.0f, 0.0f}, {520.0f, 760.0f});
    const Box inner = ctx.panel_frame("audio", frame, "Audio");

    const float tabs_h = ctx.style.row_height + 6.0f;
    static const std::vector<std::string> pages{"Music", "Spatial", "UI Sounds", "Mixer"};
    ctx.tab_bar("pages", {inner.x, inner.y, inner.w, tabs_h}, pages, &page_);

    const Box body{inner.x, inner.y + tabs_h + ctx.style.spacing, inner.w,
                   std::max(0.0f, inner.h - tabs_h - ctx.style.spacing)};
    switch (page_) {
        case 0: draw_music_(ctx, body); break;
        case 1: draw_spatial_(ctx, body); break;
        case 2: draw_sounds_(ctx, body); break;
        default: draw_mixer_(ctx, body); break;
    }
}

void AudioDemoPanel::draw_music_(Context& ctx, const Box& body) {
    AudioSystem* audio = AudioSystem::active();
    if (!audio) {
        ctx.begin_region("music", body, false);
        ctx.label_dim("No audio device.");
        ctx.end_region();
        return;
    }
    MusicPlayer& music = audio->music();
    const float row = ctx.style.row_height + 14.0f;

    ctx.begin_region("music", body, true);
    const std::string now = music.current();
    ctx.heading(now.empty() ? "Nothing playing" : now);
    ctx.label_dim(music.paused() ? "Paused" : (music.playing() ? "Playing" : "Stopped"));
    ctx.spacing();

    ctx.label_dim("Transport");
    const Box t = ctx.next_box(row);
    switch (button_row(ctx, "transport", t, {"Prev", music.paused() ? "Resume" : "Pause", "Next", "Stop"})) {
        case 0: music.previous(); break;
        case 1: music.paused() ? music.resume(0.5f) : music.pause(0.5f); break;
        case 2: music.next(); break;
        case 3: music.stop(); break;
        default: break;
    }
    ctx.spacing();

    ctx.label_dim("Playlist (crossfades on its own as a track ends)");
    if (ctx.button("Restart playlist", ctx.next_box(row))) {
        coopa::scene::SceneObject* obj =
            (scene && !playlist_object.empty()) ? scene->find_object(playlist_object) : nullptr;
        if (auto* list = obj ? obj->get_component<MusicPlaylistComponent>() : nullptr) list->play();
    }
    ctx.spacing();

    ctx.label_dim("Play a track from code");
    const Box d = ctx.next_box(row);
    const int direct = button_row(ctx, "direct", d, {"Meadow", "Expedition", "Nocturne"});
    if (direct == 0) music.play("meadow", fade_seconds);
    else if (direct == 1) music.play("expedition", fade_seconds);
    else if (direct == 2) music.play("nocturne", fade_seconds);
    ctx.spacing();

    ctx.label_dim("Push a track over the music, then pop back");
    const Box st = ctx.next_box(row);
    const int stack = button_row(ctx, "stack", st, {"Push boss (0.4 s)", "Pop"});
    if (stack == 0) music.push("expedition", 0.4f);
    else if (stack == 1) music.pop();
    ctx.spacing();

    ctx.paragraph("The violet zone (south-west) pushes the night track while you stand in it; "
                  "the paused playlist resumes where it stopped when you leave.");
    ctx.end_region();
}

void AudioDemoPanel::source_toggle_(Context& ctx, const char* label, const char* object) {
    coopa::scene::SceneObject* obj = scene ? scene->find_object(object) : nullptr;
    auto* src = obj ? obj->get_component<coopa::sfx::components::AudioSource>() : nullptr;
    if (!src) {
        ctx.label_dim(std::string(label) + "  (missing)");
        return;
    }
    // The source IS the state: no mirror to drift, so one stopped elsewhere reads as off here
    // on the next frame.
    bool on = src->is_playing();
    if (ctx.property_bool(label, &on)) {
        if (on) src->play();
        else src->stop();
    }
}

void AudioDemoPanel::source_button_(Context& ctx, const Box& b, const char* label,
                                    const char* object) {
    if (!ctx.button(label, b)) return;
    coopa::scene::SceneObject* obj = scene ? scene->find_object(object) : nullptr;
    if (auto* src = obj ? obj->get_component<coopa::sfx::components::AudioSource>() : nullptr) {
        src->play();
    }
}

void AudioDemoPanel::draw_spatial_(Context& ctx, const Box& body) {
    ctx.begin_region("spatial", body, true);
    ctx.paragraph("WASD move, Q/E down/up, arrow keys turn. Use headphones.");
    ctx.spacing();
    for (const Entry& e : sources()) source_toggle_(ctx, e.label, e.name);
    ctx.spacing();

    ctx.label_dim("Ping from a direction");
    const Box r = ctx.next_box(ctx.style.row_height + 10.0f);
    const float gap = ctx.style.spacing;
    const std::vector<Entry>& p = pings();
    const float w = (r.w - gap * static_cast<float>(p.size() - 1)) / static_cast<float>(p.size());
    for (size_t i = 0; i < p.size(); ++i) {
        ctx.push_id(static_cast<int64_t>(i));
        source_button_(ctx, {r.x + static_cast<float>(i) * (w + gap), r.y, w, r.h},
                       p[i].label, p[i].name);
        ctx.pop_id();
    }
    ctx.end_region();
}

void AudioDemoPanel::draw_sounds_(Context& ctx, const Box& body) {
    ctx.begin_region("sounds", body, true);
    if (!coopa::ui::UiAudio::active()) {
        ctx.label_dim("No UI audio.");
        ctx.end_region();
        return;
    }
    ctx.paragraph("The named sounds a UI plays, through the UI bus.");
    ctx.spacing();
    const std::vector<Entry>& snd = sounds();
    for (size_t i = 0; i < snd.size(); ++i) {
        ctx.push_id(static_cast<int64_t>(i));
        if (ctx.button(snd[i].label, ctx.next_box(ctx.style.row_height + 10.0f))) {
            coopa::ui::UiAudio::active()->play(snd[i].name);
        }
        ctx.pop_id();
    }
    ctx.end_region();
}

void AudioDemoPanel::draw_mixer_(Context& ctx, const Box& body) {
    AudioSystem* audio = AudioSystem::active();
    ctx.begin_region("mixer", body, true);
    if (!audio) {
        ctx.label_dim("No audio device.");
        ctx.end_region();
        return;
    }
    ctx.paragraph("Bus volumes, linear. Everything passes a limiter afterwards, so values "
                  "above 1 push the mix without clipping it.");
    ctx.spacing();
    for (const Entry& e : buses()) {
        // Remembered, not just set: a volume the player picks should survive the session.
        float v = bus_volume_persisted(e.name);
        if (ctx.slider_float(e.label, &v, 0.0f, 1.5f, "%.2f")) set_bus_volume_persisted(e.name, v);
    }
    ctx.end_region();
}

}  // namespace toy::audio
