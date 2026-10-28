// The audio standard plugin: reads sound files, WAV, MP3, FLAC and OGG, into sounds of the mixer plugin, which plays them (DESIGN 7).
// dr_wav, dr_mp3, dr_flac and stb_vorbis read the files a piece at a time: all of a file into a sound, or into a stream of mixer while it plays, for
// long files such as music.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#define STB_DS_IMPLEMENTATION
#include "stbSDL3.h"

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
typedef void *(*AudioNewSoundFunction)(i32 channels, i32 rate, SHUSlice samples);
typedef void *(*AudioNewStreamFunction)(i32 channels, i32 rate);
typedef bool (*AudioQueueFunction)(void *sound, SHUSlice samples);
typedef f64 (*AudioQueuedFunction)(void *sound);
typedef void (*AudioSoundFunction)(void *sound);

/// @brief The file formats audio reads.
typedef enum AudioFormat
{
    AudioFormat_Wav,
    AudioFormat_Mp3,
    AudioFormat_Flac,
    AudioFormat_Ogg,
} AudioFormat;

/// @brief An open sound file, which gives its samples a piece at a time.
typedef struct AudioDecoder
{
    AudioFormat format;
    u32 channels;
    u32 rate;
    union
    {
        drwav wav;
        drmp3 mp3;
        drflac *flac;
        stb_vorbis *ogg;
    } file;
} AudioDecoder;

/// @brief A file that is read into a stream of mixer while it plays.
typedef struct AudioStream
{
    void *sound; // the stream, which audio keeps until the file ends
    AudioDecoder decoder;
    f32 *piece; // room for the samples of AUDIO_PIECE seconds
} AudioStream;

static struct
{
    ECSPlugin plugin;
    AudioNewSoundFunction NewSound;
    AudioNewStreamFunction NewStream;
    AudioQueueFunction Queue;
    AudioQueuedFunction Queued;
    AudioSoundFunction Finish;
    AudioSoundFunction KeepSound;
    AudioSoundFunction ReleaseSound;
    AudioStream **streams; // stb_ds array
    ECSTimer timer;        // feeds the streams while there are any
} AUDIO = {0};

// dr_libs allocate with SDL's allocator, as the plugin does
static void *AudioAllocate(size_t size, void *data)
{
    (void)data;
    return SDL_malloc(size);
}

static void *AudioReallocate(void *memory, size_t size, void *data)
{
    (void)data;
    return SDL_realloc(memory, size);
}

static void AudioDeallocate(void *memory, void *data)
{
    (void)data;
    SDL_free(memory);
}

static const drwav_allocation_callbacks AUDIO_WAV_ALLOCATION = {nullptr, AudioAllocate, AudioReallocate, AudioDeallocate};
static const drmp3_allocation_callbacks AUDIO_MP3_ALLOCATION = {nullptr, AudioAllocate, AudioReallocate, AudioDeallocate};
static const drflac_allocation_callbacks AUDIO_FLAC_ALLOCATION = {nullptr, AudioAllocate, AudioReallocate, AudioDeallocate};

static bool AudioEndsWith(const char *text, const char *suffix)
{
    usz length = SDL_strlen(text);
    usz suffixLength = SDL_strlen(suffix);
    return length >= suffixLength && SDL_strcasecmp(text + length - suffixLength, suffix) == 0;
}

#pragma region Decoders

/// @brief Opens a sound file's decoder, by the file's extension.
/// @return false if the file cannot be read, or has no channels or rate.
static bool AudioOpenDecoder(AudioDecoder *retDecoder, const char *path)
{
    *retDecoder = (AudioDecoder){0};

    if (AudioEndsWith(path, ".wav") && drwav_init_file(&retDecoder->file.wav, path, &AUDIO_WAV_ALLOCATION))
    {
        retDecoder->format = AudioFormat_Wav;
        retDecoder->channels = retDecoder->file.wav.channels;
        retDecoder->rate = retDecoder->file.wav.sampleRate;
    }
    else if (AudioEndsWith(path, ".mp3") && drmp3_init_file(&retDecoder->file.mp3, path, &AUDIO_MP3_ALLOCATION))
    {
        retDecoder->format = AudioFormat_Mp3;
        retDecoder->channels = retDecoder->file.mp3.channels;
        retDecoder->rate = retDecoder->file.mp3.sampleRate;
    }
    else if (AudioEndsWith(path, ".flac") && (retDecoder->file.flac = drflac_open_file(path, &AUDIO_FLAC_ALLOCATION)) != nullptr)
    {
        retDecoder->format = AudioFormat_Flac;
        retDecoder->channels = retDecoder->file.flac->channels;
        retDecoder->rate = retDecoder->file.flac->sampleRate;
    }
    else if (AudioEndsWith(path, ".ogg") && (retDecoder->file.ogg = stb_vorbis_open_filename(path, nullptr, nullptr)) != nullptr)
    {
        stb_vorbis_info info = stb_vorbis_get_info(retDecoder->file.ogg);
        retDecoder->format = AudioFormat_Ogg;
        retDecoder->channels = (u32)info.channels;
        retDecoder->rate = info.sample_rate;
    }
    else
    {
        return false;
    }

    return true;
}

static void AudioCloseDecoder(AudioDecoder *decoder)
{
    switch (decoder->format)
    {
    case AudioFormat_Wav:
        drwav_uninit(&decoder->file.wav);
        break;
    case AudioFormat_Mp3:
        drmp3_uninit(&decoder->file.mp3);
        break;
    case AudioFormat_Flac:
        drflac_close(decoder->file.flac);
        break;
    case AudioFormat_Ogg:
        stb_vorbis_close(decoder->file.ogg);
        break;
    }
}

/// @brief Reads up to a number of frames of 32-bit float samples, frame after frame, the file's channels each.
/// @return How many frames were read; fewer than wanted once the file ends.
static u64 AudioReadFrames(AudioDecoder *decoder, u64 wanted, f32 *retSamples)
{
    switch (decoder->format)
    {
    case AudioFormat_Wav:
        return drwav_read_pcm_frames_f32(&decoder->file.wav, wanted, retSamples);
    case AudioFormat_Mp3:
        return drmp3_read_pcm_frames_f32(&decoder->file.mp3, wanted, retSamples);
    case AudioFormat_Flac:
        return drflac_read_pcm_frames_f32(decoder->file.flac, wanted, retSamples);
    case AudioFormat_Ogg:
        return (u64)stb_vorbis_get_samples_float_interleaved(decoder->file.ogg, (int)decoder->channels, retSamples, (int)(wanted * decoder->channels));
    }

    return 0;
}

/// @brief Opens a sound file's decoder, from a path whose relative form starts at the executable's folder.
/// @return false if the file cannot be read; the reason is logged, after what was being done.
static bool AudioOpenFile(AudioDecoder *retDecoder, const char *path, const char *doing)
{
    char *fullPath = nullptr;
    bool opened = ECSPath_GetFull(&fullPath, path) == SHUResult_Ok && AudioOpenDecoder(retDecoder, fullPath);

    if (opened && (retDecoder->channels == 0 || retDecoder->rate == 0))
    {
        AudioCloseDecoder(retDecoder);
        opened = false;
    }

    if (!opened)
    {
        ECS_LogError(AUDIO.plugin, "Cannot %s the sound '%s': not a WAV, MP3, FLAC or OGG file that can be read.", doing, fullPath != nullptr ? fullPath : path);
    }

    ECSPath_Free(fullPath);
    return opened;
}

#pragma endregion Decoders

/// @brief Reads a sound file whole into a new sound of the mixer plugin.
/// @return The sound, or nullptr if the file cannot be read; the reason is logged.
static void *AudioLoad(const char *path)
{
    AudioDecoder decoder;

    if (!AudioOpenFile(&decoder, path, "read"))
    {
        return nullptr;
    }

    // the samples are read a piece at a time to the file's end
    u64 wanted = (u64)((f64)decoder.rate * AUDIO_PIECE);
    usz pieceSamples = (usz)wanted * decoder.channels;
    f32 *samples = nullptr;
    u64 read = 0;

    do
    {
        usz before = arrlenu(samples);
        f32 *piece = arraddnptr(samples, pieceSamples);
        read = AudioReadFrames(&decoder, wanted, piece);
        arrsetlen(samples, before + (usz)read * decoder.channels);
    } while (read == wanted);

    AudioCloseDecoder(&decoder);

    // mixer copies the samples into its sound
    void *sound = AUDIO.NewSound((i32)decoder.channels, (i32)decoder.rate, cs(samples, arrlenu(samples) * sizeof(f32)));
    arrfree(samples);
    return sound;
}

/// @brief Closes a stream's file and lets go of its sound.
static void AudioFreeStream(AudioStream *stream)
{
    AudioCloseDecoder(&stream->decoder);

    if (stream->sound != nullptr)
    {
        AUDIO.ReleaseSound(stream->sound);
    }

    SDL_free(stream->piece);
    SDL_free(stream);
}

/// @brief Reads the next piece of a stream's file into its sound.
/// @return false once the file has ended, or cannot be read further.
static bool AudioReadPiece(AudioStream *stream)
{
    u64 wanted = (u64)((f64)stream->decoder.rate * AUDIO_PIECE);
    u64 frames = AudioReadFrames(&stream->decoder, wanted, stream->piece);
    return frames > 0 && AUDIO.Queue(stream->sound, cs(stream->piece, (usz)frames * stream->decoder.channels * sizeof(f32))) && frames == wanted;
}

/// @brief Keeps each stream's sound AUDIO_AHEAD seconds ahead of what plays, and finishes the streams whose files have ended.
static void AudioFeed(void *data)
{
    (void)data;

    for (usz i = arrlenu(AUDIO.streams); i > 0; i--)
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
            AudioFreeStream(stream);
            arrdelswap(AUDIO.streams, i - 1);
        }
    }

    if (arrlenu(AUDIO.streams) == 0 && AUDIO.timer != nullptr)
    {
        ECSTimer_Stop(&AUDIO.timer);
    }
}

/// @brief Opens a sound file to read a piece at a time into a new stream of mixer, which audio feeds while it plays.
/// @return The stream, or nullptr if the file cannot be read; the reason is logged.
static void *AudioStreamFile(const char *path)
{
    AudioStream *stream = SDL_calloc(1, sizeof(AudioStream));

    if (stream == nullptr)
    {
        ECS_LogError(AUDIO.plugin, "Cannot stream the sound '%s': out of memory.", path);
        return nullptr;
    }

    if (!AudioOpenFile(&stream->decoder, path, "stream"))
    {
        SDL_free(stream);
        return nullptr;
    }

    stream->piece = SDL_malloc((usz)((f64)stream->decoder.rate * AUDIO_PIECE) * stream->decoder.channels * sizeof(f32));
    stream->sound = stream->piece == nullptr ? nullptr : AUDIO.NewStream((i32)stream->decoder.channels, (i32)stream->decoder.rate);

    if (stream->sound == nullptr || (AUDIO.timer == nullptr && ECSTimer_Start(AUDIO.plugin, &AUDIO.timer, AUDIO_FEED_INTERVAL, true, AudioFeed, nullptr) != SHUResult_Ok))
    {
        ECS_LogError(AUDIO.plugin, "Cannot stream the sound '%s': out of memory.", path);
        AudioFreeStream(stream);
        return nullptr;
    }

    // audio keeps the stream while it feeds it, so the stream plays to its end even when no one uses its handle
    void *sound = stream->sound;
    AUDIO.KeepSound(sound);
    arrput(AUDIO.streams, stream);
    AudioFeed(nullptr);
    return sound;
}

/// @brief Closes the files of the streams; the Destroy function of the plugin.
static void AudioFree(void *data)
{
    (void)data;

    for (usz i = 0; i < arrlenu(AUDIO.streams); i++)
    {
        AudioFreeStream(AUDIO.streams[i]);
    }

    if (AUDIO.timer != nullptr)
    {
        ECSTimer_Stop(&AUDIO.timer);
    }

    arrfree(AUDIO.streams);
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
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&AUDIO.KeepSound, "mixer.keepSound", "void(handle<mixer.sound> sound)"));
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&AUDIO.ReleaseSound, "mixer.releaseSound", "void(handle<mixer.sound> sound)"));

    const ECSPluginDesc desc = {.Destroy = AudioFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc));
    SHU_ReturnResult(ECSService_RegisterFunction(plugin, "load", (ECSFunction)AudioLoad, "handle<mixer.sound>(string path)",
                                                 "Read a sound file into a sound of mixer; nothing if it cannot be read\n"
                                                 "@param path The file's path, a .wav, .mp3, .flac or .ogg file; a relative path starts at the executable's folder\n"
                                                 "@return A new sound with all the file's samples, or nothing if the file cannot be read; the reason is logged"));
    return ECSService_RegisterFunction(plugin, "stream", (ECSFunction)AudioStreamFile, "handle<mixer.sound>(string path)",
                                       "Read a long sound file, such as music, a piece at a time into a stream of mixer while it plays; nothing if it cannot be read\n"
                                       "@param path The file's path, a .wav, .mp3, .flac or .ogg file; a relative path starts at the executable's folder\n"
                                       "@return A new stream, which audio keeps and finishes at the file's end, or nothing if the file cannot be read; the reason is logged");
}
