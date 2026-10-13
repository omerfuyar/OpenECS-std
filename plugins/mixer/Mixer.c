// The mixer standard plugin: plays sounds on the default output, many at once (DESIGN 3).
// It knows sounds as samples; loaders, such as audio, turn files into them. SDL's audio streams play the sounds, and SDL mixes the streams that play at once.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#pragma region Source Only

/// @brief A sound: 32-bit float samples, kept whole. Its handle is mixer.sound; the voices that play it keep it alive too.
typedef struct MixerSound
{
    f32 *samples; // the frames one after another, each with a sample for every channel
    u64 frames;
    SDL_AudioSpec spec; // 32-bit float samples, the sound's channels and rate
    u32 users;          // its handle, unless it was freed, and each voice that plays it
} MixerSound;

/// @brief A sound that plays: an audio stream bound to the output, which asks for more of the sound as it plays.
typedef struct MixerVoice
{
    i32 id;
    MixerSound *sound;
    SDL_AudioStream *stream;
    u64 position; // the next frame to give the stream; only the audio thread changes it while the voice plays
    bool loop;
    SDL_AtomicInt finished; // the stream has been given the whole sound
} MixerVoice;

static struct
{
    ECSPlugin plugin;
    MixerVoice **voices;
    usz voiceCount;
    i32 nextId;
    SDL_AudioDeviceID device; // 0 until the first sound plays
    bool audioStarted;
} MIXER = {0};

/// @brief Lets go of a sound, and frees it once nothing uses it.
static void MixerRelease(MixerSound *sound)
{
    if (--sound->users == 0)
    {
        SDL_free(sound->samples);
        SDL_free(sound);
    }
}

/// @brief The Destroy function of the handle type mixer.sound: the handle lets go of its sound.
static void MixerSoundDestroy(void *object)
{
    MixerRelease(object);
}

static MixerSound *MixerNewSound(i32 channels, i32 rate, SHUSlice samples)
{
    usz frameSize = (usz)SDL_max(channels, 1) * sizeof(f32);

    if (channels <= 0 || rate <= 0 || samples.size % frameSize != 0)
    {
        ECS_LogWarn(MIXER.plugin, "A sound needs channels and a rate above 0, and whole frames of float samples.");
        return nullptr;
    }

    MixerSound *sound = SDL_calloc(1, sizeof(MixerSound));
    sound->samples = sound == nullptr || samples.size == 0 ? nullptr : SDL_malloc(samples.size);

    if (sound == nullptr || (samples.size != 0 && sound->samples == nullptr))
    {
        SDL_free(sound);
        return nullptr;
    }

    SDL_memcpy(sound->samples, samples.data, samples.size);
    sound->frames = samples.size / frameSize;
    sound->spec = (SDL_AudioSpec){.format = SDL_AUDIO_F32, .channels = channels, .freq = rate};
    sound->users = 1;
    return sound;
}

static f64 MixerLength(MixerSound *sound)
{
    return (f64)sound->frames / (f64)sound->spec.freq;
}

/// @brief Gives a voice's stream more of its sound; SDL calls it on its audio thread when the stream runs low.
static void SDLCALL MixerFeed(void *data, SDL_AudioStream *stream, int needed, int total)
{
    (void)total;
    MixerVoice *voice = data;
    usz frameSize = (usz)voice->sound->spec.channels * sizeof(f32);

    while (needed > 0 && !SDL_GetAtomicInt(&voice->finished))
    {
        if (voice->position >= voice->sound->frames)
        {
            if (!voice->loop || voice->sound->frames == 0)
            {
                SDL_SetAtomicInt(&voice->finished, 1);
                break;
            }

            voice->position = 0;
        }

        u64 frames = SDL_min(voice->sound->frames - voice->position, (u64)needed / frameSize + 1);
        SDL_PutAudioStreamData(stream, voice->sound->samples + voice->position * (u64)voice->sound->spec.channels, (int)(frames * frameSize));
        voice->position += frames;
        needed -= (int)(frames * frameSize);
    }
}

static void MixerFreeVoice(MixerVoice *voice)
{
    SDL_DestroyAudioStream(voice->stream);
    MixerRelease(voice->sound);
    SDL_free(voice);
}

/// @brief Forgets the voices whose sound has ended and whose stream has played it all.
static void MixerTidy(void)
{
    for (usz i = MIXER.voiceCount; i > 0; i--)
    {
        MixerVoice *voice = MIXER.voices[i - 1];

        if (SDL_GetAtomicInt(&voice->finished) && SDL_GetAudioStreamQueued(voice->stream) == 0)
        {
            MixerFreeVoice(voice);
            MIXER.voices[i - 1] = MIXER.voices[--MIXER.voiceCount];
        }
    }
}

static MixerVoice *MixerFindVoice(i32 id)
{
    for (usz i = 0; i < MIXER.voiceCount; i++)
    {
        if (MIXER.voices[i]->id == id)
        {
            return MIXER.voices[i];
        }
    }

    return nullptr;
}

/// @brief Opens the default output the first time a sound plays.
/// @return false if there is none; the reason is logged.
static bool MixerOpen(void)
{
    if (MIXER.device != 0)
    {
        return true;
    }

    if (!MIXER.audioStarted)
    {
        MIXER.audioStarted = SDL_InitSubSystem(SDL_INIT_AUDIO);
    }

    MIXER.device = MIXER.audioStarted ? SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr) : 0;

    if (MIXER.device == 0)
    {
        ECS_LogWarn(MIXER.plugin, "There is no audio output: %s", SDL_GetError());
    }

    return MIXER.device != 0;
}

static i32 MixerPlay(MixerSound *sound, f32 volume, bool loop)
{
    MixerTidy();
    MixerVoice **voices = MixerOpen() ? SDL_realloc(MIXER.voices, (MIXER.voiceCount + 1) * sizeof(MixerVoice *)) : nullptr;
    MixerVoice *voice = voices == nullptr ? nullptr : SDL_calloc(1, sizeof(MixerVoice));

    if (voices != nullptr)
    {
        MIXER.voices = voices;
    }

    if (voice == nullptr)
    {
        return 0;
    }

    *voice = (MixerVoice){.id = ++MIXER.nextId, .sound = sound, .loop = loop};
    voice->stream = SDL_CreateAudioStream(&sound->spec, nullptr);

    if (voice->stream == nullptr || !SDL_SetAudioStreamGain(voice->stream, SDL_max(0.0f, volume)) || !SDL_SetAudioStreamGetCallback(voice->stream, MixerFeed, voice) || !SDL_BindAudioStream(MIXER.device, voice->stream))
    {
        ECS_LogError(MIXER.plugin, "Cannot play a sound: %s", SDL_GetError());
        SDL_DestroyAudioStream(voice->stream);
        SDL_free(voice);
        return 0;
    }

    // the voice keeps the sound alive while it plays, also when its handle lets go of it
    sound->users++;
    MIXER.voices[MIXER.voiceCount++] = voice;
    return voice->id;
}

static void MixerStop(i32 id)
{
    MixerVoice *voice = MixerFindVoice(id);

    if (voice != nullptr)
    {
        SDL_SetAtomicInt(&voice->finished, 1);
        SDL_ClearAudioStream(voice->stream);
    }

    MixerTidy();
}

static void MixerStopAll(void)
{
    for (usz i = 0; i < MIXER.voiceCount; i++)
    {
        SDL_SetAtomicInt(&MIXER.voices[i]->finished, 1);
        SDL_ClearAudioStream(MIXER.voices[i]->stream);
    }

    MixerTidy();
}

static bool MixerPlaying(i32 id)
{
    MixerTidy();
    return MixerFindVoice(id) != nullptr;
}

static void MixerVolume(i32 id, f32 volume)
{
    MixerVoice *voice = MixerFindVoice(id);

    if (voice != nullptr)
    {
        SDL_SetAudioStreamGain(voice->stream, SDL_max(0.0f, volume));
    }
}

/// @brief Lets go of a sound now; it is freed once the voices that play it end.
static void MixerFreeSound(MixerSound *sound)
{
    ECSHandle_Forget(sound);
    MixerRelease(sound);
}

/// @brief Stops every voice and closes the output; the Destroy function of the plugin.
static void MixerFree(void *data)
{
    (void)data;

    for (usz i = 0; i < MIXER.voiceCount; i++)
    {
        MixerFreeVoice(MIXER.voices[i]);
    }

    if (MIXER.device != 0)
    {
        SDL_CloseAudioDevice(MIXER.device);
    }

    if (MIXER.audioStarted)
    {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    SDL_free(MIXER.voices);
    SDL_zero(MIXER);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    MIXER.plugin = plugin;

    // a plugin whose Init fails is not destroyed, so it cleans up here
    const ECSPluginDesc desc = {.Destroy = MixerFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc), MixerFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "sound", MixerSoundDestroy), MixerFree(nullptr););

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"newSound", (ECSFunction)MixerNewSound, "handle<mixer.sound>(int channels, int rate, buffer samples)", "Make a sound from 32-bit float samples, frame after frame, at a rate in frames a second"},
        {"length", (ECSFunction)MixerLength, "double(handle<mixer.sound> sound)", "Give a sound's length in seconds"},
        {"play", (ECSFunction)MixerPlay, "int(handle<mixer.sound> sound, float volume, bool loop)", "Play a sound, and give the number of the voice that plays it; 0 if it cannot play"},
        {"stop", (ECSFunction)MixerStop, "void(int voice)", "Stop a voice"},
        {"stopAll", (ECSFunction)MixerStopAll, "void()", "Stop every voice"},
        {"playing", (ECSFunction)MixerPlaying, "bool(int voice)", "Tell whether a voice still plays"},
        {"volume", (ECSFunction)MixerVolume, "void(int voice, float volume)", "Change a voice's volume; 1 is the sound's own"},
        {"freeSound", (ECSFunction)MixerFreeSound, "void(handle<mixer.sound> sound)", "Free a sound now, instead of when no one uses its handle; voices that play it finish first"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description), MixerFree(nullptr););
    }

    return SHUResult_Ok;
}
