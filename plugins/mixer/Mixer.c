// The mixer standard plugin: plays sounds on the default output, many at once (DESIGN 3).
// It knows sounds as samples; loaders, such as audio, turn files into them. SDL's audio streams play the sounds, and SDL mixes the streams that play at once.
// A stream is a sound whose samples a plugin gives while it plays, such as a synthesizer's.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#pragma region Source Only

/// @brief A sound: 32-bit float samples, kept whole, or a stream's, queued as they come. Its handle is mixer.sound; the voices that play it keep it alive too.
typedef struct MixerSound
{
    f32 *samples; // the frames one after another, each with a sample for every channel
    u64 frames;
    SDL_AudioSpec spec; // 32-bit float samples, the sound's channels and rate
    u32 users;          // its handle, unless it was freed, and each voice that plays it

    // a stream: the queue that the main thread fills and its voice empties, which SDL keeps thread-safe
    SDL_AudioStream *queue;
    SDL_AtomicInt finished; // no more samples come
    bool playing;           // a voice plays it; a stream plays in one voice at a time
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
    SDL_AtomicInt pan;      // from -1000, left, to 1000, right
    i32 channels;           // the channels the voice gives its stream: a mono sound plays in stereo, so it can be panned
    f32 *scratch;           // the audio thread's room for samples on their way
    usz scratchSize;        // in floats
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
        SDL_DestroyAudioStream(sound->queue);
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

static MixerSound *MixerNewStream(i32 channels, i32 rate)
{
    if (channels <= 0 || rate <= 0)
    {
        ECS_LogWarn(MIXER.plugin, "A stream needs channels and a rate above 0.");
        return nullptr;
    }

    MixerSound *sound = SDL_calloc(1, sizeof(MixerSound));

    if (sound == nullptr)
    {
        return nullptr;
    }

    sound->spec = (SDL_AudioSpec){.format = SDL_AUDIO_F32, .channels = channels, .freq = rate};
    sound->queue = SDL_CreateAudioStream(&sound->spec, &sound->spec);
    sound->users = 1;

    if (sound->queue == nullptr)
    {
        ECS_LogError(MIXER.plugin, "Cannot make a stream: %s", SDL_GetError());
        SDL_free(sound);
        return nullptr;
    }

    return sound;
}

static bool MixerQueue(MixerSound *sound, SHUSlice samples)
{
    usz frameSize = (usz)sound->spec.channels * sizeof(f32);

    if (sound->queue == nullptr || SDL_GetAtomicInt(&sound->finished) || samples.size % frameSize != 0 || samples.size > (usz)SDL_MAX_SINT32)
    {
        return false;
    }

    return SDL_PutAudioStreamData(sound->queue, samples.data, (int)samples.size);
}

static f64 MixerQueued(MixerSound *sound)
{
    int bytes = sound->queue == nullptr ? 0 : SDL_GetAudioStreamAvailable(sound->queue);
    return (f64)SDL_max(bytes, 0) / (f64)((usz)sound->spec.channels * sizeof(f32)) / (f64)sound->spec.freq;
}

static void MixerFinish(MixerSound *sound)
{
    if (sound->queue != nullptr)
    {
        SDL_SetAtomicInt(&sound->finished, 1);
    }
}

static f64 MixerLength(MixerSound *sound)
{
    return (f64)sound->frames / (f64)sound->spec.freq;
}

/// @brief Makes sure a voice's room for samples holds a number of floats, on the audio thread.
static bool MixerScratch(MixerVoice *voice, usz floats)
{
    if (voice->scratchSize < floats)
    {
        f32 *scratch = SDL_realloc(voice->scratch, floats * sizeof(f32));

        if (scratch == nullptr)
        {
            return false;
        }

        voice->scratch = scratch;
        voice->scratchSize = floats;
    }

    return true;
}

/// @brief Takes up to a number of frames of a voice's sound: from its samples, going round if it loops, or from its stream's queue.
/// @return Where the frames are, and how many there are in retCount; nullptr when there are none now.
static const f32 *MixerTake(MixerVoice *voice, u64 wanted, u64 *retCount)
{
    MixerSound *sound = voice->sound;
    usz frameSize = (usz)sound->spec.channels * sizeof(f32);
    *retCount = 0;

    if (sound->queue != nullptr)
    {
        int got = MixerScratch(voice, (usz)wanted * (usz)sound->spec.channels * 3) ? SDL_GetAudioStreamData(sound->queue, voice->scratch, (int)(wanted * frameSize)) : 0;

        // a stream ends once it is finished and its queue is empty; until then, a voice that runs out waits for more
        if (got <= 0 && SDL_GetAtomicInt(&sound->finished) && SDL_GetAudioStreamAvailable(sound->queue) == 0)
        {
            SDL_SetAtomicInt(&voice->finished, 1);
        }

        *retCount = got > 0 ? (u64)got / frameSize : 0;
        return *retCount > 0 ? voice->scratch : nullptr;
    }

    if (voice->position >= sound->frames)
    {
        if (!voice->loop || sound->frames == 0)
        {
            SDL_SetAtomicInt(&voice->finished, 1);
            return nullptr;
        }

        voice->position = 0;
    }

    *retCount = SDL_min(sound->frames - voice->position, wanted);
    const f32 *frames = sound->samples + voice->position * (u64)sound->spec.channels;
    voice->position += *retCount;
    return frames;
}

/// @brief Gives a voice's stream more of its sound, panned; SDL calls it on its audio thread when the stream runs low.
static void SDLCALL MixerFeed(void *data, SDL_AudioStream *stream, int needed, int total)
{
    (void)total;
    MixerVoice *voice = data;
    i32 in = voice->sound->spec.channels;
    i32 out = voice->channels;
    usz outFrameSize = (usz)out * sizeof(f32);

    while (needed > 0 && !SDL_GetAtomicInt(&voice->finished))
    {
        u64 count = 0;
        const f32 *frames = MixerTake(voice, (u64)needed / outFrameSize + 1, &count);

        if (frames == nullptr)
        {
            break;
        }

        // the left and right channels take a share of the balance; at the middle, both keep their whole gain
        f32 pan = (f32)SDL_GetAtomicInt(&voice->pan) / 1000.0f;
        f32 left = SDL_min(1.0f, 1.0f - pan);
        f32 right = SDL_min(1.0f, 1.0f + pan);

        if (out == in && (out != 2 || pan == 0.0f))
        {
            SDL_PutAudioStreamData(stream, frames, (int)(count * outFrameSize));
        }
        else
        {
            // a stream's frames are in the room already, so the panned frames go after them
            usz offset = voice->sound->queue != nullptr ? (usz)count * (usz)in : 0;

            if (!MixerScratch(voice, offset + (usz)count * (usz)out))
            {
                break;
            }

            frames = voice->sound->queue != nullptr ? voice->scratch : frames;
            f32 *panned = voice->scratch + offset;

            for (u64 i = 0; i < count; i++)
            {
                f32 l = frames[i * (u64)in];
                f32 r = in == 1 ? l : frames[i * (u64)in + 1];
                panned[2 * i] = l * left;
                panned[2 * i + 1] = r * right;
            }

            SDL_PutAudioStreamData(stream, panned, (int)(count * outFrameSize));
        }

        needed -= (int)(count * outFrameSize);
    }

    // a stream that converts the rate keeps its last frames back until it is flushed, so a voice that has ended flushes them, and its stream empties
    if (SDL_GetAtomicInt(&voice->finished))
    {
        SDL_FlushAudioStream(stream);
    }
}

static void MixerFreeVoice(MixerVoice *voice)
{
    SDL_DestroyAudioStream(voice->stream);
    voice->sound->playing = false;
    SDL_free(voice->scratch);
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

    // a stream's samples go to one voice, so it plays in one voice at a time
    if (voice == nullptr || (sound->queue != nullptr && sound->playing))
    {
        SDL_free(voice);
        return 0;
    }

    // a mono sound plays in stereo, so it can be panned
    *voice = (MixerVoice){.id = ++MIXER.nextId, .sound = sound, .loop = loop && sound->queue == nullptr, .channels = sound->spec.channels == 1 ? 2 : sound->spec.channels};
    SDL_AudioSpec spec = {.format = SDL_AUDIO_F32, .channels = voice->channels, .freq = sound->spec.freq};
    voice->stream = SDL_CreateAudioStream(&spec, nullptr);

    if (voice->stream == nullptr || !SDL_SetAudioStreamGain(voice->stream, SDL_max(0.0f, volume)) || !SDL_SetAudioStreamGetCallback(voice->stream, MixerFeed, voice) || !SDL_BindAudioStream(MIXER.device, voice->stream))
    {
        ECS_LogError(MIXER.plugin, "Cannot play a sound: %s", SDL_GetError());
        SDL_DestroyAudioStream(voice->stream);
        SDL_free(voice);
        return 0;
    }

    // the voice keeps the sound alive while it plays, also when its handle lets go of it
    sound->users++;
    sound->playing = true;
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

static void MixerPan(i32 id, f32 pan)
{
    MixerVoice *voice = MixerFindVoice(id);

    if (voice != nullptr)
    {
        SDL_SetAtomicInt(&voice->pan, (int)SDL_roundf(SDL_clamp(pan, -1.0f, 1.0f) * 1000.0f));
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
        {"newStream", (ECSFunction)MixerNewStream, "handle<mixer.sound>(int channels, int rate)", "Make a stream: a sound whose 32-bit float samples are given while it plays, with queue"},
        {"queue", (ECSFunction)MixerQueue, "bool(handle<mixer.sound> stream, buffer samples)", "Add samples to the end of a stream; false if it is not a stream, it is finished, or the samples are not whole frames"},
        {"queued", (ECSFunction)MixerQueued, "double(handle<mixer.sound> stream)", "Give the seconds of a stream's samples that wait to play"},
        {"finish", (ECSFunction)MixerFinish, "void(handle<mixer.sound> stream)", "Tell that no more samples come, so the stream's voice ends once it has played what waits"},
        {"length", (ECSFunction)MixerLength, "double(handle<mixer.sound> sound)", "Give a sound's length in seconds; 0 for a stream"},
        {"play", (ECSFunction)MixerPlay, "int(handle<mixer.sound> sound, float volume, bool loop)", "Play a sound, and give the number of the voice that plays it; 0 if it cannot play"},
        {"stop", (ECSFunction)MixerStop, "void(int voice)", "Stop a voice"},
        {"stopAll", (ECSFunction)MixerStopAll, "void()", "Stop every voice"},
        {"playing", (ECSFunction)MixerPlaying, "bool(int voice)", "Tell whether a voice still plays"},
        {"volume", (ECSFunction)MixerVolume, "void(int voice, float volume)", "Change a voice's volume; 1 is the sound's own"},
        {"pan", (ECSFunction)MixerPan, "void(int voice, float pan)", "Move a voice from left, -1, to right, 1; 0 is the middle"},
        {"freeSound", (ECSFunction)MixerFreeSound, "void(handle<mixer.sound> sound)", "Free a sound now, instead of when no one uses its handle; voices that play it finish first"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description), MixerFree(nullptr););
    }

    return SHUResult_Ok;
}
