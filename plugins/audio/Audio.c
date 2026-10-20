// The audio standard plugin: reads sound files, WAV, MP3, FLAC and OGG, into sounds of the mixer plugin, which plays them (DESIGN 7).
// dr_wav, dr_mp3, dr_flac and stb_vorbis read the files whole, or a piece at a time into a stream of mixer, for long files such as music.

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

/// @brief How often, in seconds, the streams are fed, how many seconds of each wait in mixer, and how many are read at a time.
#define AUDIO_FEED_INTERVAL 0.1
#define AUDIO_AHEAD 0.5
#define AUDIO_PIECE 0.25

/// @brief The functions of mixer that audio uses.
typedef void *(*AudioNewFunction)(i32 channels, i32 rate, SHUSlice samples);
typedef void *(*AudioNewStreamFunction)(i32 channels, i32 rate);
typedef bool (*AudioQueueFunction)(void *sound, SHUSlice samples);
typedef f64 (*AudioQueuedFunction)(void *sound);
typedef void (*AudioSoundFunction)(void *sound);

/// @brief A file that is read into a stream of mixer while it plays.
typedef struct AudioStream
{
    void *sound;     // the stream, which audio keeps until the file ends
    u32 channels;
    u32 rate;
    i32 format;      // AudioFormat_*
    union
    {
        drwav wav;
        drmp3 mp3;
        drflac *flac;
        stb_vorbis *ogg;
    } decoder;
    f32 *piece;      // room for the samples of AUDIO_PIECE seconds
} AudioStream;

enum
{
    AudioFormat_Wav,
    AudioFormat_Mp3,
    AudioFormat_Flac,
    AudioFormat_Ogg,
};

static struct
{
    ECSPlugin plugin;
    AudioNewFunction NewSound;
    AudioNewStreamFunction NewStream;
    AudioQueueFunction Queue;
    AudioQueuedFunction Queued;
    AudioSoundFunction Finish;
    AudioSoundFunction Keep;
    AudioSoundFunction Release;
    AudioStream **streams;
    usz streamCount;
    ECSTimer timer; // feeds the streams while there are any
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

static const drwav_allocation_callbacks AUDIO_WAV_ALLOCATION = {nullptr, AudioMalloc, AudioRealloc, AudioRelease};
static const drmp3_allocation_callbacks AUDIO_MP3_ALLOCATION = {nullptr, AudioMalloc, AudioRealloc, AudioRelease};
static const drflac_allocation_callbacks AUDIO_FLAC_ALLOCATION = {nullptr, AudioMalloc, AudioRealloc, AudioRelease};

static bool AudioEndsWith(const char *text, const char *suffix)
{
    usz length = SDL_strlen(text);
    usz suffixLength = SDL_strlen(suffix);
    return length >= suffixLength && SDL_strcasecmp(text + length - suffixLength, suffix) == 0;
}

/// @brief Reads a sound file whole into a new sound of the mixer plugin.
/// @return The sound, or nullptr if the file cannot be read; the reason is logged.
static void *AudioLoad(const char *path)
{
    char *full = nullptr;

    // a relative path starts at the executable's folder
    if (SDL_asprintf(&full, "%s%s", ECSPath_IsAbsolute(path) ? "" : SDL_GetBasePath(), path) < 0)
    {
        return nullptr;
    }

    unsigned int channels = 0;
    unsigned int rate = 0;
    u64 frames = 0;
    f32 *samples = nullptr;

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
        short *decoded = nullptr;
        int count = stb_vorbis_decode_filename(full, &oggChannels, &oggRate, &decoded);

        if (count > 0 && decoded != nullptr)
        {
            channels = (unsigned int)oggChannels;
            rate = (unsigned int)oggRate;
            frames = (u64)count;
            samples = SDL_malloc((usz)count * channels * sizeof(f32));

            for (usz i = 0; samples != nullptr && i < (usz)count * channels; i++)
            {
                samples[i] = (f32)decoded[i] / 32768.0f;
            }
        }

        free(decoded);
    }

    if (samples == nullptr || channels == 0 || rate == 0)
    {
        ECS_LogError(AUDIO.plugin, "Cannot read the sound '%s': not a WAV, MP3, FLAC or OGG file that can be read.", full);
        SDL_free(samples);
        SDL_free(full);
        return nullptr;
    }

    // mixer copies the samples into its sound
    void *sound = AUDIO.NewSound((i32)channels, (i32)rate, cs(samples, (usz)frames * channels * sizeof(f32)));
    SDL_free(samples);
    SDL_free(full);
    return sound;
}

/// @brief Closes a stream's file and lets go of its sound.
static void AudioClose(AudioStream *stream)
{
    switch (stream->format)
    {
    case AudioFormat_Wav:
        drwav_uninit(&stream->decoder.wav);
        break;
    case AudioFormat_Mp3:
        drmp3_uninit(&stream->decoder.mp3);
        break;
    case AudioFormat_Flac:
        drflac_close(stream->decoder.flac);
        break;
    default:
        stb_vorbis_close(stream->decoder.ogg);
        break;
    }

    if (stream->sound != nullptr)
    {
        AUDIO.Release(stream->sound);
    }

    SDL_free(stream->piece);
    SDL_free(stream);
}

/// @brief Reads the next piece of a stream's file into its sound.
/// @return false once the file has ended, or cannot be read further.
static bool AudioReadPiece(AudioStream *stream)
{
    u64 wanted = (u64)((f64)stream->rate * AUDIO_PIECE);
    u64 frames = 0;

    switch (stream->format)
    {
    case AudioFormat_Wav:
        frames = drwav_read_pcm_frames_f32(&stream->decoder.wav, wanted, stream->piece);
        break;
    case AudioFormat_Mp3:
        frames = drmp3_read_pcm_frames_f32(&stream->decoder.mp3, wanted, stream->piece);
        break;
    case AudioFormat_Flac:
        frames = drflac_read_pcm_frames_f32(stream->decoder.flac, wanted, stream->piece);
        break;
    default:
        frames = (u64)stb_vorbis_get_samples_float_interleaved(stream->decoder.ogg, (int)stream->channels, stream->piece, (int)(wanted * stream->channels));
        break;
    }

    return frames > 0 && AUDIO.Queue(stream->sound, cs(stream->piece, (usz)frames * stream->channels * sizeof(f32))) && frames == wanted;
}

/// @brief Keeps each stream's sound AUDIO_AHEAD seconds ahead of what plays, and finishes the streams whose files have ended.
static void AudioFeed(void *data)
{
    (void)data;

    for (usz i = AUDIO.streamCount; i > 0; i--)
    {
        AudioStream *stream = AUDIO.streams[i - 1];
        bool more = true;

        while (more && AUDIO.Queued(stream->sound) < AUDIO_AHEAD)
        {
            more = AudioReadPiece(stream);
        }

        if (!more)
        {
            AUDIO.Finish(stream->sound);
            AudioClose(stream);
            AUDIO.streams[i - 1] = AUDIO.streams[--AUDIO.streamCount];
        }
    }

    if (AUDIO.streamCount == 0 && AUDIO.timer != nullptr)
    {
        ECSTimer_Stop(&AUDIO.timer);
    }
}

/// @brief Opens a sound file's decoder for a stream, by the file's extension.
/// @return false if the file cannot be read.
static bool AudioOpen(AudioStream *stream, const char *path)
{
    if (AudioEndsWith(path, ".wav") && drwav_init_file(&stream->decoder.wav, path, &AUDIO_WAV_ALLOCATION))
    {
        stream->format = AudioFormat_Wav;
        stream->channels = stream->decoder.wav.channels;
        stream->rate = stream->decoder.wav.sampleRate;
    }
    else if (AudioEndsWith(path, ".mp3") && drmp3_init_file(&stream->decoder.mp3, path, &AUDIO_MP3_ALLOCATION))
    {
        stream->format = AudioFormat_Mp3;
        stream->channels = stream->decoder.mp3.channels;
        stream->rate = stream->decoder.mp3.sampleRate;
    }
    else if (AudioEndsWith(path, ".flac") && (stream->decoder.flac = drflac_open_file(path, &AUDIO_FLAC_ALLOCATION)) != nullptr)
    {
        stream->format = AudioFormat_Flac;
        stream->channels = stream->decoder.flac->channels;
        stream->rate = stream->decoder.flac->sampleRate;
    }
    else if (AudioEndsWith(path, ".ogg") && (stream->decoder.ogg = stb_vorbis_open_filename(path, nullptr, nullptr)) != nullptr)
    {
        stb_vorbis_info info = stb_vorbis_get_info(stream->decoder.ogg);
        stream->format = AudioFormat_Ogg;
        stream->channels = (u32)info.channels;
        stream->rate = info.sample_rate;
    }
    else
    {
        return false;
    }

    return true;
}

/// @brief Opens a sound file to read a piece at a time into a new stream of mixer, which audio feeds while it plays.
/// @return The stream, or nullptr if the file cannot be read; the reason is logged.
static void *AudioStreamFile(const char *path)
{
    char *full = nullptr;

    // a relative path starts at the executable's folder
    if (SDL_asprintf(&full, "%s%s", ECSPath_IsAbsolute(path) ? "" : SDL_GetBasePath(), path) < 0)
    {
        return nullptr;
    }

    AudioStream *stream = SDL_calloc(1, sizeof(AudioStream));

    bool opened = stream != nullptr && AudioOpen(stream, full);

    // each failure says its own reason: no memory, or a file that cannot be read, which a sound without channels or rate is too
    if (!opened || stream->channels == 0 || stream->rate == 0)
    {
        ECS_LogError(AUDIO.plugin, "Cannot stream the sound '%s': %s", full, stream == nullptr ? "out of memory." : "not a WAV, MP3, FLAC or OGG file that can be read.");

        if (opened)
        {
            AudioClose(stream);
        }
        else
        {
            SDL_free(stream);
        }

        SDL_free(full);
        return nullptr;
    }

    stream->piece = SDL_malloc((usz)((f64)stream->rate * AUDIO_PIECE) * stream->channels * sizeof(f32));
    stream->sound = stream->piece == nullptr ? nullptr : AUDIO.NewStream((i32)stream->channels, (i32)stream->rate);
    AudioStream **streams = stream->sound == nullptr ? nullptr : SDL_realloc(AUDIO.streams, (AUDIO.streamCount + 1) * sizeof(AudioStream *));
    AUDIO.streams = streams != nullptr ? streams : AUDIO.streams;

    if (streams == nullptr || (AUDIO.timer == nullptr && ECSTimer_Start(AUDIO.plugin, &AUDIO.timer, AUDIO_FEED_INTERVAL, true, AudioFeed, nullptr) != SHUResult_Ok))
    {
        ECS_LogError(AUDIO.plugin, "Cannot stream the sound '%s': out of memory.", full);
        AudioClose(stream);
        SDL_free(full);
        return nullptr;
    }

    SDL_free(full);

    // audio keeps the stream while it feeds it, so the stream plays to its end even when no one uses its handle
    void *sound = stream->sound;
    AUDIO.Keep(sound);
    AUDIO.streams[AUDIO.streamCount++] = stream;
    AudioFeed(nullptr);
    return sound;
}

/// @brief Closes the files of the streams; the Destroy function of the plugin.
static void AudioQuit(void *data)
{
    (void)data;

    while (AUDIO.streamCount > 0)
    {
        AudioClose(AUDIO.streams[--AUDIO.streamCount]);
    }

    if (AUDIO.timer != nullptr)
    {
        ECSTimer_Stop(&AUDIO.timer);
    }

    SDL_free(AUDIO.streams);
    SDL_zero(AUDIO);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    AUDIO.plugin = plugin;
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&AUDIO.NewSound, "mixer.newSound", "handle<mixer.sound>(int channels, int rate, buffer samples)"));
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&AUDIO.NewStream, "mixer.newStream", "handle<mixer.sound>(int channels, int rate)"));
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&AUDIO.Queue, "mixer.queue", "bool(handle<mixer.sound> stream, buffer samples)"));
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&AUDIO.Queued, "mixer.queued", "double(handle<mixer.sound> stream)"));
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&AUDIO.Finish, "mixer.finish", "void(handle<mixer.sound> stream)"));
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&AUDIO.Keep, "mixer.keepSound", "void(handle<mixer.sound> sound)"));
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&AUDIO.Release, "mixer.releaseSound", "void(handle<mixer.sound> sound)"));

    const ECSPluginDesc desc = {.Destroy = AudioQuit};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc));
    SHU_ReturnResult(ECSService_RegisterFunction(plugin, "load", (ECSFunction)AudioLoad, "handle<mixer.sound>(string path)", "Read a sound file into a sound of mixer; nothing if it cannot be read"));
    return ECSService_RegisterFunction(plugin, "stream", (ECSFunction)AudioStreamFile, "handle<mixer.sound>(string path)",
                                       "Read a long sound file, such as music, a piece at a time into a stream of mixer while it plays; nothing if it cannot be read");
}
