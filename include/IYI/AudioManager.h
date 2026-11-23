// Declares the audio subsystem manager for IYIEngine.
// Manages sound effect instances via raylib sound aliases, positional 2D audio attenuation,
// volume buses, music streaming with crossfades, and persistent bus settings.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include "raylib.h"

namespace IYI {

class AssetManager;
struct ProjectConfig;

struct SoundInstance
{
    int Handle = 0;
    Sound Alias = { 0 };
    std::string Path;        // asset path, for the per-asset voice cap
    std::string Bus = "sfx";
    float Volume = 1.0f;
    float Pitch = 1.0f;
    float Pan = 0.5f;
    bool Loop = false;
    bool Paused = false;

    bool IsPositional = false;
    float PosX = 0.0f;
    float PosY = 0.0f;
};

class AudioManager
{
public:
    static AudioManager& Get();

    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    void Initialize(AssetManager* assets, const ProjectConfig& config);
    void Shutdown();
    void Update(float dt);

    // Sound effects
    // `handle` comes from the game side (engine.audio.play returns it before the call runs here).
    void Play(int handle, const std::string& path, float volume = 1.0f, float pitch = 1.0f, float pan = 0.5f, bool loop = false, const std::string& bus = "sfx");
    void PlayAt(int handle, const std::string& path, float x, float y, float volume = 1.0f, float pitch = 1.0f, bool loop = false, const std::string& bus = "sfx");
    // For the game side's frame input: the handles playing now.
    std::vector<int> PlayingHandles() const;
    const std::unordered_map<std::string, float>& BusVolumes() const { return m_busVolumes; }
    void Stop(int handle);
    void Pause(int handle);
    void Resume(int handle);
    bool IsPlaying(int handle) const;
    void SetVolume(int handle, float volume);
    void SetPitch(int handle, float pitch);
    void SetPan(int handle, float pan);
    void SetPosition(int handle, float x, float y);
    void StopAll(const std::string& bus = "");

    // Spatial listener & falloff
    void SetListener(float x, float y);
    void SetFalloff(float nearDist, float farDist);

    // Music
    void PlayMusic(const std::string& path, float fade = 0.0f, bool loop = true, float volume = 1.0f);
    void StopMusic(float fade = 0.0f);
    void PauseMusic();
    void ResumeMusic();
    void SetMusicPitch(float pitch);
    std::string CurrentMusic() const;
    void Queue(const std::string& path);
    void ClearQueue();

    // Buses
    void SetBusVolume(const std::string& name, float volume);
    float GetBusVolume(const std::string& name) const;
    void SetBusGain(const std::string& name, float gain);

private:
    AudioManager() = default;
    ~AudioManager() = default;

    void ApplyEffectiveVolume(SoundInstance& inst);
    void ApplyEffectivePan(SoundInstance& inst);
    float CalculateEffectiveVolume(const std::string& bus, float instanceVolume) const;
    void UpdateAllSoundVolumes();
    void UpdateAllMusicVolumes();
    void FlushAudioConfig();
    // Evicts the oldest non-looping voice for the given asset path when the
    // per-asset cap is reached. Prevents mixer sum-compounding on rapid retriggering.
    void EvictOldestVoice(const std::string& path);

    static constexpr int k_maxVoicesPerAsset = 4;

    AssetManager* m_assets = nullptr;
    std::string m_userDataPath;

    // Bus volumes & persistence
    std::unordered_map<std::string, float> m_busVolumes;
    std::unordered_map<std::string, float> m_busGains;   // session only, never saved
    bool m_dirty = false;
    float m_saveTimer = 0.0f;

    // Active sound instances
    std::unordered_map<int, SoundInstance> m_sounds;

    // Spatial parameters
    float m_listenerX = 0.0f;
    float m_listenerY = 0.0f;
    float m_falloffNear = 100.0f;
    float m_falloffFar = 1000.0f;

    // Music playback state
    enum class MusicFadeState { None, In, Out };

    std::string m_currentMusicPath;
    Music* m_currentMusic = nullptr;
    float m_currentMusicVolume = 1.0f;
    bool m_currentMusicLoop = true;
    float m_musicPitch = 1.0f;  // speed and pitch of every stream (tape slowdown)
    bool m_musicPaused = false;
    MusicFadeState m_musicFadeState = MusicFadeState::None;
    float m_musicFadeDuration = 0.0f;
    float m_musicFadeTimer = 0.0f;
    float m_musicFadeFactor = 1.0f;
    float m_lastFade = 0.0f;

    // Outgoing music crossfade state
    Music* m_outgoingMusic = nullptr;
    float m_outgoingMusicVolume = 1.0f;
    float m_outgoingFadeDuration = 0.0f;
    float m_outgoingFadeTimer = 0.0f;
    float m_outgoingFadeFactor = 1.0f;

    // Music queue
    std::vector<std::string> m_musicQueue;
};

namespace Api {
// The game side's audio calls (engine.audio.* in Python). Threads: each call is recorded into the frame packet's
// Audio pass and run on the audio side in order; handles are given at once; queries answer from the frame input's
// copy of the audio state, kept current within the step for what this step itself changed.
int AudioPlay(const std::string& path, float volume = 1.0f, float pitch = 1.0f, float pan = 0.5f, bool loop = false, const std::string& bus = "sfx");   // handle; 0: no such file
int AudioPlayAt(const std::string& path, float x, float y, float volume = 1.0f, float pitch = 1.0f, bool loop = false, const std::string& bus = "sfx");
void AudioStop(int handle);
void AudioPause(int handle);
void AudioResume(int handle);
bool AudioIsPlaying(int handle);
void AudioSetVolume(int handle, float volume);
void AudioSetPitch(int handle, float pitch);
void AudioSetPan(int handle, float pan);                     // 0 left, 0.5 centre, 1 right
void AudioSetPosition(int handle, float x, float y);
void AudioStopAll(const std::string& bus = "");              // "": every bus
void AudioSetListener(float x, float y);
void AudioSetFalloff(float nearDist, float farDist);
void AudioMusic(const std::string& path, float fade = 0.0f, bool loop = true, float volume = 1.0f);
void AudioStopMusic(float fade = 0.0f);
void AudioPauseMusic();
void AudioResumeMusic();
void AudioSetMusicPitch(float pitch);
std::string AudioCurrentMusic();                             // "": none
void AudioQueue(const std::string& path);                    // plays when the current non-looping track ends
void AudioClearQueue();
void AudioSetBusVolume(const std::string& name, float volume);
float AudioGetBusVolume(const std::string& name);
void AudioSetBusGain(const std::string& name, float gain);   // session-only multiplier, never saved
} // namespace Api

} // namespace IYI
