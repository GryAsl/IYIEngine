// Implements the audio subsystem manager for IYIEngine.
// Handles sound aliases, positional audio attenuation, audio buses,
// crossfading music streams, and JSON settings persistence.

#include "IYI/AudioManager.h"
#include "IYI/Application.h"
#include "IYI/AssetManager.h"
#include "IYI/FrameInput.h"
#include "IYI/FramePacket.h"
#include "IYI/ProjectConfig.h"
#include "IYI/Log.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <climits>
#include <cmath>
#include <algorithm>

namespace fs = std::filesystem;

namespace IYI {

static float s_outgoingFadeStart = 1.0f;
static float s_musicFadeOutStart = 1.0f;

AudioManager& AudioManager::Get()
{
    static AudioManager instance;
    return instance;
}

void AudioManager::Initialize(AssetManager* assets, const ProjectConfig& config)
{
    m_assets = assets;
    m_userDataPath = config.UserDataRoot();
    m_busVolumes.clear();
    m_dirty = false;
    m_saveTimer = 0.0f;

    fs::path jsonPath = fs::path(m_userDataPath) / "audio.json";
    bool loadedFromFile = false;

    if (fs::exists(jsonPath)) {
        try {
            std::ifstream file(jsonPath);
            if (file.is_open()) {
                nlohmann::json j;
                file >> j;
                if (j.is_object()) {
                    for (auto& [k, v] : j.items()) {
                        if (v.is_number()) {
                            float val = v.get<float>();
                            if (std::isfinite(val)) {
                                m_busVolumes[k] = std::clamp(val, 0.0f, 1.0f);
                            }
                        }
                    }
                    loadedFromFile = true;
                }
            }
        } catch (const std::exception& e) {
            LogWrite(LogLevel::Warn, "audio", std::string("Failed to load audio.json: ") + e.what());
        } catch (...) {
            LogWrite(LogLevel::Warn, "audio", "Unknown error loading audio.json");
        }
    }

    if (!loadedFromFile) {
        for (const auto& [name, vol] : config.AudioBusDefaults) {
            if (std::isfinite(vol)) {
                m_busVolumes[name] = std::clamp(vol, 0.0f, 1.0f);
            }
        }
    }

    for (const char* bus : { "master", "music", "sfx" }) m_busVolumes.try_emplace(bus, 1.0f);
}

void AudioManager::Shutdown()
{
    StopAll("");

    if (m_outgoingMusic) {
        StopMusicStream(*m_outgoingMusic);
        m_outgoingMusic = nullptr;
    }
    if (m_currentMusic) {
        StopMusicStream(*m_currentMusic);
        m_currentMusic = nullptr;
        m_currentMusicPath.clear();
    }
    m_musicQueue.clear();

    if (m_dirty) {
        FlushAudioConfig();
        m_dirty = false;
    }

    m_assets = nullptr;
}

void AudioManager::FlushAudioConfig()
{
    if (m_userDataPath.empty()) return;

    try {
        std::error_code ec;
        fs::create_directories(m_userDataPath, ec);
        if (ec) {
            LogWrite(LogLevel::Warn, "audio", "Failed to create directory for audio.json: " + ec.message());
            return;
        }
        fs::path jsonPath = fs::path(m_userDataPath) / "audio.json";
        nlohmann::json j;
        for (const auto& [k, v] : m_busVolumes) {
            if (std::isfinite(v)) {
                j[k] = std::clamp(v, 0.0f, 1.0f);
            }
        }
        std::ofstream file(jsonPath);
        if (file.is_open()) {
            file << j.dump(4);
        }
    } catch (const std::exception& e) {
        LogWrite(LogLevel::Warn, "audio", std::string("Failed to save audio.json: ") + e.what());
    } catch (...) {
        LogWrite(LogLevel::Warn, "audio", "Unknown error saving audio.json");
    }
}

float AudioManager::CalculateEffectiveVolume(const std::string& bus, float instanceVolume) const
{
    const auto gain = [this](const std::string& name) {
        const auto it = m_busGains.find(name);
        return it != m_busGains.end() ? it->second : 1.0f;
    };
    float masterVol = GetBusVolume("master") * gain("master");
    float busVol = (bus == "master") ? 1.0f : GetBusVolume(bus) * gain(bus);
    return masterVol * busVol * instanceVolume;
}

void AudioManager::ApplyEffectiveVolume(SoundInstance& inst)
{
    float baseEff = CalculateEffectiveVolume(inst.Bus, inst.Volume);
    if (inst.IsPositional) {
        float dx = inst.PosX - m_listenerX;
        float dy = inst.PosY - m_listenerY;
        float dist = std::hypot(dx, dy);
        float atten = 1.0f;
        if (dist <= m_falloffNear) {
            atten = 1.0f;
        } else if (dist >= m_falloffFar) {
            atten = 0.0f;
        } else if (m_falloffFar > m_falloffNear) {
            atten = 1.0f - (dist - m_falloffNear) / (m_falloffFar - m_falloffNear);
        }
        atten = std::clamp(atten, 0.0f, 1.0f);
        baseEff *= atten;
    }
    SetSoundVolume(inst.Alias, std::max(0.0f, baseEff));
}

void AudioManager::ApplyEffectivePan(SoundInstance& inst)
{
    float pan = inst.Pan;   // engine pan: 0 left, 0.5 center, 1 right (SPEC §20)
    if (inst.IsPositional) {
        float dx = inst.PosX - m_listenerX;
        float panDist = std::max(m_falloffFar, 1.0f);
        pan = 0.5f + 0.5f * (dx / panDist);
    }
    pan = std::clamp(pan, 0.0f, 1.0f);
#if RAYLIB_VERSION_MAJOR >= 6
    pan = pan * 2.0f - 1.0f;   // raylib 6.0 changed SetSoundPan to -1 left .. 1 right; games keep 0..1 (SPEC §0.1)
#endif
    SetSoundPan(inst.Alias, pan);
}

void AudioManager::UpdateAllSoundVolumes()
{
    for (auto& [h, inst] : m_sounds) {
        ApplyEffectiveVolume(inst);
    }
}

void AudioManager::UpdateAllMusicVolumes()
{
    if (m_currentMusic) {
        float eff = CalculateEffectiveVolume("music", m_currentMusicVolume) * m_musicFadeFactor;
        SetMusicVolume(*m_currentMusic, std::max(0.0f, eff));
    }
    if (m_outgoingMusic) {
        float eff = CalculateEffectiveVolume("music", m_outgoingMusicVolume) * m_outgoingFadeFactor;
        SetMusicVolume(*m_outgoingMusic, std::max(0.0f, eff));
    }
}

// Prevents unbounded mixer pile-up: when the same non-looping sound is retriggered
// rapidly (e.g. hovering across UI buttons), each call to Play() allocates a new alias
// and all of them run simultaneously in the hardware mixer, summing their volumes. At
// the cap we evict the oldest alias (lowest handle, since handles are monotonic) so the
// running count never exceeds k_maxVoicesPerAsset for any one asset.
// Looping voices are excluded: a looping ambient legitimately needs to live until
// explicitly stopped.
void AudioManager::EvictOldestVoice(const std::string& path)
{
    int count = 0;
    int oldestHandle = -1;
    for (const auto& [h, inst] : m_sounds) {
        if (inst.Path == path && !inst.Loop) {
            ++count;
            if (oldestHandle < 0 || h < oldestHandle) {
                oldestHandle = h;
            }
        }
    }
    if (count >= k_maxVoicesPerAsset && oldestHandle >= 0) {
        auto it = m_sounds.find(oldestHandle);
        if (it != m_sounds.end()) {
            StopSound(it->second.Alias);
            UnloadSoundAlias(it->second.Alias);
            m_sounds.erase(it);
        }
    }
}

void AudioManager::Play(int handle, const std::string& path, float volume, float pitch, float pan, bool loop, const std::string& bus)
{
    if (!m_assets) return;
    Sound* base = m_assets->GetSound(path);
    if (!base || !IsSoundValid(*base)) return;

    // Evict the oldest alias for this asset before allocating a new one so the
    // hardware mixer never accumulates more than k_maxVoicesPerAsset simultaneous
    // copies of the same sample. Without this, rapid hover events pile up
    // unboundedly and the perceived volume compounds.
    if (!loop) EvictOldestVoice(path);

    Sound alias = LoadSoundAlias(*base);
    if (!IsSoundValid(alias)) return;

    SoundInstance inst;
    inst.Handle = handle;
    inst.Alias = alias;
    inst.Path = path;
    inst.Bus = bus;
    inst.Volume = volume;
    inst.Pitch = pitch;
    inst.Pan = pan;
    inst.Loop = loop;
    inst.Paused = false;
    inst.IsPositional = false;

    SetSoundPitch(alias, std::max(0.001f, pitch));
    ApplyEffectivePan(inst);
    ApplyEffectiveVolume(inst);
    PlaySound(alias);

    auto old = m_sounds.find(handle);
    if (old != m_sounds.end()) UnloadSoundAlias(old->second.Alias);
    m_sounds[handle] = inst;
}

void AudioManager::PlayAt(int handle, const std::string& path, float x, float y, float volume, float pitch, bool loop, const std::string& bus)
{
    if (!m_assets) return;
    Sound* base = m_assets->GetSound(path);
    if (!base || !IsSoundValid(*base)) return;

    if (!loop) EvictOldestVoice(path);

    Sound alias = LoadSoundAlias(*base);
    if (!IsSoundValid(alias)) return;

    SoundInstance inst;
    inst.Handle = handle;
    inst.Alias = alias;
    inst.Path = path;
    inst.Bus = bus;
    inst.Volume = volume;
    inst.Pitch = pitch;
    inst.Pan = 0.5f;
    inst.Loop = loop;
    inst.Paused = false;
    inst.IsPositional = true;
    inst.PosX = x;
    inst.PosY = y;

    SetSoundPitch(alias, std::max(0.001f, pitch));
    ApplyEffectivePan(inst);
    ApplyEffectiveVolume(inst);
    PlaySound(alias);

    auto old = m_sounds.find(handle);
    if (old != m_sounds.end()) UnloadSoundAlias(old->second.Alias);
    m_sounds[handle] = inst;
}

void AudioManager::Stop(int handle)
{
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end()) {
        StopSound(it->second.Alias);
        UnloadSoundAlias(it->second.Alias);
        m_sounds.erase(it);
    }
}

void AudioManager::Pause(int handle)
{
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end() && !it->second.Paused) {
        PauseSound(it->second.Alias);
        it->second.Paused = true;
    }
}

void AudioManager::Resume(int handle)
{
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end() && it->second.Paused) {
        ResumeSound(it->second.Alias);
        it->second.Paused = false;
    }
}

std::vector<int> AudioManager::PlayingHandles() const
{
    std::vector<int> handles;
    for (const auto& [handle, inst] : m_sounds) {
        if (!inst.Paused && IsSoundPlaying(inst.Alias)) handles.push_back(handle);
    }
    return handles;
}

bool AudioManager::IsPlaying(int handle) const
{
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end()) {
        return !it->second.Paused && IsSoundPlaying(it->second.Alias);
    }
    return false;
}

void AudioManager::SetVolume(int handle, float volume)
{
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end()) {
        it->second.Volume = volume;
        ApplyEffectiveVolume(it->second);
    }
}

void AudioManager::SetPitch(int handle, float pitch)
{
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end()) {
        it->second.Pitch = pitch;
        SetSoundPitch(it->second.Alias, std::max(0.001f, pitch));
    }
}

void AudioManager::SetPan(int handle, float pan)
{
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end()) {
        it->second.Pan = pan;
        it->second.IsPositional = false;
        ApplyEffectivePan(it->second);
    }
}

void AudioManager::SetPosition(int handle, float x, float y)
{
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end()) {
        it->second.PosX = x;
        it->second.PosY = y;
        it->second.IsPositional = true;
        ApplyEffectivePan(it->second);
        ApplyEffectiveVolume(it->second);
    }
}

void AudioManager::StopAll(const std::string& bus)
{
    if (bus.empty()) {
        for (auto& [h, inst] : m_sounds) {
            StopSound(inst.Alias);
            UnloadSoundAlias(inst.Alias);
        }
        m_sounds.clear();
    } else {
        std::vector<int> toRemove;
        for (auto& [h, inst] : m_sounds) {
            if (inst.Bus == bus) {
                StopSound(inst.Alias);
                UnloadSoundAlias(inst.Alias);
                toRemove.push_back(h);
            }
        }
        for (int h : toRemove) {
            m_sounds.erase(h);
        }
    }
}

void AudioManager::SetListener(float x, float y)
{
    m_listenerX = x;
    m_listenerY = y;
    for (auto& [h, inst] : m_sounds) {
        if (inst.IsPositional) {
            ApplyEffectivePan(inst);
            ApplyEffectiveVolume(inst);
        }
    }
}

void AudioManager::SetFalloff(float nearDist, float farDist)
{
    m_falloffNear = std::max(0.0f, nearDist);
    m_falloffFar = std::max(m_falloffNear + 0.001f, farDist);
    for (auto& [h, inst] : m_sounds) {
        if (inst.IsPositional) {
            ApplyEffectivePan(inst);
            ApplyEffectiveVolume(inst);
        }
    }
}

void AudioManager::PlayMusic(const std::string& path, float fade, bool loop, float volume)
{
    if (path.empty()) {
        StopMusic(fade);
        return;
    }

    if (!m_assets) return;
    Music* next = m_assets->GetMusic(path);
    if (!next || !IsMusicValid(*next)) return;

    m_lastFade = fade;

    if (m_currentMusic == next && m_currentMusicPath == path) {
        m_currentMusicLoop = loop;
        m_currentMusic->looping = loop;
        m_currentMusicVolume = volume;
        if (m_musicFadeState == MusicFadeState::Out) {
            m_musicFadeState = (fade > 0.0f) ? MusicFadeState::In : MusicFadeState::None;
            m_musicFadeDuration = fade;
            m_musicFadeTimer = (fade > 0.0f) ? (m_musicFadeFactor * fade) : 0.0f;
        } else if (fade <= 0.0f) {
            m_musicFadeState = MusicFadeState::None;
            m_musicFadeFactor = 1.0f;
            SetMusicVolume(*m_currentMusic, std::max(0.0f, CalculateEffectiveVolume("music", volume)));
        }
        if (!IsMusicStreamPlaying(*m_currentMusic) && !m_musicPaused) {
            PlayMusicStream(*m_currentMusic);
        }
        return;
    }

    if (m_currentMusic) {
        if (m_outgoingMusic && m_outgoingMusic != m_currentMusic) {
            StopMusicStream(*m_outgoingMusic);
        }
        if (fade > 0.0f) {
            m_outgoingMusic = m_currentMusic;
            m_outgoingMusicVolume = m_currentMusicVolume;
            m_outgoingFadeDuration = fade;
            m_outgoingFadeTimer = 0.0f;
            m_outgoingFadeFactor = m_musicFadeFactor;
            s_outgoingFadeStart = m_outgoingFadeFactor;
        } else {
            StopMusicStream(*m_currentMusic);
            m_outgoingMusic = nullptr;
        }
    }

    m_currentMusic = next;
    m_currentMusicPath = path;
    m_currentMusicVolume = volume;
    m_currentMusicLoop = loop;
    m_currentMusic->looping = loop;
    ::SetMusicPitch(*m_currentMusic, m_musicPitch);
    m_musicPaused = false;

    if (fade > 0.0f) {
        m_musicFadeState = MusicFadeState::In;
        m_musicFadeDuration = fade;
        m_musicFadeTimer = 0.0f;
        m_musicFadeFactor = 0.0f;
        SetMusicVolume(*m_currentMusic, 0.0f);
    } else {
        m_musicFadeState = MusicFadeState::None;
        m_musicFadeDuration = 0.0f;
        m_musicFadeTimer = 0.0f;
        m_musicFadeFactor = 1.0f;
        SetMusicVolume(*m_currentMusic, std::max(0.0f, CalculateEffectiveVolume("music", volume)));
    }

    PlayMusicStream(*m_currentMusic);
}

void AudioManager::StopMusic(float fade)
{
    m_lastFade = (fade > 0.0f) ? fade : m_lastFade;

    if (m_outgoingMusic) {
        StopMusicStream(*m_outgoingMusic);
        m_outgoingMusic = nullptr;
    }

    if (!m_currentMusic) return;

    if (fade > 0.0f) {
        s_musicFadeOutStart = m_musicFadeFactor;
        m_musicFadeState = MusicFadeState::Out;
        m_musicFadeDuration = fade;
        m_musicFadeTimer = 0.0f;
    } else {
        StopMusicStream(*m_currentMusic);
        m_currentMusic = nullptr;
        m_currentMusicPath.clear();
        m_musicFadeState = MusicFadeState::None;
        m_musicFadeFactor = 0.0f;
    }
}

void AudioManager::PauseMusic()
{
    if (m_currentMusic && !m_musicPaused) {
        PauseMusicStream(*m_currentMusic);
    }
    if (m_outgoingMusic) {
        PauseMusicStream(*m_outgoingMusic);
    }
    m_musicPaused = true;
}

void AudioManager::SetMusicPitch(float pitch)
{
    m_musicPitch = std::max(0.001f, pitch);
    if (m_currentMusic) ::SetMusicPitch(*m_currentMusic, m_musicPitch);
    if (m_outgoingMusic) ::SetMusicPitch(*m_outgoingMusic, m_musicPitch);
}

void AudioManager::ResumeMusic()
{
    if (m_musicPaused) {
        if (m_currentMusic) {
            ResumeMusicStream(*m_currentMusic);
        }
        if (m_outgoingMusic) {
            ResumeMusicStream(*m_outgoingMusic);
        }
        m_musicPaused = false;
    }
}

std::string AudioManager::CurrentMusic() const
{
    return m_currentMusicPath;
}

void AudioManager::Queue(const std::string& path)
{
    if (path.empty()) return;
    if (!m_currentMusic && m_musicQueue.empty()) {
        PlayMusic(path, m_lastFade, false, 1.0f);
    } else {
        m_musicQueue.push_back(path);
    }
}

void AudioManager::ClearQueue()
{
    m_musicQueue.clear();
}

void AudioManager::SetBusVolume(const std::string& name, float volume)
{
    if (!std::isfinite(volume)) return;
    m_busVolumes[name] = std::clamp(volume, 0.0f, 1.0f);
    m_dirty = true;
    UpdateAllSoundVolumes();
    UpdateAllMusicVolumes();
}

// A session-only multiplier (a capture or a scene muting a bus): never marks the config dirty, so the
// player's saved volume in audio.json stays what the Settings screen set.
void AudioManager::SetBusGain(const std::string& name, float gain)
{
    if (!std::isfinite(gain)) return;
    m_busGains[name] = std::clamp(gain, 0.0f, 1.0f);
    UpdateAllSoundVolumes();
    UpdateAllMusicVolumes();
}

float AudioManager::GetBusVolume(const std::string& name) const
{
    auto it = m_busVolumes.find(name);
    if (it != m_busVolumes.end()) {
        return it->second;
    }
    return 1.0f;
}

void AudioManager::Update(float dt)
{
    // 1. Flush dirty config at most once per second
    if (m_dirty) {
        m_saveTimer += dt;
        if (m_saveTimer >= 1.0f) {
            FlushAudioConfig();
            m_saveTimer = 0.0f;
            m_dirty = false;
        }
    }

    // 2. Reaps finished sound aliases, restarts looping sounds, updates positional sounds
    std::vector<int> finishedHandles;
    for (auto& [h, inst] : m_sounds) {
        if (!inst.Paused) {
            if (!IsSoundPlaying(inst.Alias)) {
                if (inst.Loop) {
                    PlaySound(inst.Alias);
                } else {
                    finishedHandles.push_back(h);
                    continue;
                }
            }
        }
        if (inst.IsPositional) {
            ApplyEffectivePan(inst);
            ApplyEffectiveVolume(inst);
        }
    }
    for (int h : finishedHandles) {
        auto it = m_sounds.find(h);
        if (it != m_sounds.end()) {
            UnloadSoundAlias(it->second.Alias);
            m_sounds.erase(it);
        }
    }

    // 3. Outgoing music fade
    if (m_outgoingMusic) {
        m_outgoingFadeTimer += dt;
        if (m_outgoingFadeDuration > 0.0f) {
            m_outgoingFadeFactor = s_outgoingFadeStart * (1.0f - (m_outgoingFadeTimer / m_outgoingFadeDuration));
        } else {
            m_outgoingFadeFactor = 0.0f;
        }
        if (m_outgoingFadeFactor <= 0.0f) {
            StopMusicStream(*m_outgoingMusic);
            m_outgoingMusic = nullptr;
        } else {
            float effOut = CalculateEffectiveVolume("music", m_outgoingMusicVolume) * m_outgoingFadeFactor;
            SetMusicVolume(*m_outgoingMusic, std::max(0.0f, effOut));
            if (IsMusicStreamPlaying(*m_outgoingMusic)) {
                UpdateMusicStream(*m_outgoingMusic);
            }
        }
    }

    // 4. Current music fade and stream update
    if (m_currentMusic) {
        if (m_musicFadeState == MusicFadeState::In) {
            m_musicFadeTimer += dt;
            if (m_musicFadeDuration > 0.0f) {
                m_musicFadeFactor = m_musicFadeTimer / m_musicFadeDuration;
            } else {
                m_musicFadeFactor = 1.0f;
            }
            if (m_musicFadeFactor >= 1.0f) {
                m_musicFadeFactor = 1.0f;
                m_musicFadeState = MusicFadeState::None;
            }
            float effCur = CalculateEffectiveVolume("music", m_currentMusicVolume) * m_musicFadeFactor;
            SetMusicVolume(*m_currentMusic, std::max(0.0f, effCur));
        } else if (m_musicFadeState == MusicFadeState::Out) {
            m_musicFadeTimer += dt;
            if (m_musicFadeDuration > 0.0f) {
                m_musicFadeFactor = s_musicFadeOutStart * (1.0f - (m_musicFadeTimer / m_musicFadeDuration));
            } else {
                m_musicFadeFactor = 0.0f;
            }
            if (m_musicFadeFactor <= 0.0f) {
                m_musicFadeFactor = 0.0f;
                m_musicFadeState = MusicFadeState::None;
                StopMusicStream(*m_currentMusic);
                m_currentMusic = nullptr;
                m_currentMusicPath.clear();
            } else {
                float effCur = CalculateEffectiveVolume("music", m_currentMusicVolume) * m_musicFadeFactor;
                SetMusicVolume(*m_currentMusic, std::max(0.0f, effCur));
            }
        } else {
            float effCur = CalculateEffectiveVolume("music", m_currentMusicVolume);
            SetMusicVolume(*m_currentMusic, std::max(0.0f, effCur));
        }

        if (m_currentMusic && IsMusicStreamPlaying(*m_currentMusic)) {
            UpdateMusicStream(*m_currentMusic);
        }

        // 5. Check if non-looping track ended to advance queue
        if (m_currentMusic && !m_currentMusicLoop && !m_musicPaused && m_musicFadeState != MusicFadeState::Out) {
            if (!IsMusicStreamPlaying(*m_currentMusic)) {
                StopMusicStream(*m_currentMusic);
                m_currentMusic = nullptr;
                m_currentMusicPath.clear();

                if (!m_musicQueue.empty()) {
                    std::string nextPath = m_musicQueue.front();
                    m_musicQueue.erase(m_musicQueue.begin());
                    PlayMusic(nextPath, m_lastFade, false, m_currentMusicVolume);
                }
            }
        }
    } else {
        if (!m_musicQueue.empty()) {
            std::string nextPath = m_musicQueue.front();
            m_musicQueue.erase(m_musicQueue.begin());
            PlayMusic(nextPath, m_lastFade, false, m_currentMusicVolume);
        }
    }
}

namespace Api {

namespace {

// Game side. The frame input's audio state is the audio side's as of its last Audio pass, which in
// the threaded engine is two game steps old. What this side asked since holds until the audio side
// has run that call (FrameInput::AudioDone reaches its number), then the audio side's state wins.
long long s_sent = 0;                                                  // calls recorded so far
int s_nextHandle = 1;
std::unordered_map<int, std::pair<bool, long long>> s_playing;         // handle: playing?, call number
std::unordered_map<std::string, std::pair<float, long long>> s_buses;  // bus: volume, call number
std::pair<std::string, long long> s_music{ "", 0 };                    // music path, call number

long long Send(RenderCommand command)
{
    RecordAudio(std::move(command));
    if (s_playing.size() > 256) {   // forget what the audio side has caught up with
        const long long done = GameInput().AudioDone;
        std::erase_if(s_playing, [done](const auto& entry) { return entry.second.second <= done; });
    }
    return ++s_sent;
}

bool Pending(long long call)
{
    return call > GameInput().AudioDone;
}

// A file that is not there gives handle 0 at once; one that fails to load later plays nothing.
int NewHandle(const std::string& path)
{
    if (!CurrentApp().GetAssets().GetResourceStore().Exists(path)) return 0;
    const int h = s_nextHandle;
    s_nextHandle = s_nextHandle == INT_MAX ? 1 : s_nextHandle + 1;   // wrap, no signed overflow (UB)
    return h;
}

} // namespace

int AudioPlay(const std::string& path, float volume, float pitch, float pan, bool loop, const std::string& bus)
{
    const int h = NewHandle(path);
    if (h) s_playing[h] = { true, Send([h, path, volume, pitch, pan, loop, bus]() { AudioManager::Get().Play(h, path, volume, pitch, pan, loop, bus); }) };
    return h;
}

int AudioPlayAt(const std::string& path, float x, float y, float volume, float pitch, bool loop, const std::string& bus)
{
    const int h = NewHandle(path);
    if (h) s_playing[h] = { true, Send([h, path, x, y, volume, pitch, loop, bus]() { AudioManager::Get().PlayAt(h, path, x, y, volume, pitch, loop, bus); }) };
    return h;
}

void AudioStop(int handle) { s_playing[handle] = { false, Send([handle]() { AudioManager::Get().Stop(handle); }) }; }
void AudioPause(int handle) { s_playing[handle] = { false, Send([handle]() { AudioManager::Get().Pause(handle); }) }; }
void AudioResume(int handle) { s_playing[handle] = { true, Send([handle]() { AudioManager::Get().Resume(handle); }) }; }

bool AudioIsPlaying(int handle)
{
    const auto mine = s_playing.find(handle);
    if (mine != s_playing.end() && Pending(mine->second.second)) return mine->second.first;
    const std::vector<int>& playing = GameInput().AudioPlaying;
    return std::find(playing.begin(), playing.end(), handle) != playing.end();
}

void AudioSetVolume(int handle, float volume) { Send([handle, volume]() { AudioManager::Get().SetVolume(handle, volume); }); }
void AudioSetPitch(int handle, float pitch) { Send([handle, pitch]() { AudioManager::Get().SetPitch(handle, pitch); }); }
void AudioSetPan(int handle, float pan) { Send([handle, pan]() { AudioManager::Get().SetPan(handle, pan); }); }
void AudioSetPosition(int handle, float x, float y) { Send([handle, x, y]() { AudioManager::Get().SetPosition(handle, x, y); }); }
void AudioStopAll(const std::string& bus) { Send([bus]() { AudioManager::Get().StopAll(bus); }); }
void AudioSetListener(float x, float y) { Send([x, y]() { AudioManager::Get().SetListener(x, y); }); }
void AudioSetFalloff(float nearDist, float farDist) { Send([nearDist, farDist]() { AudioManager::Get().SetFalloff(nearDist, farDist); }); }

void AudioMusic(const std::string& path, float fade, bool loop, float volume)
{
    s_music = { path, Send([path, fade, loop, volume]() { AudioManager::Get().PlayMusic(path, fade, loop, volume); }) };
}

void AudioStopMusic(float fade) { s_music = { "", Send([fade]() { AudioManager::Get().StopMusic(fade); }) }; }
void AudioPauseMusic() { Send([]() { AudioManager::Get().PauseMusic(); }); }
void AudioResumeMusic() { Send([]() { AudioManager::Get().ResumeMusic(); }); }
void AudioSetMusicPitch(float pitch) { if (std::isfinite(pitch)) Send([pitch]() { AudioManager::Get().SetMusicPitch(pitch); }); }
std::string AudioCurrentMusic() { return Pending(s_music.second) ? s_music.first : GameInput().Music; }
void AudioQueue(const std::string& path) { Send([path]() { AudioManager::Get().Queue(path); }); }
void AudioClearQueue() { Send([]() { AudioManager::Get().ClearQueue(); }); }

void AudioSetBusVolume(const std::string& name, float volume)
{
    const long long call = Send([name, volume]() { AudioManager::Get().SetBusVolume(name, volume); });
    if (std::isfinite(volume)) s_buses[name] = { std::clamp(volume, 0.0f, 1.0f), call };
}

void AudioSetBusGain(const std::string& name, float gain) { Send([name, gain]() { AudioManager::Get().SetBusGain(name, gain); }); }

float AudioGetBusVolume(const std::string& name)
{
    const auto mine = s_buses.find(name);
    if (mine != s_buses.end() && Pending(mine->second.second)) return mine->second.first;
    const auto& buses = GameInput().BusVolumes;
    const auto it = buses.find(name);
    return it != buses.end() ? it->second : 1.0f;
}

} // namespace Api

} // namespace IYI
