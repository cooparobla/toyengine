/**
 * @file music_player.h
 * @brief Background music: named tracks, crossfades, playlists and a push/pop stack.
 *
 * One MusicPlayer lives in the AudioSystem (`AudioSystem::music()`) and plays on the Music bus.
 * Each playing track is a "deck" with its own fade envelope, so any number of tracks can be
 * fading at once (switching A -> B -> C quickly fades A and B out together while C comes in).
 * Fades are equal-power (sin/cos shaped), so a crossfade holds its loudness through the middle.
 *
 *   play(track, fade)  crossfades from whatever is playing to `track` (a registered name or a
 *                      clip path). Playing the track that is already current does nothing.
 *   stop(fade)         fades the current track out.
 *   pause / resume     fade out and freeze / continue where it stopped.
 *   set_playlist(p)    plays p's tracks in order (or shuffled). Each plays once; when one
 *                      reaches its last `crossfade` seconds the next starts and they cross --
 *                      the "natural" transition. `repeat` wraps the list.
 *   next / previous    skip within the playlist, crossfading.
 *   push(track) / pop  a temporary track over the current one (a music zone, a boss fight):
 *                      push fades the current deck out and PAUSES it; pop fades the pushed
 *                      track out and resumes the paused one from where it stopped.
 *
 * Music persists across scene loads: a new scene's MusicPlaylist that names the same track
 * keeps it playing rather than restarting it. AudioSystem::stop_all() stops it.
 *
 * Main thread only. Track lengths come from the decoded clip; playback time is integrated from
 * update(dt) (frozen while the engine or the deck is paused), and a voice that ends early is
 * caught from the engine's own voice state.
 *
 * @code
 * auto& music = engine.audio().music();
 * music.add_track({"calm", "audio/music/meadow.wav", 0.8f});
 * music.play("calm");                  // default crossfade
 * music.play("audio/music/boss.wav", 0.5f);
 * music.push("shop", 1.5f);  ...  music.pop(1.5f);
 * @endcode
 */

#ifndef TOYENGINE_AUDIO_MUSIC_PLAYER_H
#define TOYENGINE_AUDIO_MUSIC_PLAYER_H

#include <sfxcoopa/core/engine.h>

#include <coopa/event/signal.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace toy::audio {

/** @brief A named piece of music. */
struct MusicTrack {
    std::string name;          ///< What play()/playlists call it; empty = the clip path.
    std::string clip;          ///< Clip path (resolved against the asset roots).
    float volume = 1.0f;       ///< Linear gain on top of the Music bus.
    bool loop = true;          ///< Loops when played on its own; playlist tracks always play once.
};

/** @brief An ordered set of tracks that plays through, crossfading track to track. */
struct MusicPlaylist {
    std::vector<std::string> tracks;   ///< Track names or clip paths.
    bool shuffle = false;              ///< Random order (never the same track twice in a row).
    bool repeat = true;                ///< Wrap past the last track (else the music ends).
    float crossfade = 3.0f;            ///< Seconds the outgoing and incoming tracks overlap.
};

class MusicPlayer {
public:
    /** @brief Plays through `engine` on the bus named `bus` (Master when it doesn't exist). */
    MusicPlayer(coopa::sfx::core::AudioEngine& engine, std::string bus);
    ~MusicPlayer();

    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    /** @brief Fade used when a call passes a negative fade. */
    float default_fade = 2.0f;

    /** @brief Resolves a relative clip path to a file (AudioSystem sets the asset-root resolver). */
    std::function<std::string(const std::string&)> resolve;

    // --- tracks ---------------------------------------------------------------------------
    /** @brief Registers (or replaces) a named track. */
    void add_track(const MusicTrack& track);
    const MusicTrack* find_track(const std::string& name) const;
    const std::vector<MusicTrack>& tracks() const { return tracks_; }

    /** @brief Decodes a track's clip now, so starting it later doesn't hitch. False if it can't load. */
    bool preload(const std::string& track);

    // --- direct control -------------------------------------------------------------------
    /**
     * @brief Crossfades to `track` (a name or clip path) over `fade` seconds; leaves playlist
     *        mode. A no-op when `track` is already the current track (unless `restart`).
     * @return False if the clip can't load (the current music keeps playing).
     */
    bool play(const std::string& track, float fade = -1.0f, bool restart = false);

    /** @brief Fades the current track out (and any suspended ones are dropped). */
    void stop(float fade = -1.0f);

    /** @brief Fades out and freezes the current track; resume() continues it. */
    void pause(float fade = 0.5f);
    void resume(float fade = 0.5f);
    bool paused() const { return paused_; }

    // --- playlist -------------------------------------------------------------------------
    /**
     * @brief Plays `playlist` from `start` (an index into its tracks; -1 = first, or a random
     *        one when shuffled). If the current track is in the list it keeps playing and the
     *        playlist continues from it.
     */
    void set_playlist(const MusicPlaylist& playlist, int start = -1);
    void clear_playlist() { playlist_ = {}; order_.clear(); }
    bool has_playlist() const { return !order_.empty(); }
    const MusicPlaylist& playlist() const { return playlist_; }

    /** @brief Crossfades to the next / previous playlist track (`fade` < 0: the playlist's crossfade). */
    bool next(float fade = -1.0f);
    bool previous(float fade = -1.0f);

    // --- stack ----------------------------------------------------------------------------
    /** @brief Plays `track` over the current music, which fades out and pauses until pop(). */
    bool push(const std::string& track, float fade = -1.0f);
    /** @brief Fades the pushed track out and resumes the one under it. False if nothing was pushed. */
    bool pop(float fade = -1.0f);
    size_t stack_depth() const { return stack_.size(); }

    // --- state ----------------------------------------------------------------------------
    /** @brief The track playing (or fading in) now; empty when silent. */
    std::string current() const;
    /** @brief Seconds into the current track (wraps when it loops). */
    float position() const;
    /** @brief Length of the current track's clip in seconds (0 when silent). */
    float duration() const;
    /** @brief True while some track is audible or fading. */
    bool playing() const;
    /** @brief True while any deck is fading in or out. */
    bool transitioning() const;
    /** @brief Number of live decks (playing, fading or paused under a push). */
    size_t deck_count() const { return decks_.size(); }

    /** @brief Fired from play()/next()/a natural transition with the track that starts. */
    coopa::event::Signal<std::string> on_track_started;

    /** @brief Per frame (AudioSystem::update): fades, natural transitions, finished voices. */
    void update(float dt);

    /** @brief Stops every deck at once (no fade); keeps tracks and playlist settings. */
    void stop_now();

private:
    struct Deck;

    const MusicTrack& track_for_(const std::string& key);
    Deck* start_deck_(const std::string& key, float fade, bool loop);
    void fade_deck_(Deck& d, float target, float seconds, int then);
    bool advance_(int step, float fade);
    void rebuild_order_(int keep_first);
    int playlist_slot_of_(const std::string& key) const;
    float playlist_fade_(float fade) const;
    float fade_or_default_(float fade) const { return fade < 0.0f ? default_fade : fade; }

    coopa::sfx::core::AudioEngine& engine_;
    std::string bus_;
    std::vector<MusicTrack> tracks_;
    std::vector<std::unique_ptr<Deck>> decks_;
    Deck* current_ = nullptr;             ///< The deck play()/next() replace; null when silent.
    std::vector<Deck*> stack_;            ///< Decks paused under a push(), innermost last.
    MusicPlaylist playlist_;
    std::vector<int> order_;              ///< Playlist play order (indices into playlist_.tracks).
    int cursor_ = -1;                     ///< Position in order_ of the current playlist track.
    bool paused_ = false;
    std::mt19937 rng_{0x5eedu};
};

} // namespace toy::audio

#endif // TOYENGINE_AUDIO_MUSIC_PLAYER_H
