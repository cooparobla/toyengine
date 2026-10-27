/**
 * @file audio_components.h
 * @brief Game-side audio components that need the engine's AudioSystem (not just sfxcoopa).
 *
 *
 *   MusicPlaylist   hands the scene's background music to the AudioSystem's MusicPlayer: a
 *                   list of tracks that plays through with crossfades. A track already playing
 *                   (carried over from the previous scene) keeps playing.
 *   MusicZone       a box (or sphere) region: while the listener is inside, its track is pushed
 *                   over the current music, which pauses and resumes when the listener leaves.
 *   MusicStatusText writes "track  0:12 / 0:31" (and the transition state) into a Text here.
 *
 * @code
 * - type: MusicPlaylist
 *   crossfade: 3.0
 *   tracks:
 *     - {name: meadow, clip: audio/music/meadow.wav, volume: 0.8}
 *     - audio/music/expedition.wav
 * - type: MusicZone
 *   track: nocturne
 *   size: {x: 8, y: 8, z: 6}
 *   fade: 2.0
 * @endcode
 *
 * @code
 * - type: SettingRow
 *   kind: slider
 *   label: Music
 *   min: 0
 *   max: 1
 * @endcode
 */

#ifndef TOYENGINE_AUDIO_AUDIO_COMPONENTS_H
#define TOYENGINE_AUDIO_AUDIO_COMPONENTS_H

#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>
#include <toyengine/audio/audio_system.h>
#include <toyengine/audio/music_player.h>



#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace toy::audio {

/** @brief The UserSettings key a bus's volume is stored under ("audio.music"). */
std::string volume_setting_key(const std::string& bus);

/**
 * @brief Sets a bus's volume AND remembers it for the player.
 *
 * What a volume control is actually for: the mixer change is this session, the UserSettings
 * write is every session after it. A UI that only called AudioSystem::set_bus_volume() would
 * forget the setting the moment the game closed.
 *
 * @return False if there is no audio system or no such bus.
 */
bool set_bus_volume_persisted(const std::string& bus, float linear);

/**
 * @brief A bus's remembered volume, or its current one when the player has never set it.
 *
 * The value a volume slider should start at.
 */
float bus_volume_persisted(const std::string& bus);

/** @brief Scene background music: plays its playlist through AudioSystem::music() once the scene simulates. */
class MusicPlaylistComponent : public coopa::scene::Component {
public:
    std::vector<MusicTrack> tracks;
    bool shuffle = false;
    bool repeat = true;
    float crossfade = 3.0f;
    int start = -1;               ///< First track index (-1: the first, or random when shuffled).
    bool play_on_start = true;   ///< False: only registers the tracks by name (play() starts it)
    bool stop_on_destroy = false; ///< Fade the music out when this object goes (else it plays on).

    std::string type_name() const override { return "MusicPlaylist"; }

    ~MusicPlaylistComponent() override;

    void update(float) override;

    /** @brief Registers the tracks (by name, for MusicZone) and decodes them. */
    void register_tracks();

    /** @brief Registers the tracks and starts the playlist now. */
    void play();

private:
    bool registered_ = false;
    bool started_ = false;
};

/** @brief While the listener is inside this region, `track` plays over the scene's music. */
class MusicZone : public coopa::scene::Component {
public:
    std::string track;                     ///< Track name or clip path.
    glm::vec3 size{10.0f, 10.0f, 6.0f};    ///< Full box extents, centred on the object (world axes, times its scale).
    float radius = 0.0f;                   ///< > 0: a sphere instead of the box.
    float fade = 2.0f;                     ///< Crossfade in and out, seconds.

    std::string type_name() const override { return "MusicZone"; }

    ~MusicZone() override;

    void update(float) override;

    /** @brief True while the listener is inside (and the track is pushed). */
    bool inside() const { return inside_; }
    /** @brief Whether `p` is inside the zone. */
    bool contains(const glm::vec3& p) const;

private:
    bool inside_ = false;
};

/** @brief Drives the music when a signal fires. */

/** @brief Plays / stops an AudioSource on `target` (or self) when a signal fires. */

/** @brief Shows the music state in the Text on this object. */
struct MusicStatusText {
    /** @brief The status line for `music` ("meadow  0:12 / 0:31  [crossfading]"). */
    static std::string describe(const MusicPlayer& music);
};

/** @brief Registers the music components' scene parsers. */
void register_audio_components();

} // namespace toy::audio

#endif // TOYENGINE_AUDIO_AUDIO_COMPONENTS_H
