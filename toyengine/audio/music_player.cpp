#include <toyengine/audio/music_player.h>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace toy::audio {

namespace {
constexpr int k_music_priority = 100;   // music never loses its voice to a burst of one-shots
constexpr float k_half_pi = 1.57079632679f;
}

/** @brief One playing (or fading, or paused) track. */
struct MusicPlayer::Deck {
    enum Then { Keep, Stop, Pause };

    std::string key;                      ///< Track name (or clip path when unregistered).
    coopa::sfx::mixer::VoiceHandle voice;
    float volume = 1.0f;
    float pitch = 1.0f;
    float duration = 0.0f;                ///< Clip length, seconds.
    float elapsed = 0.0f;                 ///< Seconds played (pitch-scaled; frozen while paused).
    bool loops = false;                   ///< Voice loops: position() wraps.
    bool from_playlist = false;           ///< Auto-advances to the next playlist track near its end.
    bool advanced = false;                ///< Its natural transition already fired.
    float level = 0.0f;                   ///< Fade envelope 0..1 (gain = volume * sin(level * pi/2)).
    float target = 1.0f;
    float rate = 0.0f;                    ///< Envelope change per second, toward target.
    Then then = Keep;                     ///< What happens when the envelope reaches target.
    bool voice_paused = false;
    bool dead = false;
    float sent_gain = -1.0f;
};

MusicPlayer::MusicPlayer(coopa::sfx::core::AudioEngine& engine, std::string bus) : engine_(engine), bus_(std::move(bus)) {}

MusicPlayer::~MusicPlayer() { stop_now(); }

void MusicPlayer::add_track(const MusicTrack& track) {
    MusicTrack t = track;
    if (t.name.empty()) t.name = t.clip;
    for (MusicTrack& existing : tracks_) {
        if (existing.name == t.name) { existing = t; return; }
    }
    tracks_.push_back(std::move(t));
}

const MusicTrack* MusicPlayer::find_track(const std::string& name) const {
    for (const MusicTrack& t : tracks_) if (t.name == name) return &t;
    return nullptr;
}

const MusicTrack& MusicPlayer::track_for_(const std::string& key) {
    if (const MusicTrack* t = find_track(key)) return *t;
    // A bare clip path: registered on first use, so later lookups (current(), playlists) agree.
    add_track(MusicTrack{key, key, 1.0f, true});
    return tracks_.back();
}

bool MusicPlayer::preload(const std::string& key) {
    const MusicTrack& t = track_for_(key);
    try {
        engine_.load_clip(resolve ? resolve(t.clip) : t.clip);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[toyengine] Music: cannot load '" << t.clip << "': " << e.what() << "\n";
        return false;
    }
}

MusicPlayer::Deck* MusicPlayer::start_deck_(const std::string& key, float fade, bool loop) {
    const MusicTrack t = track_for_(key);   // a copy: track_for_ may grow tracks_
    const std::string path = resolve ? resolve(t.clip) : t.clip;
    std::shared_ptr<coopa::sfx::data::AudioClip> clip;
    try {
        clip = engine_.load_clip(path);
    } catch (const std::exception& e) {
        std::cerr << "[toyengine] Music: cannot play '" << t.clip << "': " << e.what() << "\n";
        return nullptr;
    }
    auto d = std::make_unique<Deck>();
    d->key = t.name;
    d->volume = t.volume * clip->settings().volume;
    d->pitch = clip->settings().pitch;
    const uint32_t rate = clip->format().sample_rate;
    d->duration = rate ? static_cast<float>(clip->frame_count()) / static_cast<float>(rate) : 0.0f;
    d->loops = loop;

    coopa::sfx::core::PlayParams p;
    p.bus_name = engine_.find_bus(bus_) ? bus_ : engine_.master().name();
    p.gain = fade > 0.0f ? 0.0f : d->volume;
    p.pitch = d->pitch;
    p.loop = loop;
    p.priority = k_music_priority;
    try {
        d->voice = engine_.play(path, p);
    } catch (const std::exception& e) {
        std::cerr << "[toyengine] Music: cannot play '" << t.clip << "': " << e.what() << "\n";
        return nullptr;
    }
    d->level = fade > 0.0f ? 0.0f : 1.0f;
    d->sent_gain = p.gain;
    fade_deck_(*d, 1.0f, fade, Deck::Keep);
    decks_.push_back(std::move(d));
    return decks_.back().get();
}

void MusicPlayer::fade_deck_(Deck& d, float target, float seconds, int then) {
    d.target = target;
    d.then = static_cast<Deck::Then>(then);
    if (seconds <= 0.0f) {
        d.level = target;   // update() applies `then` on its next pass
        d.rate = 0.0f;
    } else {
        d.rate = std::fabs(target - d.level) / seconds;
    }
}

bool MusicPlayer::play(const std::string& key, float fade, bool restart) {
    const float f = fade_or_default_(fade);
    const MusicTrack& t = track_for_(key);
    if (!restart && current_ && current_->key == t.name && (current_->target > 0.0f || paused_)) {
        clear_playlist();
        current_->from_playlist = false;
        if (paused_) resume(f);
        return true;
    }
    Deck* d = start_deck_(t.name, f, t.loop);
    if (!d) return false;
    clear_playlist();
    if (current_) fade_deck_(*current_, 0.0f, f, Deck::Stop);
    current_ = d;
    paused_ = false;
    on_track_started.emit(d->key);
    return true;
}

void MusicPlayer::stop(float fade) {
    const float f = fade_or_default_(fade);
    if (current_) fade_deck_(*current_, 0.0f, f, Deck::Stop);
    for (Deck* d : stack_) if (d) fade_deck_(*d, 0.0f, 0.0f, Deck::Stop);
    current_ = nullptr;
    stack_.clear();
    clear_playlist();
    paused_ = false;
}

void MusicPlayer::pause(float fade) {
    if (!current_ || paused_) return;
    fade_deck_(*current_, 0.0f, fade, Deck::Pause);
    paused_ = true;
}

void MusicPlayer::resume(float fade) {
    if (!paused_) return;
    paused_ = false;
    if (!current_) return;
    if (current_->voice_paused) {
        engine_.resume(current_->voice);
        current_->voice_paused = false;
    }
    fade_deck_(*current_, 1.0f, fade, Deck::Keep);
}

int MusicPlayer::playlist_slot_of_(const std::string& key) const {
    for (size_t i = 0; i < playlist_.tracks.size(); ++i) {
        const MusicTrack* t = find_track(playlist_.tracks[i]);
        if (playlist_.tracks[i] == key || (t && t->name == key)) return static_cast<int>(i);
    }
    return -1;
}

void MusicPlayer::rebuild_order_(int keep_first) {
    const int n = static_cast<int>(playlist_.tracks.size());
    order_.resize(n);
    for (int i = 0; i < n; ++i) order_[i] = i;
    if (!playlist_.shuffle) {
        cursor_ = keep_first >= 0 ? keep_first : 0;
        return;
    }
    const int previous = (cursor_ >= 0 && cursor_ < static_cast<int>(order_.size())) ? order_[cursor_] : -1;
    std::shuffle(order_.begin(), order_.end(), rng_);
    if (keep_first >= 0) {
        std::iter_swap(order_.begin(), std::find(order_.begin(), order_.end(), keep_first));
    } else if (n > 1 && order_[0] == previous) {
        std::swap(order_[0], order_[n - 1]);   // a reshuffle never repeats the track that just ended
    }
    cursor_ = 0;
}

float MusicPlayer::playlist_fade_(float fade) const { return fade < 0.0f ? playlist_.crossfade : fade; }

void MusicPlayer::set_playlist(const MusicPlaylist& playlist, int start) {
    playlist_ = playlist;
    order_.clear();
    cursor_ = -1;
    if (playlist_.tracks.empty()) return;
    for (const std::string& k : playlist_.tracks) track_for_(k);
    const int n = static_cast<int>(playlist_.tracks.size());

    // The track already playing (from the previous scene, say) carries on as the playlist's.
    const int playing = current_ && stack_.empty() && !paused_ ? playlist_slot_of_(current_->key) : -1;
    if (playing >= 0) {
        rebuild_order_(playing);
        current_->from_playlist = true;
        current_->advanced = false;
        // A looping track joining a playlist plays out its current pass, then moves on.
        if (current_->loops && current_->duration > 0.0f) current_->elapsed = std::fmod(current_->elapsed, current_->duration);
        return;
    }
    int first = (start >= 0 && start < n) ? start : -1;
    if (first < 0 && playlist_.shuffle) first = std::uniform_int_distribution<int>(0, n - 1)(rng_);
    rebuild_order_(first);
    cursor_ -= 1;   // advance_ steps onto it
    advance_(+1, std::min(playlist_.crossfade, 1.5f));   // a quick fade-in from silence
}

bool MusicPlayer::advance_(int step, float fade) {
    if (order_.empty()) return false;
    const int n = static_cast<int>(order_.size());
    int next = cursor_ + step;
    if (next >= n) {
        if (!playlist_.repeat) return false;
        if (playlist_.shuffle) { rebuild_order_(-1); next = 0; }
        else next = 0;
    } else if (next < 0) {
        next = playlist_.repeat ? n - 1 : 0;
    }
    const std::string key = playlist_.tracks[order_[next]];
    // Playlist voices loop underneath: if a frame hitch delays the hand-off, the music never gaps.
    Deck* d = start_deck_(key, fade, true);
    if (!d) return false;
    cursor_ = next;
    d->from_playlist = true;
    if (current_) fade_deck_(*current_, 0.0f, fade, Deck::Stop);
    current_ = d;
    paused_ = false;
    on_track_started.emit(d->key);
    return true;
}

bool MusicPlayer::next(float fade) { return has_playlist() && advance_(+1, playlist_fade_(fade)); }

bool MusicPlayer::previous(float fade) { return has_playlist() && advance_(-1, playlist_fade_(fade)); }

bool MusicPlayer::push(const std::string& key, float fade) {
    const float f = fade_or_default_(fade);
    const MusicTrack& t = track_for_(key);
    Deck* d = start_deck_(t.name, f, t.loop);
    if (!d) return false;
    if (current_) fade_deck_(*current_, 0.0f, f, current_->target > 0.0f || paused_ ? Deck::Pause : Deck::Stop);
    stack_.push_back(current_);
    current_ = d;
    paused_ = false;
    on_track_started.emit(d->key);
    return true;
}

bool MusicPlayer::pop(float fade) {
    if (stack_.empty()) return false;
    const float f = fade_or_default_(fade);
    if (current_) fade_deck_(*current_, 0.0f, f, Deck::Stop);
    Deck* under = stack_.back();
    stack_.pop_back();
    current_ = under;
    paused_ = false;
    if (under) {
        if (under->voice_paused) {
            engine_.resume(under->voice);
            under->voice_paused = false;
        }
        fade_deck_(*under, 1.0f, f, Deck::Keep);
        on_track_started.emit(under->key);
    }
    return true;
}

std::string MusicPlayer::current() const { return current_ ? current_->key : std::string(); }

float MusicPlayer::position() const {
    if (!current_) return 0.0f;
    if (current_->duration > 0.0f && current_->elapsed >= current_->duration) return std::fmod(current_->elapsed, current_->duration);
    return current_->elapsed;
}

float MusicPlayer::duration() const { return current_ ? current_->duration : 0.0f; }

bool MusicPlayer::playing() const {
    for (const auto& d : decks_) if (!d->voice_paused && (d->level > 0.0f || d->target > 0.0f)) return true;
    return false;
}

bool MusicPlayer::transitioning() const {
    for (const auto& d : decks_) if (d->level != d->target) return true;
    return false;
}

void MusicPlayer::update(float dt) {
    dt = std::max(dt, 0.0f);
    bool current_died = false;
    for (auto& dp : decks_) {
        Deck& d = *dp;
        if (!engine_.is_voice_active(d.voice)) {   // ended on its own, or stolen
            d.dead = true;
            if (&d == current_) current_died = true;
            continue;
        }
        if (!d.voice_paused) {
            d.elapsed += dt * d.pitch;
            // Wrap a plain looping track; a playlist track counts on to trigger its hand-off.
            if (d.loops && !d.from_playlist && d.duration > 0.0f && d.elapsed >= d.duration) d.elapsed = std::fmod(d.elapsed, d.duration);
        }
        if (d.level != d.target) {
            const float step = d.rate * dt;
            d.level = d.level < d.target ? std::min(d.target, d.level + step) : std::max(d.target, d.level - step);
        }
        if (d.level == d.target) {
            if (d.then == Deck::Stop) {
                engine_.stop(d.voice);
                d.dead = true;
                continue;
            }
            if (d.then == Deck::Pause && !d.voice_paused) {
                engine_.pause(d.voice);
                d.voice_paused = true;
            }
        }
        const float gain = d.volume * std::sin(d.level * k_half_pi);
        if (std::fabs(gain - d.sent_gain) > 1e-5f) {
            engine_.set_voice_gain(d.voice, gain);
            d.sent_gain = gain;
        }
    }

    // Natural transition: a playlist track nearing its end hands off to the next one.
    if (current_ && !current_->dead && current_->from_playlist && !current_->advanced && stack_.empty() && !paused_ &&
        current_->duration > 0.0f) {
        const float remaining = current_->duration - current_->elapsed;
        if (order_.size() <= 1 && playlist_.repeat) {
            // One-track playlist: its voice simply loops.
            if (remaining <= 0.0f) current_->elapsed = std::fmod(current_->elapsed, current_->duration);
        } else if (remaining <= playlist_.crossfade) {
            current_->advanced = true;
            const float fade = std::clamp(remaining, 0.0f, playlist_.crossfade);
            if (!advance_(+1, fade)) {
                // End of a non-repeating playlist: let the last track fade out with its own tail.
                fade_deck_(*current_, 0.0f, std::max(remaining, 0.05f), Deck::Stop);
                current_ = nullptr;
                clear_playlist();
            }
        }
    }

    if (std::none_of(decks_.begin(), decks_.end(), [](const auto& d) { return d->dead; })) return;
    const bool advance_after = current_died && current_->from_playlist && !current_->advanced;
    for (Deck*& s : stack_) if (s && s->dead) s = nullptr;
    if (current_ && current_->dead) current_ = nullptr;
    decks_.erase(std::remove_if(decks_.begin(), decks_.end(), [](const auto& d) { return d->dead; }), decks_.end());
    if (advance_after) advance_(+1, 0.0f);
}

void MusicPlayer::stop_now() {
    for (const auto& d : decks_) engine_.stop(d->voice);
    decks_.clear();
    current_ = nullptr;
    stack_.clear();
    clear_playlist();
    paused_ = false;
}

} // namespace toy::audio
