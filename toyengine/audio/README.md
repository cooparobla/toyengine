# toyengine/audio

The engine's audio layer: one sfxcoopa `AudioEngine` with a fixed bus tree, background music,
and the scene components that need the engine (plain `AudioSource` / `AudioListener` come from
sfxcoopa itself and are registered here against this engine).

```
Master
 ├─ Music   MusicPlayer: tracks, crossfades, playlists, push/pop
 ├─ SFX     AudioSource's default bus, play_oneshot()
 └─ UI      uicoopa's UiAudio: button clicks and other named UI sounds
```

| File | Purpose |
|---|---|
| [`audio_system.h`](audio_system.h) | `AudioSystem` — owns the engine, the bus tree and the sound card. Created by `Engine::init_audio_()` and registered with sfxcoopa's scene bridge and with `UiAudio`, so scene-YAML audio components and UI sounds share one device. `play_oneshot()` / `play_oneshot_3d()` fire-and-forget; `bus()` / `set_bus_volume()` reach the mixer; `pause_all()` / `resume_all()` gate everything for a pause menu. `render()` mixes through a peak limiter (ceiling −1 dBFS), so stacked sounds or volumes above 1 never hard-clip at the card. **Never fatal**: with no sound card it runs a null device and the game is unchanged but silent — `has_output()` reports which. `active()` / `set_active()` expose the one live system to components that cannot reach the Engine. |
| [`music_player.h`](music_player.h) | `MusicPlayer` — background music on the Music bus, reached as `AudioSystem::music()`. Each playing track is a *deck* with its own equal-power (sin/cos) fade envelope, so any number can fade at once and a crossfade holds loudness through the middle. `play()` crossfades (playing the current track is a no-op unless `restart`), `stop()` / `pause()` / `resume()` fade, `set_playlist()` walks a `MusicPlaylist` in order or shuffled with `repeat`. Tracks are registered names or clip paths; `resolve` maps a name to a file against the asset roots. |
| [`audio_components.h`](audio_components.h) | The components that need `AudioSystem` rather than just sfxcoopa. `VolumeBinding` ties a UI `Slider` in its own subtree to a mixer bus and remembers the value in `UserSettings` (`audio.<bus>`), so it is restored next launch. `MusicPlaylistComponent` hands the scene's music to the `MusicPlayer` — a track carried over from the previous scene keeps playing. `MusicZone` is a box or sphere region that pushes its track over the current music while the listener is inside and pops it on exit. `MusicOnSignal` / `AudioSourceOnSignal` are `SignalReactor`s that play on a named UI signal; `MusicStatusText` writes the current track into a UI label. |

## Conventions

- **The listener follows an `AudioListener` component, else the main camera.** `Engine::update_audio_()`
  does this every frame; a scene needs no listener object to have positional audio work.
- **Audio never fails a run.** Every entry point tolerates a missing device, a missing clip and a
  missing bus. A headless or CI run opens the null device (`AudioSystemOptions`), and tests
  therefore exercise the same code paths as a desktop run.
- **Bus volumes are the player's, not the scene's.** They live in `UserSettings`, outside the
  project's assets, and are applied at startup — a scene file never sets them.
