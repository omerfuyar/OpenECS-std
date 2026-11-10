// The video standard plugin: plays MPEG-1 video files with their MP2 sound, for intros, tutorials and previews (DESIGN 28).
// pl_mpeg's code is compiled into this file (DESIGN 1). A frame becomes an image of draw's, and the sound a stream of mixer's.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#define STB_DS_IMPLEMENTATION
#include "stbSDL3.h"

// pl_mpeg's code is compiled into this file, with SDL's allocator
#define PL_MPEG_IMPLEMENTATION
#define PLM_MALLOC(size) SDL_malloc(size)
#define PLM_FREE(memory) SDL_free(memory)
#define PLM_REALLOC(memory, size) SDL_realloc(memory, size)
#include "pl_mpeg.h"

#pragma region Source Only

// how often playing videos decode, in seconds, and how far their sound leads, so it never runs out between two decodes
#define VIDEO_TICK (1.0 / 120.0)
#define VIDEO_AUDIO_LEAD 0.15

/// @brief A video file: its decoder, the image of its frame, the stream of its sound, and whether it plays.
typedef struct VideoVideo
{
    plm_t *decoder;
    u8 *pixels;   // the frame in ARGB8888, native byte order
    void *image;  // draw.image of the frame; video keeps it, and no handle of it reaches Lua
    void *stream; // mixer.sound stream of the sound while it plays, or nullptr
    i32 voice;    // the stream's voice, or 0
    f32 volume;   // of the sound, 1 for its own
    bool playing;
    bool ended; // it played to its end, without looping
} VideoVideo;

static struct
{
    ECSPlugin plugin;
    VideoVideo **playing; // stb_ds array of the videos that play
    ECSTimer timer;       // decodes the playing videos while there are some
    u64 lastTicks;

    // draw's and mixer's functions
    void *(*NewImage)(i32 width, i32 height, SHUSlice pixels);
    bool (*SetImagePixels)(void *image, SHUSlice pixels);
    void (*FreeImage)(void *image);
    void (*DrawImage)(ECSSurface *surface, void *image, f32 x, f32 y, f32 width, f32 height);
    void *(*NewStream)(i32 channels, i32 rate);
    bool (*Queue)(void *stream, SHUSlice samples);
    void (*Finish)(void *stream);
    i32 (*Play)(void *sound, f32 volume, bool loop);
    void (*Stop)(i32 voice);
    void (*SetVolume)(i32 voice, f32 volume);
    void (*FreeSound)(void *sound);
} VIDEO = {0};

/// @brief Turns a decoded frame into the video's image.
static void VideoOnFrame(plm_t *decoder, plm_frame_t *frame, void *data)
{
    (void)decoder;
    VideoVideo *video = data;

    // ARGB8888 in native byte order is B, G, R, A in memory on little-endian systems, as Linux and Windows on x86_64 and aarch64 are
    plm_frame_to_bgra(frame, video->pixels, (int)frame->width * 4);
    (void)VIDEO.SetImagePixels(video->image, (SHUSlice){video->pixels, (usz)frame->width * (usz)frame->height * 4U});
}

/// @brief Adds decoded sound to the video's stream: two channels of float samples, frame after frame.
static void VideoOnSamples(plm_t *decoder, plm_samples_t *samples, void *data)
{
    (void)decoder;
    VideoVideo *video = data;

    if (video->stream != nullptr)
    {
        (void)VIDEO.Queue(video->stream, (SHUSlice){samples->interleaved, (usz)samples->count * 2U * sizeof(f32)});
    }
}

/// @brief Stops the video's sound: its voice stops, and its stream is freed with what waits in it.
static void VideoStopSound(VideoVideo *video)
{
    if (video->voice != 0)
    {
        VIDEO.Stop(video->voice);
    }

    if (video->stream != nullptr)
    {
        VIDEO.FreeSound(video->stream);
    }

    video->stream = nullptr;
    video->voice = 0;
}

/// @brief Starts the video's sound: a new stream, which its voice plays as video fills it.
static void VideoStartSound(VideoVideo *video)
{
    if (!plm_get_num_audio_streams(video->decoder) || !plm_get_audio_enabled(video->decoder))
    {
        return;
    }

    video->stream = VIDEO.NewStream(2, plm_get_samplerate(video->decoder));
    video->voice = video->stream == nullptr ? 0 : VIDEO.Play(video->stream, video->volume, false);
}

static void VideoRemovePlaying(VideoVideo *video)
{
    for (usz i = 0; i < arrlenu(VIDEO.playing); i++)
    {
        if (VIDEO.playing[i] == video)
        {
            arrdel(VIDEO.playing, i);
            break;
        }
    }

    if (arrlenu(VIDEO.playing) == 0)
    {
        ECSTimer_Stop(&VIDEO.timer);
    }
}

/// @brief Decodes each playing video by the time since the last tick; a video at its end stops, unless it loops.
static void VideoTick(void *data)
{
    (void)data;
    u64 now = SDL_GetTicksNS();
    f64 elapsed = (f64)(now - VIDEO.lastTicks) / 1e9;
    VIDEO.lastTicks = now;

    // a video may stop while it decodes, so the list is read backwards
    for (usz i = arrlenu(VIDEO.playing); i > 0; i--)
    {
        VideoVideo *video = VIDEO.playing[i - 1];
        plm_decode(video->decoder, SDL_min(elapsed, 0.25));

        if (plm_has_ended(video->decoder))
        {
            video->playing = false;
            video->ended = true;

            if (video->stream != nullptr)
            {
                VIDEO.Finish(video->stream);
            }

            VideoRemovePlaying(video);
        }
    }
}

static void VideoVideoDestroy(void *data)
{
    VideoVideo *video = data;
    VideoRemovePlaying(video);
    VideoStopSound(video);

    if (video->image != nullptr)
    {
        VIDEO.FreeImage(video->image);
    }

    plm_destroy(video->decoder);
    SDL_free(video->pixels);
    SDL_free(video);
}

static VideoVideo *VideoOpen(const char *path, const char **retError)
{
    *retError = nullptr;
    char *full = nullptr;
    plm_t *decoder = ECSPath_GetFull(&full, path) ? nullptr : plm_create_with_filename(full);
    SDL_free(full);

    if (decoder == nullptr || !plm_probe(decoder, (size_t)5000 * 1024) || plm_get_num_video_streams(decoder) == 0)
    {
        plm_destroy(decoder);
        *retError = "the file cannot be read, or holds no MPEG-1 video";
        return nullptr;
    }

    i32 width = plm_get_width(decoder);
    i32 height = plm_get_height(decoder);
    VideoVideo *video = SDL_calloc(1, sizeof(VideoVideo));
    u8 *pixels = SDL_malloc((usz)width * (usz)height * 4U);

    // pl_mpeg writes only the colours of a pixel, so the frame is opaque from the start
    if (pixels != nullptr)
    {
        SDL_memset(pixels, 0xFF, (usz)width * (usz)height * 4U);
    }

    void *image = pixels == nullptr ? nullptr : VIDEO.NewImage(width, height, (SHUSlice){pixels, (usz)width * (usz)height * 4U});

    if (video == nullptr || image == nullptr)
    {
        if (image != nullptr)
        {
            VIDEO.FreeImage(image);
        }

        SDL_free(pixels);
        SDL_free(video);
        plm_destroy(decoder);
        *retError = "out of memory";
        return nullptr;
    }

    *video = (VideoVideo){.decoder = decoder, .pixels = pixels, .image = image, .volume = 1.0f};
    plm_set_video_decode_callback(decoder, VideoOnFrame, video);
    plm_set_audio_decode_callback(decoder, VideoOnSamples, video);
    plm_set_audio_lead_time(decoder, VIDEO_AUDIO_LEAD);

    // the first frame shows before the video plays
    plm_frame_t *first = plm_decode_video(decoder);

    if (first != nullptr)
    {
        VideoOnFrame(decoder, first, video);
    }

    plm_rewind(decoder);
    return video;
}

static void VideoPlay(VideoVideo *video)
{
    if (video->playing)
    {
        return;
    }

    if (video->ended)
    {
        plm_rewind(video->decoder);
        video->ended = false;
    }

    VideoStartSound(video);

    if (arrlenu(VIDEO.playing) == 0)
    {
        VIDEO.lastTicks = SDL_GetTicksNS();

        if (ECSTimer_Start(VIDEO.plugin, &VIDEO.timer, VIDEO_TICK, true, VideoTick, nullptr))
        {
            VideoStopSound(video);
            return;
        }
    }

    arrput(VIDEO.playing, video);
    video->playing = true;
}

static void VideoPause(VideoVideo *video)
{
    if (video->playing)
    {
        video->playing = false;
        VideoStopSound(video);
        VideoRemovePlaying(video);
    }
}

static bool VideoPlaying(VideoVideo *video)
{
    return video->playing;
}

/// @brief Goes to a time; the frame there shows at once.
static bool VideoSeek(VideoVideo *video, f64 seconds)
{
    bool playing = video->playing;
    VideoStopSound(video);
    bool sought = plm_seek(video->decoder, SDL_clamp(seconds, 0.0, plm_get_duration(video->decoder)), true) != 0;
    video->ended = false;

    if (playing)
    {
        VideoStartSound(video);
    }

    return sought;
}

static f64 VideoTime(VideoVideo *video)
{
    return plm_get_time(video->decoder);
}

static f64 VideoDuration(VideoVideo *video)
{
    return plm_get_duration(video->decoder);
}

static void VideoSize(VideoVideo *video, i32 *retWidth, i32 *retHeight)
{
    *retWidth = plm_get_width(video->decoder);
    *retHeight = plm_get_height(video->decoder);
}

static void VideoSetLoop(VideoVideo *video, bool loop)
{
    plm_set_loop(video->decoder, loop);
}

static void VideoSetVolume(VideoVideo *video, f32 volume)
{
    video->volume = SDL_max(volume, 0.0f);

    if (video->voice != 0)
    {
        VIDEO.SetVolume(video->voice, video->volume);
    }
}

static void VideoDraw(VideoVideo *video, ECSSurface *surface, f32 x, f32 y, f32 width, f32 height)
{
    VIDEO.DrawImage(surface, video->image, x, y, width, height);
}

static void VideoFree(VideoVideo *video)
{
    ECSHandle_Free(VIDEO.plugin, "video", video);
}

static void VideoFreePlugin(void *data)
{
    (void)data;
    ECSTimer_Stop(&VIDEO.timer);
    arrfree(VIDEO.playing);
    SDL_zero(VIDEO);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    VIDEO.plugin = plugin;

    const struct
    {
        ECSFunction *function;
        const char *name;
        const char *signature;
    } needed[] = {
        {(ECSFunction *)&VIDEO.NewImage, "draw.newImage", "handle<draw.image>(int width, int height, buffer pixels)"},
        {(ECSFunction *)&VIDEO.SetImagePixels, "draw.setImagePixels", "bool(handle<draw.image> image, buffer pixels)"},
        {(ECSFunction *)&VIDEO.FreeImage, "draw.freeImage", "void(handle<draw.image> image)"},
        {(ECSFunction *)&VIDEO.DrawImage, "draw.image", "void(handle<ecs.surface> surface, handle<draw.image> image, float x, float y, float width, float height)"},
        {(ECSFunction *)&VIDEO.NewStream, "mixer.newStream", "handle<mixer.sound>(int channels, int rate)"},
        {(ECSFunction *)&VIDEO.Queue, "mixer.queue", "bool(handle<mixer.sound> stream, buffer samples)"},
        {(ECSFunction *)&VIDEO.Finish, "mixer.finish", "void(handle<mixer.sound> stream)"},
        {(ECSFunction *)&VIDEO.Play, "mixer.play", "int(handle<mixer.sound> sound, float volume, bool loop)"},
        {(ECSFunction *)&VIDEO.Stop, "mixer.stop", "void(int voice)"},
        {(ECSFunction *)&VIDEO.SetVolume, "mixer.volume", "void(int voice, float volume)"},
        {(ECSFunction *)&VIDEO.FreeSound, "mixer.freeSound", "void(handle<mixer.sound> sound)"},
    };

    for (usz i = 0; i < SDL_arraysize(needed); i++)
    {
        SHU_ReturnResult(ECSService_GetFunction(plugin, needed[i].function, needed[i].name, needed[i].signature));
    }

    const ECSPluginDesc desc = {.Destroy = VideoFreePlugin};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc));
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "video", VideoVideoDestroy));

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"open", (ECSFunction)VideoOpen, "handle<video.video>(string path, out string error)",
         "Open a video file: MPEG-1 video, with MP2 sound or none, in an MPEG program stream such as a .mpg file; its first frame shows at once\n"
         "@param path The file; a relative path starts at the executable's folder\n"
         "@param error Why it cannot be opened; nothing if it opened\n"
         "@return The video, paused at its start; nothing if it cannot be opened"},
        {"play", (ECSFunction)VideoPlay, "void(handle<video.video> video)",
         "Play a video from where it is, with its sound; a video at its end starts again\n"
         "@param video The video"},
        {"pause", (ECSFunction)VideoPause, "void(handle<video.video> video)",
         "Pause a video where it is\n"
         "@param video The video"},
        {"playing", (ECSFunction)VideoPlaying, "bool(handle<video.video> video)",
         "Tell whether a video plays\n"
         "@param video The video\n"
         "@return false once it is paused or has played to its end"},
        {"seek", (ECSFunction)VideoSeek, "bool(handle<video.video> video, double seconds)",
         "Go to a time of a video; its frame there shows at once\n"
         "@param video The video\n"
         "@param seconds The time from its start\n"
         "@return false if the time cannot be reached"},
        {"time", (ECSFunction)VideoTime, "double(handle<video.video> video)",
         "Give where a video is\n"
         "@param video The video\n"
         "@return The seconds from its start"},
        {"duration", (ECSFunction)VideoDuration, "double(handle<video.video> video)",
         "Give a video's length\n"
         "@param video The video\n"
         "@return Its length in seconds"},
        {"size", (ECSFunction)VideoSize, "void(handle<video.video> video, out int width, out int height)",
         "Give a video's size\n"
         "@param video The video\n"
         "@param width Its width in pixels\n"
         "@param height Its height in pixels"},
        {"setLoop", (ECSFunction)VideoSetLoop, "void(handle<video.video> video, bool loop)",
         "Make a video start again at its end, or stop there\n"
         "@param video The video\n"
         "@param loop true to start again"},
        {"setVolume", (ECSFunction)VideoSetVolume, "void(handle<video.video> video, float volume)",
         "Set the volume of a video's sound\n"
         "@param video The video\n"
         "@param volume 1 for its own, 0 for silence"},
        {"draw", (ECSFunction)VideoDraw, "void(handle<video.video> video, handle<ecs.surface> surface, float x, float y, float width, float height)",
         "Draw a video's frame stretched to a rectangle, with draw; a panel that shows a playing video draws every frame\n"
         "@param video The video\n"
         "@param surface The surface, from a panel's draw\n"
         "@param x Left, in layout units\n"
         "@param y Top, in layout units\n"
         "@param width Width, in layout units\n"
         "@param height Height, in layout units"},
        {"free", (ECSFunction)VideoFree, "void(handle<video.video> video)",
         "Close a video at once, and make its handles invalid\n"
         "@param video The video"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description));
    }

    // Lua calls these on a video, such as video:play(); C calls the functions themselves (OpenECS DESIGN 10.6)
    const char *methods[] = {"play", "pause", "playing", "seek", "time", "duration", "size", "setLoop", "setVolume", "draw", "free"};

    for (usz i = 0; i < SDL_arraysize(methods); i++)
    {
        SHU_ReturnResult(ECSHandle_AddMethod(plugin, "video", methods[i], methods[i]));
    }

    return SHUResult_Ok;
}
