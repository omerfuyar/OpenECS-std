// The audio standard plugin: reads sound files, WAV, MP3, FLAC and OGG, into sounds of the mixer plugin, which plays them (DESIGN 7).
// dr_wav, dr_mp3, dr_flac and stb_vorbis read the files whole.

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

/// @brief mixer.newSound, which makes a sound of the mixer plugin from samples.
typedef void *(*AudioNewFunction)(i32 channels, i32 rate, SHUSlice samples);

static struct
{
    ECSPlugin plugin;
    AudioNewFunction NewSound;
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
    if (SDL_asprintf(&full, "%s%s", path[0] == '/' ? "" : SDL_GetBasePath(), path) < 0)
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

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    AUDIO.plugin = plugin;
    SHU_ReturnResult(ECSService_GetFunction(plugin, (ECSFunction *)&AUDIO.NewSound, "mixer.newSound", "handle<mixer.sound>(int channels, int rate, buffer samples)"));
    return ECSService_RegisterFunction(plugin, "load", (ECSFunction)AudioLoad, "handle<mixer.sound>(string path)", "Read a sound file into a sound of mixer; nothing if it cannot be read");
}
