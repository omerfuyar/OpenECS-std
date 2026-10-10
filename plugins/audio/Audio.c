// The audio standard plugin: reads sound files, WAV, MP3, FLAC and OGG, and plays them on the default output (DESIGN 6).
// dr_wav, dr_mp3, dr_flac and stb_vorbis read the files whole; SDL's audio streams play them, and SDL mixes the streams that play at once.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

#pragma region Source Only

/// @brief A name of the plugin: "audio." and a local name.
#define AUDIO_NAME(localName) "audio." localName

/// @brief A sound file, read once and kept whole.
typedef struct AudioSound
{
    char *path;
    f32 *samples; // the frames one after another, each with a sample for every channel
    u64 frames;
    SDL_AudioSpec spec; // 32-bit float samples, the file's channels and rate
} AudioSound;

/// @brief A sound that plays: an audio stream bound to the output, which asks for more of the sound as it plays.
typedef struct AudioVoice
{
    i32 id;
    AudioSound *sound;
    SDL_AudioStream *stream;
    u64 position; // the next frame to give the stream; only the audio thread changes it while the voice plays
    bool loop;
    SDL_AtomicInt finished; // the stream has been given the whole sound
} AudioVoice;

static struct
{
    ECSPlugin plugin;
    AudioSound **sounds;
    usz soundCount;
    AudioVoice **voices;
    usz voiceCount;
    i32 nextId;
    SDL_AudioDeviceID device; // 0 until the first sound plays
    bool audioStarted;
} AUDIO = {0};

// dr_libs allocate with SDL's allocator, so the samples they give are freed like the plugin's own
static void *AudioMalloc(size_t size, void *data)
{
    (void)data;
    return SDL_malloc(size);
}

static void *AudioRealloc(void *memory, size_t size, void *data)
{
    (void)data;
    return SDL_realloc(memory, size);
}

static void AudioRelease(void *memory, void *data)
{
    (void)data;
    SDL_free(memory);
}

static const drwav_allocation_callbacks AUDIO_WAV_ALLOCATION = {NULL, AudioMalloc, AudioRealloc, AudioRelease};
static const drmp3_allocation_callbacks AUDIO_MP3_ALLOCATION = {NULL, AudioMalloc, AudioRealloc, AudioRelease};
static const drflac_allocation_callbacks AUDIO_FLAC_ALLOCATION = {NULL, AudioMalloc, AudioRealloc, AudioRelease};

static bool AudioEndsWith(const char *text, const char *suffix)
{
    usz length = SDL_strlen(text);
    usz suffixLength = SDL_strlen(suffix);
    return length >= suffixLength && SDL_strcasecmp(text + length - suffixLength, suffix) == 0;
}

/// @brief Reads a sound file whole into 32-bit float samples.
/// @return The sound, or NULL if the file cannot be read; the reason is logged.
static AudioSound *AudioRead(const char *path)
{
    char *full = NULL;
    AudioSound *sound = SDL_calloc(1, sizeof(AudioSound));

    // a relative path starts at the executable's folder
    if (sound == NULL || SDL_asprintf(&full, "%s%s", path[0] == '/' ? "" : SDL_GetBasePath(), path) < 0)
    {
        SDL_free(sound);
        return NULL;
    }

    unsigned int channels = 0;
    unsigned int rate = 0;
    u64 frames = 0;
    f32 *samples = NULL;

    if (AudioEndsWith(full, ".wav"))
    {
        drwav_uint64 count = 0;
        samples = drwav_open_file_and_read_pcm_frames_f32(full, &channels, &rate, &count, &AUDIO_WAV_ALLOCATION);
        frames = count;
    }
    else if (AudioEndsWith(full, ".mp3"))
    {
        drmp3_config config = {0};
        drmp3_uint64 count = 0;
        samples = drmp3_open_file_and_read_pcm_frames_f32(full, &config, &count, &AUDIO_MP3_ALLOCATION);
        channels = config.channels;
        rate = config.sampleRate;
        frames = count;
    }
    else if (AudioEndsWith(full, ".flac"))
    {
        drflac_uint64 count = 0;
        samples = drflac_open_file_and_read_pcm_frames_f32(full, &channels, &rate, &count, &AUDIO_FLAC_ALLOCATION);
        frames = count;
    }
    else if (AudioEndsWith(full, ".ogg"))
    {
        int oggChannels = 0;
        int oggRate = 0;
        short *decoded = NULL;
        int count = stb_vorbis_decode_filename(full, &oggChannels, &oggRate, &decoded);

        if (count > 0 && decoded != NULL)
        {
            channels = (unsigned int)oggChannels;
            rate = (unsigned int)oggRate;
            frames = (u64)count;
            samples = SDL_malloc((usz)count * channels * sizeof(f32));

            for (usz i = 0; samples != NULL && i < (usz)count * channels; i++)
            {
                samples[i] = (f32)decoded[i] / 32768.0f;
            }
        }

        free(decoded);
    }

    if (samples == NULL || channels == 0 || rate == 0)
    {
        ECS_Log(AUDIO.plugin, ECSLogLevel_Error, "Cannot read the sound '%s': not a WAV, MP3, FLAC or OGG file that can be read.", full);
        SDL_free(samples);
        SDL_free(full);
        SDL_free(sound);
        return NULL;
    }

    SDL_free(full);
    sound->path = SDL_strdup(path);
    sound->samples = samples;
    sound->frames = frames;
    sound->spec = (SDL_AudioSpec){.format = SDL_AUDIO_F32, .channels = (int)channels, .freq = (int)rate};
    return sound;
}

static void AudioFreeSound(AudioSound *sound)
{
    SDL_free(sound->samples);
    SDL_free(sound->path);
    SDL_free(sound);
}

/// @brief Gets a sound file, reading it the first time; a file that cannot be read is tried again next time.
static AudioSound *AudioLoad(const char *path)
{
    for (usz i = 0; i < AUDIO.soundCount; i++)
    {
        if (SDL_strcmp(AUDIO.sounds[i]->path, path) == 0)
        {
            return AUDIO.sounds[i];
        }
    }

    AudioSound *sound = AudioRead(path);
    AudioSound **sounds = sound == NULL || sound->path == NULL ? NULL : SDL_realloc(AUDIO.sounds, (AUDIO.soundCount + 1) * sizeof(AudioSound *));

    if (sounds == NULL)
    {
        if (sound != NULL)
        {
            AudioFreeSound(sound);
        }

        return NULL;
    }

    AUDIO.sounds = sounds;
    AUDIO.sounds[AUDIO.soundCount++] = sound;
    return sound;
}

static f64 AudioLength(AudioSound *sound)
{
    return (f64)sound->frames / (f64)sound->spec.freq;
}

/// @brief Gives a voice's stream more of its sound; SDL calls it on its audio thread when the stream runs low.
static void SDLCALL AudioFeed(void *data, SDL_AudioStream *stream, int needed, int total)
{
    (void)total;
    AudioVoice *voice = data;
    usz frameSize = (usz)voice->sound->spec.channels * sizeof(f32);

    while (needed > 0 && !SDL_GetAtomicInt(&voice->finished))
    {
        if (voice->position >= voice->sound->frames)
        {
            if (!voice->loop)
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

static void AudioFreeVoice(AudioVoice *voice)
{
    SDL_DestroyAudioStream(voice->stream);
    SDL_free(voice);
}

/// @brief Forgets the voices whose sound has ended and whose stream has played it all.
static void AudioTidy(void)
{
    for (usz i = AUDIO.voiceCount; i > 0; i--)
    {
        AudioVoice *voice = AUDIO.voices[i - 1];

        if (SDL_GetAtomicInt(&voice->finished) && SDL_GetAudioStreamQueued(voice->stream) == 0)
        {
            AudioFreeVoice(voice);
            AUDIO.voices[i - 1] = AUDIO.voices[--AUDIO.voiceCount];
        }
    }
}

static AudioVoice *AudioFindVoice(i32 id)
{
    for (usz i = 0; i < AUDIO.voiceCount; i++)
    {
        if (AUDIO.voices[i]->id == id)
        {
            return AUDIO.voices[i];
        }
    }

    return NULL;
}

/// @brief Opens the default output the first time a sound plays.
/// @return false if there is none; the reason is logged once.
static bool AudioOpen(void)
{
    if (AUDIO.device != 0)
    {
        return true;
    }

    if (!AUDIO.audioStarted)
    {
        AUDIO.audioStarted = SDL_InitSubSystem(SDL_INIT_AUDIO);
    }

    AUDIO.device = AUDIO.audioStarted ? SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL) : 0;

    if (AUDIO.device == 0)
    {
        ECS_Log(AUDIO.plugin, ECSLogLevel_Warning, "There is no audio output: %s", SDL_GetError());
    }

    return AUDIO.device != 0;
}

static i32 AudioPlay(AudioSound *sound, f32 volume, bool loop)
{
    AudioTidy();
    AudioVoice **voices = AudioOpen() ? SDL_realloc(AUDIO.voices, (AUDIO.voiceCount + 1) * sizeof(AudioVoice *)) : NULL;
    AudioVoice *voice = voices == NULL ? NULL : SDL_calloc(1, sizeof(AudioVoice));

    if (voices != NULL)
    {
        AUDIO.voices = voices;
    }

    if (voice == NULL)
    {
        return 0;
    }

    *voice = (AudioVoice){.id = ++AUDIO.nextId, .sound = sound, .loop = loop};
    voice->stream = SDL_CreateAudioStream(&sound->spec, NULL);

    if (voice->stream == NULL || !SDL_SetAudioStreamGain(voice->stream, SDL_max(0.0f, volume)) || !SDL_SetAudioStreamGetCallback(voice->stream, AudioFeed, voice) || !SDL_BindAudioStream(AUDIO.device, voice->stream))
    {
        ECS_Log(AUDIO.plugin, ECSLogLevel_Error, "Cannot play '%s': %s", sound->path, SDL_GetError());
        SDL_DestroyAudioStream(voice->stream);
        SDL_free(voice);
        return 0;
    }

    AUDIO.voices[AUDIO.voiceCount++] = voice;
    return voice->id;
}

static void AudioStop(i32 id)
{
    AudioVoice *voice = AudioFindVoice(id);

    if (voice != NULL)
    {
        SDL_SetAtomicInt(&voice->finished, 1);
        SDL_ClearAudioStream(voice->stream);
    }

    AudioTidy();
}

static void AudioStopAll(void)
{
    for (usz i = 0; i < AUDIO.voiceCount; i++)
    {
        SDL_SetAtomicInt(&AUDIO.voices[i]->finished, 1);
        SDL_ClearAudioStream(AUDIO.voices[i]->stream);
    }

    AudioTidy();
}

static bool AudioPlaying(i32 id)
{
    AudioTidy();
    return AudioFindVoice(id) != NULL;
}

static void AudioVolume(i32 id, f32 volume)
{
    AudioVoice *voice = AudioFindVoice(id);

    if (voice != NULL)
    {
        SDL_SetAudioStreamGain(voice->stream, SDL_max(0.0f, volume));
    }
}

/// @brief A sound's handle stays valid until the plugin shuts down, which frees the sounds; the handle has nothing to free.
static void AudioForget(void *object)
{
    (void)object;
}

static void AudioFree(void)
{
    for (usz i = 0; i < AUDIO.voiceCount; i++)
    {
        AudioFreeVoice(AUDIO.voices[i]);
    }

    for (usz i = 0; i < AUDIO.soundCount; i++)
    {
        AudioFreeSound(AUDIO.sounds[i]);
    }

    if (AUDIO.device != 0)
    {
        SDL_CloseAudioDevice(AUDIO.device);
    }

    if (AUDIO.audioStarted)
    {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    SDL_free(AUDIO.voices);
    SDL_free(AUDIO.sounds);
    SDL_zero(AUDIO);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    AUDIO.plugin = plugin;
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, AUDIO_NAME("sound"), AudioForget), AudioFree(););

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {AUDIO_NAME("load"), (ECSFunction)AudioLoad, "handle<audio.sound>(string path)", "Read a sound file, once; nothing if it cannot be read"},
        {AUDIO_NAME("length"), (ECSFunction)AudioLength, "double(handle<audio.sound> sound)", "Give a sound's length in seconds"},
        {AUDIO_NAME("play"), (ECSFunction)AudioPlay, "int(handle<audio.sound> sound, float volume, bool loop)", "Play a sound, and give the number of the voice that plays it; 0 if it cannot play"},
        {AUDIO_NAME("stop"), (ECSFunction)AudioStop, "void(int voice)", "Stop a voice"},
        {AUDIO_NAME("stopAll"), (ECSFunction)AudioStopAll, "void()", "Stop every voice"},
        {AUDIO_NAME("playing"), (ECSFunction)AudioPlaying, "bool(int voice)", "Tell whether a voice still plays"},
        {AUDIO_NAME("volume"), (ECSFunction)AudioVolume, "void(int voice, float volume)", "Change a voice's volume; 1 is the sound's own"},
    };

    // a plugin whose Init fails gets no Shutdown, so it cleans up here
    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description), AudioFree(););
    }

    return SHUResult_Ok;
}

void ECSPlugin_Shutdown(ECSPlugin plugin)
{
    (void)plugin;
    AudioFree();
}
