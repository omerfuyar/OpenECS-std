// The mixer standard plugin: plays sounds on the default output, many at once (DESIGN 3).
// It knows sounds as samples; loaders, such as audio, turn files into them. SDL's audio streams play the sounds, and SDL mixes the streams that play at once.
// A stream is a sound whose samples a plugin gives while it plays, such as a synthesizer's.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#define STB_DS_IMPLEMENTATION
#include "stbSDL3.h"

#pragma region Source Only

/// @brief A sound: 32-bit float samples, kept whole, or a stream's, queued as they come. Its handle is mixer.sound; the voices that play it keep it alive too.
typedef struct MixerSound
{
    f32 *samples; // the frames one after another, each with a sample for every channel
    u64 frames;
    SDL_AudioSpec spec; // 32-bit float samples, the sound's channels and rate
    u32 users;          // its handle, unless it was freed, each voice that plays it, and each keepSound

    // a stream: the queue that the main thread fills and its voice empties, which SDL keeps thread-safe
    SDL_AudioStream *queue;
    SDL_AtomicInt finished; // no more samples come
    bool playing;           // a voice plays it; a stream plays in one voice at a time
} MixerSound;

/// @brief A filter of a voice's channel: a biquad of the Audio EQ Cookbook, its coefficients, and what it last heard and gave.
typedef struct MixerBiquad
{
    f32 b0, b1, b2, a1, a2;
    f32 x1, x2, y1, y2;
} MixerBiquad;

/// @brief An echo of a voice: what it played a delay ago comes back, softer each time. The audio thread uses it under the voice's stream's lock.
typedef struct MixerEcho
{
    f32 *samples; // the last delay's frames, each with a sample for every channel
    u64 length;   // the delay in frames
    u64 at;       // the frame to read and write next
    i32 channels;
    f32 feedback; // how loud each echo is next to the one before
    f32 mix;      // how loud the first echo is
    u64 ring;     // how many frames the echo rings for after the sound ends, until it is about a thousandth as loud
} MixerEcho;

/// @brief A sound that plays: an audio stream bound to the output, which asks for more of the sound as it plays.
typedef struct MixerVoice
{
    i32 id;
    MixerSound *sound;
    SDL_AudioStream *stream;
    u64 position; // the next frame to give the stream; only the audio thread changes it while the voice plays
    bool loop;
    SDL_AtomicInt finished; // the stream has been given the whole sound, and its echo has rung out
    bool ended;             // the audio thread has taken the sound's last frame
    u64 ringing;            // the audio thread: frames of echo still to give after the end, or SDL_MAX_UINT64 before the end
    MixerEcho *echo;        // nullptr for none
    SDL_AtomicInt pan;      // from -1000, left, to 1000, right
    i32 channels;           // the channels the voice gives its stream: a mono sound plays in stereo, so it can be panned
    f32 *taken;             // the audio thread's stb_ds array of the samples taken from a stream's queue
    f32 *processed;         // the audio thread's stb_ds array of the samples that panning, the filters and the echo changed

    // the low-pass and high-pass filters: their frequencies in hertz, 0 for none, which the main thread sets,
    // and for each channel the two filters, which the audio thread tunes to the frequencies it last saw
    SDL_AtomicInt lowpass;
    SDL_AtomicInt highpass;
    i32 tunedLowpass;
    i32 tunedHighpass;
    MixerBiquad *filters;
} MixerVoice;

/// @brief The delays of the reverb's filters in frames at 44100 Hz, Freeverb's: eight comb filters and four all-pass filters for each channel.
static const i32 MIXER_COMBS[] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const i32 MIXER_ALLPASSES[] = {556, 441, 341, 225};

/// @brief How many frames longer an odd channel's delays are, so that two channels sound apart.
#define MIXER_SPREAD 23

/// @brief A delay line of a filter: its samples, and where it reads and writes next.
typedef struct MixerDelay
{
    f32 *samples;
    i32 length;
    i32 at;
    f32 kept; // a comb filter's last output, which damping smooths
} MixerDelay;

/// @brief A reverb on the output: each channel's filters, for the output's channels and rate. The audio thread uses it while it is set.
typedef struct MixerReverb
{
    i32 channels;
    i32 rate;
    MixerDelay *delays; // for each channel, its comb filters, then its all-pass filters
    f32 *memory;        // the samples of every delay
    usz samples;        // how many samples memory holds
    bool resting;       // the audio thread saw a mix of 0, so the delays hold an old sound
    SDL_AtomicInt mix;  // how much of the reverb is added, in thousandths; 0 adds none
    SDL_AtomicInt size; // the room's size, in thousandths: a larger room rings longer
} MixerReverb;

static struct
{
    ECSPlugin plugin;
    MixerReverb *reverb; // nullptr until a reverb is set
    MixerVoice **voices; // stb_ds array
    i32 nextId;
    SDL_AudioDeviceID device; // 0 until the first sound plays
    bool audioStarted;
} MIXER = {0};

#pragma region Sounds

/// @brief Lets go of a sound, and frees it once nothing uses it. It is the Destroy function of the handle type mixer.sound, whose handle uses the
/// sound, and mixer.releaseSound.
static void MixerReleaseSound(void *object)
{
    MixerSound *sound = object;

    if (--sound->users == 0)
    {
        SDL_DestroyAudioStream(sound->queue);
        SDL_free(sound->samples);
        SDL_free(sound);
    }
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

    if (sound == nullptr)
    {
        return nullptr;
    }

    if (samples.size > 0)
    {
        sound->samples = SDL_malloc(samples.size);

        if (sound->samples == nullptr)
        {
            SDL_free(sound);
            return nullptr;
        }

        SDL_memcpy(sound->samples, samples.data, samples.size);
    }

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

/// @brief Keeps a sound alive for a plugin that uses it after giving its handle away, such as one that feeds a stream, until releaseSound.
static void MixerKeepSound(MixerSound *sound)
{
    sound->users++;
}

/// @brief Lets go of a sound now; it is freed once the voices that play it end.
static void MixerFreeSound(MixerSound *sound)
{
    ECSHandle_Free(MIXER.plugin, "sound", sound);
}

#pragma endregion Sounds

#pragma region Audio thread

/// @brief Makes a buffer of the audio thread, an stb_ds array, hold at least a number of floats.
/// @return The buffer.
static f32 *MixerReserve(f32 **buffer, usz floats)
{
    if (arrlenu(*buffer) < floats)
    {
        arrsetlen(*buffer, floats);
    }

    return *buffer;
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
        f32 *taken = MixerReserve(&voice->taken, (usz)wanted * (usz)sound->spec.channels);
        int got = SDL_GetAudioStreamData(sound->queue, taken, (int)(wanted * frameSize));

        // a stream ends once it is finished and its queue is empty; until then, a voice that runs out waits for more
        if (got <= 0 && SDL_GetAtomicInt(&sound->finished) && SDL_GetAudioStreamAvailable(sound->queue) == 0)
        {
            voice->ended = true;
        }

        *retCount = got > 0 ? (u64)got / frameSize : 0;
        return *retCount > 0 ? taken : nullptr;
    }

    if (voice->position >= sound->frames)
    {
        if (!voice->loop || sound->frames == 0)
        {
            voice->ended = true;
            return nullptr;
        }

        voice->position = 0;
    }

    *retCount = SDL_min(sound->frames - voice->position, wanted);
    const f32 *frames = sound->samples + voice->position * (u64)sound->spec.channels;
    voice->position += *retCount;
    return frames;
}

/// @brief A sample that has faded to almost nothing becomes 0, as in Freeverb: numbers that small are denormal, which many processors work on far slower.
static f32 MixerFlushDenormal(f32 sample)
{
    return SDL_fabsf(sample) < 1e-20f ? 0.0f : sample;
}

/// @brief Sets a filter's coefficients, divided by back0 as the Audio EQ Cookbook does, and keeps what it heard.
/// @param forward0, forward1, forward2 The cookbook's b0, b1 and b2.
/// @param back0, back1, back2 The cookbook's a0, a1 and a2.
static void MixerSetCoefficients(MixerBiquad *filter, f32 forward0, f32 forward1, f32 forward2, f32 back0, f32 back1, f32 back2)
{
    filter->b0 = forward0 / back0;
    filter->b1 = forward1 / back0;
    filter->b2 = forward2 / back0;
    filter->a1 = back1 / back0;
    filter->a2 = back2 / back0;
}

/// @brief The angle of a filter's frequency, for the Audio EQ Cookbook. A frequency at or past half the rate cannot be filtered, so it is kept just below.
static f32 MixerAngle(i32 hertz, i32 rate)
{
    return 2.0f * SDL_PI_F * SDL_min((f32)hertz, 0.49f * (f32)rate) / (f32)rate;
}

// the filters are Butterworth filters of the Audio EQ Cookbook, whose Q is 1/sqrt(2), for no peak at the edge; at 0 hertz, they let everything through

/// @brief Tunes a filter to cut the sounds above a frequency.
static void MixerTuneLowpass(MixerBiquad *filter, i32 hertz, i32 rate)
{
    if (hertz <= 0)
    {
        MixerSetCoefficients(filter, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
        return;
    }

    f32 angle = MixerAngle(hertz, rate);
    f32 alpha = SDL_sinf(angle) / (2.0f * 0.70710678f);
    f32 cosine = SDL_cosf(angle);
    f32 edge = (1.0f - cosine) / 2.0f;
    MixerSetCoefficients(filter, edge, 2.0f * edge, edge, 1.0f + alpha, -2.0f * cosine, 1.0f - alpha);
}

/// @brief Tunes a filter to cut the sounds below a frequency.
static void MixerTuneHighpass(MixerBiquad *filter, i32 hertz, i32 rate)
{
    if (hertz <= 0)
    {
        MixerSetCoefficients(filter, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
        return;
    }

    f32 angle = MixerAngle(hertz, rate);
    f32 alpha = SDL_sinf(angle) / (2.0f * 0.70710678f);
    f32 cosine = SDL_cosf(angle);
    f32 edge = (1.0f + cosine) / 2.0f;
    MixerSetCoefficients(filter, edge, -2.0f * edge, edge, 1.0f + alpha, -2.0f * cosine, 1.0f - alpha);
}

/// @brief Runs a voice's frames through its filters, in place, retuning them when their frequencies changed.
static void MixerFilter(MixerVoice *voice, f32 *frames, u64 count)
{
    i32 lowpass = SDL_GetAtomicInt(&voice->lowpass);
    i32 highpass = SDL_GetAtomicInt(&voice->highpass);
    i32 channels = voice->channels;

    if (lowpass != voice->tunedLowpass || highpass != voice->tunedHighpass)
    {
        // filters that were off forget what they heard before, so turning them on does not click
        if (voice->tunedLowpass == 0 && voice->tunedHighpass == 0)
        {
            SDL_memset(voice->filters, 0, (usz)channels * 2 * sizeof(MixerBiquad));
        }

        for (i32 channel = 0; channel < channels; channel++)
        {
            MixerTuneLowpass(&voice->filters[2 * (usz)channel], lowpass, voice->sound->spec.freq);
            MixerTuneHighpass(&voice->filters[2 * (usz)channel + 1], highpass, voice->sound->spec.freq);
        }

        voice->tunedLowpass = lowpass;
        voice->tunedHighpass = highpass;
    }

    for (u64 i = 0; i < count; i++)
    {
        for (i32 channel = 0; channel < channels; channel++)
        {
            f32 sample = frames[i * (u64)channels + (u64)channel];

            // the low-pass filter, then the high-pass filter
            for (i32 stage = 0; stage < 2; stage++)
            {
                MixerBiquad *filter = &voice->filters[2 * channel + stage];
                f32 output = filter->b0 * sample + filter->b1 * filter->x1 + filter->b2 * filter->x2 - filter->a1 * filter->y1 - filter->a2 * filter->y2;
                filter->x2 = filter->x1;
                filter->x1 = sample;
                // a filter that hears silence fades toward 0 through denormal numbers, so its memory is flushed as the reverb's is
                filter->y2 = filter->y1;
                filter->y1 = MixerFlushDenormal(output);
                sample = output;
            }

            frames[i * (u64)channels + (u64)channel] = sample;
        }
    }
}

/// @brief Adds a voice's echo to its frames, in place.
static void MixerEchoProcess(MixerEcho *echo, f32 *frames, u64 count)
{
    for (u64 i = 0; i < count; i++)
    {
        f32 *delayed = echo->samples + echo->at * (u64)echo->channels;

        for (i32 channel = 0; channel < echo->channels; channel++)
        {
            f32 sample = frames[i * (u64)echo->channels + (u64)channel];
            f32 back = delayed[channel];
            frames[i * (u64)echo->channels + (u64)channel] = sample + echo->mix * back;
            delayed[channel] = MixerFlushDenormal(sample + echo->feedback * back);
        }

        echo->at = echo->at + 1 == echo->length ? 0 : echo->at + 1;
    }
}

/// @brief Pans frames of one or two channels into two: the left and right channels take a share of the balance; at the middle, both keep their whole gain.
static void MixerPanFrames(const f32 *frames, i32 inputChannels, f32 *retPanned, u64 count, f32 pan)
{
    f32 leftGain = SDL_min(1.0f, 1.0f - pan);
    f32 rightGain = SDL_min(1.0f, 1.0f + pan);

    for (u64 i = 0; i < count; i++)
    {
        f32 left = frames[i * (u64)inputChannels];
        f32 right = inputChannels == 1 ? left : frames[i * (u64)inputChannels + 1];
        retPanned[2 * i] = left * leftGain;
        retPanned[2 * i + 1] = right * rightGain;
    }
}

/// @brief Pans, filters and echoes frames of a voice's sound, in the voice's channels.
/// @return The frames as they are, when nothing changes them, or the changed frames in the voice's room for them.
static const f32 *MixerProcess(MixerVoice *voice, const f32 *frames, u64 count)
{
    i32 inputChannels = voice->sound->spec.channels;
    f32 pan = (f32)SDL_GetAtomicInt(&voice->pan) / 1000.0f;
    bool panned = inputChannels != voice->channels || (voice->channels == 2 && pan != 0.0f);
    bool filtered = voice->tunedLowpass != 0 || voice->tunedHighpass != 0 || SDL_GetAtomicInt(&voice->lowpass) != 0 || SDL_GetAtomicInt(&voice->highpass) != 0;

    if (!panned && !filtered && voice->echo == nullptr)
    {
        return frames;
    }

    f32 *processed = MixerReserve(&voice->processed, (usz)count * (usz)voice->channels);

    if (panned)
    {
        MixerPanFrames(frames, inputChannels, processed, count, pan);
    }
    else
    {
        SDL_memcpy(processed, frames, (usz)count * (usz)voice->channels * sizeof(f32));
    }

    if (filtered)
    {
        MixerFilter(voice, processed, count);
    }

    if (voice->echo != nullptr)
    {
        MixerEchoProcess(voice->echo, processed, count);
    }

    return processed;
}

/// @brief Gives a voice's stream up to a number of frames of its echo, which rings out over silence after its sound has ended.
/// @return How many frames it gave; 0 once the echo has rung out, which finishes the voice.
static u64 MixerGiveRingOut(MixerVoice *voice, SDL_AudioStream *stream, u64 wanted)
{
    if (voice->ringing == SDL_MAX_UINT64)
    {
        voice->ringing = voice->echo != nullptr ? voice->echo->ring : 0;
    }

    u64 count = SDL_min(wanted, voice->ringing);

    if (count == 0 || voice->echo == nullptr)
    {
        SDL_SetAtomicInt(&voice->finished, 1);
        return 0;
    }

    usz floats = (usz)count * (usz)voice->channels;
    f32 *silence = MixerReserve(&voice->processed, floats);
    SDL_memset(silence, 0, floats * sizeof(f32));
    MixerEchoProcess(voice->echo, silence, count);
    SDL_PutAudioStreamData(stream, silence, (int)(floats * sizeof(f32)));
    voice->ringing -= count;
    return count;
}

/// @brief Gives a voice's stream up to a number of frames of its sound, panned, filtered and echoed, or of its echo once the sound has ended.
/// @return How many frames it gave; 0 when there are none now.
static u64 MixerGiveFrames(MixerVoice *voice, SDL_AudioStream *stream, u64 wanted)
{
    u64 count = 0;
    const f32 *frames = voice->ended ? nullptr : MixerTake(voice, wanted, &count);

    if (frames == nullptr)
    {
        return voice->ended ? MixerGiveRingOut(voice, stream, wanted) : 0;
    }

    const f32 *processed = MixerProcess(voice, frames, count);
    SDL_PutAudioStreamData(stream, processed, (int)(count * (u64)voice->channels * sizeof(f32)));
    return count;
}

/// @brief Gives a voice's stream more of its sound; SDL calls it on its audio thread when the stream runs low.
static void SDLCALL MixerFeed(void *data, SDL_AudioStream *stream, int needed, int total)
{
    (void)total;
    MixerVoice *voice = data;
    usz frameSize = (usz)voice->channels * sizeof(f32);

    while (needed > 0 && !SDL_GetAtomicInt(&voice->finished))
    {
        u64 given = MixerGiveFrames(voice, stream, (u64)needed / frameSize + 1);

        if (given == 0)
        {
            break;
        }

        needed -= (int)(given * frameSize);
    }

    // a stream that converts the rate keeps its last frames back until it is flushed, so a voice that has ended flushes them, and its stream empties
    if (SDL_GetAtomicInt(&voice->finished))
    {
        SDL_FlushAudioStream(stream);
    }
}

#pragma endregion Audio thread

#pragma region Voices

static void MixerFreeEcho(MixerEcho *echo)
{
    if (echo != nullptr)
    {
        SDL_free(echo->samples);
        SDL_free(echo);
    }
}

static void MixerFreeVoice(MixerVoice *voice)
{
    SDL_DestroyAudioStream(voice->stream);
    voice->sound->playing = false;
    arrfree(voice->taken);
    arrfree(voice->processed);
    SDL_free(voice->filters);
    MixerFreeEcho(voice->echo);
    MixerReleaseSound(voice->sound);
    SDL_free(voice);
}

/// @brief Forgets the voices whose sound has ended and whose stream has played it all.
static void MixerFreeFinishedVoices(void)
{
    for (usz i = arrlenu(MIXER.voices); i > 0; i--)
    {
        MixerVoice *voice = MIXER.voices[i - 1];

        if (SDL_GetAtomicInt(&voice->finished) && SDL_GetAudioStreamQueued(voice->stream) == 0)
        {
            MixerFreeVoice(voice);
            arrdelswap(MIXER.voices, i - 1);
        }
    }
}

static MixerVoice *MixerFindVoice(i32 id)
{
    for (usz i = 0; i < arrlenu(MIXER.voices); i++)
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
static bool MixerOpenOutput(void)
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
    MixerFreeFinishedVoices();

    // a stream's samples go to one voice, so it plays in one voice at a time
    if ((sound->queue != nullptr && sound->playing) || !MixerOpenOutput())
    {
        return 0;
    }

    MixerVoice *voice = SDL_calloc(1, sizeof(MixerVoice));

    if (voice == nullptr)
    {
        return 0;
    }

    // a mono sound plays in stereo, so it can be panned
    *voice = (MixerVoice){.id = MIXER.nextId + 1, .ringing = SDL_MAX_UINT64, .sound = sound, .loop = loop && sound->queue == nullptr, .channels = sound->spec.channels == 1 ? 2 : sound->spec.channels};
    SDL_AudioSpec spec = {.format = SDL_AUDIO_F32, .channels = voice->channels, .freq = sound->spec.freq};
    voice->filters = SDL_calloc((usz)voice->channels * 2, sizeof(MixerBiquad));
    voice->stream = voice->filters == nullptr ? nullptr : SDL_CreateAudioStream(&spec, nullptr);

    if (voice->stream == nullptr || !SDL_SetAudioStreamGain(voice->stream, SDL_max(0.0f, volume)) || !SDL_SetAudioStreamGetCallback(voice->stream, MixerFeed, voice) || !SDL_BindAudioStream(MIXER.device, voice->stream))
    {
        ECS_LogError(MIXER.plugin, "Cannot play a sound: %s", SDL_GetError());
        SDL_DestroyAudioStream(voice->stream);
        SDL_free(voice->filters);
        SDL_free(voice);
        return 0;
    }

    // the voice keeps the sound alive while it plays, also when its handle lets go of it
    MIXER.nextId = voice->id;
    sound->users++;
    sound->playing = true;
    arrput(MIXER.voices, voice);
    return voice->id;
}

/// @brief Ends a voice now, without what its stream has not played; it is freed with the finished voices.
static void MixerEndVoice(MixerVoice *voice)
{
    SDL_SetAtomicInt(&voice->finished, 1);
    SDL_ClearAudioStream(voice->stream);
}

static void MixerStop(i32 id)
{
    MixerVoice *voice = MixerFindVoice(id);

    if (voice != nullptr)
    {
        MixerEndVoice(voice);
    }

    MixerFreeFinishedVoices();
}

static void MixerStopAll(void)
{
    for (usz i = 0; i < arrlenu(MIXER.voices); i++)
    {
        MixerEndVoice(MIXER.voices[i]);
    }

    MixerFreeFinishedVoices();
}

static bool MixerPlaying(i32 id)
{
    MixerFreeFinishedVoices();
    return MixerFindVoice(id) != nullptr;
}

static void MixerSetVolume(i32 id, f32 volume)
{
    MixerVoice *voice = MixerFindVoice(id);

    if (voice != nullptr)
    {
        SDL_SetAudioStreamGain(voice->stream, SDL_max(0.0f, volume));
    }
}

static void MixerSetPan(i32 id, f32 pan)
{
    MixerVoice *voice = MixerFindVoice(id);

    if (voice != nullptr)
    {
        SDL_SetAtomicInt(&voice->pan, (int)SDL_roundf(SDL_clamp(pan, -1.0f, 1.0f) * 1000.0f));
    }
}

/// @brief Filters a voice: lowpass cuts the sounds above a frequency in hertz, and highpass those below one; 0 turns a filter off.
static void MixerSetFilter(i32 id, f32 lowpass, f32 highpass)
{
    MixerVoice *voice = MixerFindVoice(id);

    if (voice != nullptr)
    {
        SDL_SetAtomicInt(&voice->lowpass, (int)SDL_clamp(lowpass, 0.0f, 1000000.0f));
        SDL_SetAtomicInt(&voice->highpass, (int)SDL_clamp(highpass, 0.0f, 1000000.0f));
    }
}

/// @brief Makes an echo for a voice: what it played a delay ago, in seconds up to 2, comes back at mix, and each echo comes back again at feedback
/// times as loud, up to 0.95. It rings until it is about a thousandth as loud, or for one delay without feedback, and 30 seconds at most.
/// @return The echo, or nullptr if out of memory; the reason is logged.
static MixerEcho *MixerNewEcho(const MixerVoice *voice, u64 length, f32 feedback, f32 mix)
{
    MixerEcho *echo = SDL_calloc(1, sizeof(MixerEcho));
    f32 *samples = SDL_calloc((usz)length * (usz)voice->channels, sizeof(f32));

    if (echo == nullptr || samples == nullptr)
    {
        ECS_LogError(MIXER.plugin, "Cannot add an echo: out of memory");
        SDL_free(echo);
        SDL_free(samples);
        return nullptr;
    }

    f32 back = SDL_clamp(feedback, 0.0f, 0.95f);
    f32 repeats = back > 0.0f ? SDL_ceilf(SDL_logf(0.001f) / SDL_logf(back)) : 0.0f;
    f32 ring = SDL_min((repeats + 1.0f) * (f32)length, 30.0f * (f32)voice->sound->spec.freq);
    *echo = (MixerEcho){.samples = samples, .length = length, .channels = voice->channels, .feedback = back, .mix = SDL_clamp(mix, 0.0f, 1.0f), .ring = (u64)ring};
    return echo;
}

/// @brief Gives a voice an echo; a delay or a mix of 0 takes the echo away. A voice with an echo rings on after its sound ends.
static void MixerSetEcho(i32 id, f32 delay, f32 feedback, f32 mix)
{
    MixerVoice *voice = MixerFindVoice(id);

    if (voice == nullptr)
    {
        return;
    }

    MixerEcho *echo = nullptr;
    u64 length = delay > 0.0f && mix > 0.0f ? (u64)SDL_ceilf(SDL_min(delay, 2.0f) * (f32)voice->sound->spec.freq) : 0;

    if (length > 0)
    {
        echo = MixerNewEcho(voice, length, feedback, mix);

        if (echo == nullptr)
        {
            return;
        }
    }

    // the audio thread feeds the voice under its stream's lock, so the echo changes while it does not
    SDL_LockAudioStream(voice->stream);
    MixerEcho *old = voice->echo;
    voice->echo = echo;
    SDL_UnlockAudioStream(voice->stream);
    MixerFreeEcho(old);
}

#pragma endregion Voices

#pragma region Reverb

/// @brief Makes a reverb for an output's channels and rate, with every delay silent.
/// @return The reverb, to free with MixerFreeReverb, or nullptr if out of memory.
static MixerReverb *MixerNewReverb(i32 channels, i32 rate)
{
    const usz filters = SDL_arraysize(MIXER_COMBS) + SDL_arraysize(MIXER_ALLPASSES);
    MixerReverb *reverb = SDL_calloc(1, sizeof(MixerReverb));
    MixerDelay *delays = SDL_calloc((usz)channels * filters, sizeof(MixerDelay));

    if (reverb == nullptr || delays == nullptr)
    {
        SDL_free(reverb);
        SDL_free(delays);
        return nullptr;
    }

    // the delays are as long in seconds at any rate
    usz total = 0;

    for (i32 channel = 0; channel < channels; channel++)
    {
        for (usz i = 0; i < filters; i++)
        {
            i32 frames = i < SDL_arraysize(MIXER_COMBS) ? MIXER_COMBS[i] : MIXER_ALLPASSES[i - SDL_arraysize(MIXER_COMBS)];
            MixerDelay *delay = &delays[(usz)channel * filters + i];
            delay->length = SDL_max(1, (i32)((i64)(frames + (channel % 2) * MIXER_SPREAD) * rate / 44100));
            total += (usz)delay->length;
        }
    }

    f32 *memory = SDL_calloc(total, sizeof(f32));

    if (memory == nullptr)
    {
        SDL_free(delays);
        SDL_free(reverb);
        return nullptr;
    }

    for (usz i = 0, offset = 0; i < (usz)channels * filters; i++)
    {
        delays[i].samples = memory + offset;
        offset += (usz)delays[i].length;
    }

    *reverb = (MixerReverb){.channels = channels, .rate = rate, .delays = delays, .memory = memory, .samples = total};
    return reverb;
}

static void MixerFreeReverb(MixerReverb *reverb)
{
    if (reverb != nullptr)
    {
        SDL_free(reverb->memory);
        SDL_free(reverb->delays);
        SDL_free(reverb);
    }
}

/// @brief Adds the reverb to the output's mixed samples, frame after frame, in place.
static void MixerReverbProcess(MixerReverb *reverb, f32 *samples, usz frames)
{
    const usz combs = SDL_arraysize(MIXER_COMBS);
    const usz filters = combs + SDL_arraysize(MIXER_ALLPASSES);
    f32 mix = (f32)SDL_GetAtomicInt(&reverb->mix) / 1000.0f;

    // Freeverb's tuning: a room's size sets the combs' feedback, and the damping softens the high sounds as they ring
    f32 feedback = 0.7f + 0.28f * (f32)SDL_GetAtomicInt(&reverb->size) / 1000.0f;
    const f32 damp = 0.2f;
    const f32 gain = 0.015f;
    const f32 wet = mix; // a mix of 1 is Freeverb's own wet level

    if (mix <= 0.0f)
    {
        reverb->resting = true;
        return;
    }

    // a reverb turned on again starts silent, rather than with what rang when it was turned off
    if (reverb->resting)
    {
        SDL_memset(reverb->memory, 0, reverb->samples * sizeof(f32));

        for (usz i = 0; i < (usz)reverb->channels * filters; i++)
        {
            reverb->delays[i].kept = 0.0f;
        }

        reverb->resting = false;
    }

    for (usz frame = 0; frame < frames; frame++)
    {
        f32 *sample = samples + frame * (usz)reverb->channels;
        f32 input = 0.0f;

        // every channel's filters hear the channels together, as Freeverb's hear both of a stereo pair
        for (i32 channel = 0; channel < reverb->channels; channel++)
        {
            input += sample[channel];
        }

        input *= gain * 2.0f / (f32)reverb->channels;

        for (i32 channel = 0; channel < reverb->channels; channel++)
        {
            MixerDelay *delays = reverb->delays + (usz)channel * filters;
            f32 output = 0.0f;

            for (usz i = 0; i < combs; i++)
            {
                MixerDelay *comb = &delays[i];
                f32 delayed = comb->samples[comb->at];
                comb->kept = MixerFlushDenormal(delayed * (1.0f - damp) + comb->kept * damp);
                comb->samples[comb->at] = MixerFlushDenormal(input + comb->kept * feedback);
                comb->at = comb->at + 1 == comb->length ? 0 : comb->at + 1;
                output += delayed;
            }

            for (usz i = combs; i < filters; i++)
            {
                MixerDelay *allpass = &delays[i];
                f32 delayed = allpass->samples[allpass->at];
                allpass->samples[allpass->at] = MixerFlushDenormal(output + delayed * 0.5f);
                allpass->at = allpass->at + 1 == allpass->length ? 0 : allpass->at + 1;
                output = delayed - output;
            }

            sample[channel] += output * wet;
        }
    }
}

/// @brief Adds the reverb to what the output plays; SDL calls it on its audio thread after it mixes the voices.
static void SDLCALL MixerPostmix(void *data, const SDL_AudioSpec *spec, float *buffer, int size)
{
    MixerReverb *reverb = data;

    // an output that changed, such as to another device, plays without the reverb until it is set again
    if (spec->channels == reverb->channels && spec->freq == reverb->rate && size > 0)
    {
        MixerReverbProcess(reverb, buffer, (usz)size / sizeof(f32) / (usz)reverb->channels);
    }
}

/// @brief Sets the reverb on the output: mix is how much of it is added, from 0, none, to 1, and size the room's, from 0 to 1.
/// @return false if there is no output, or memory for the reverb.
static bool MixerSetReverb(f32 mix, f32 size)
{
    SDL_AudioSpec spec = {0};

    if (!MixerOpenOutput() || !SDL_GetAudioDeviceFormat(MIXER.device, &spec, nullptr))
    {
        return false;
    }

    // a reverb for another output's format is replaced; SDL holds the output while it changes the callback, so the old one is not in use after
    if (MIXER.reverb == nullptr || MIXER.reverb->channels != spec.channels || MIXER.reverb->rate != spec.freq)
    {
        MixerReverb *reverb = MixerNewReverb(spec.channels, spec.freq);

        if (reverb == nullptr || !SDL_SetAudioPostmixCallback(MIXER.device, MixerPostmix, reverb))
        {
            ECS_LogError(MIXER.plugin, "Cannot add a reverb: %s", reverb == nullptr ? "out of memory" : SDL_GetError());
            MixerFreeReverb(reverb);
            return false;
        }

        MixerFreeReverb(MIXER.reverb);
        MIXER.reverb = reverb;
    }

    SDL_SetAtomicInt(&MIXER.reverb->mix, (int)SDL_roundf(SDL_clamp(mix, 0.0f, 1.0f) * 1000.0f));
    SDL_SetAtomicInt(&MIXER.reverb->size, (int)SDL_roundf(SDL_clamp(size, 0.0f, 1.0f) * 1000.0f));
    return true;
}

#pragma endregion Reverb

/// @brief Stops every voice and closes the output; the Destroy function of the plugin.
static void MixerFree(void *data)
{
    (void)data;

    for (usz i = 0; i < arrlenu(MIXER.voices); i++)
    {
        MixerFreeVoice(MIXER.voices[i]);
    }

    // the reverb is in use until the output closes
    if (MIXER.device != 0)
    {
        SDL_CloseAudioDevice(MIXER.device);
    }

    MixerFreeReverb(MIXER.reverb);

    if (MIXER.audioStarted)
    {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    arrfree(MIXER.voices);
    SDL_zero(MIXER);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    MIXER.plugin = plugin;

    // a plugin whose Init fails is not destroyed, so it cleans up here
    const ECSPluginDesc desc = {.Destroy = MixerFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc), MixerFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "sound", MixerReleaseSound), MixerFree(nullptr););

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"newSound", (ECSFunction)MixerNewSound, "handle<mixer.sound>(int channels, int rate, buffer samples)",
         "Make a sound from 32-bit float samples, frame after frame, at a rate in frames a second\n"
         "@param channels The number of channels, above 0; each frame has a sample for each\n"
         "@param rate The frames a second, above 0\n"
         "@param samples The 32-bit float samples, frame after frame, in whole frames; they are copied\n"
         "@return The sound, or nothing if channels or rate is not above 0, or the samples are not whole frames"},
        {"newStream", (ECSFunction)MixerNewStream, "handle<mixer.sound>(int channels, int rate)",
         "Make a stream: a sound whose 32-bit float samples are given while it plays, with queue\n"
         "@param channels The number of channels, above 0; each frame has a sample for each\n"
         "@param rate The frames a second, above 0\n"
         "@return The stream, or nothing if channels or rate is not above 0"},
        {"queue", (ECSFunction)MixerQueue, "bool(handle<mixer.sound> stream, buffer samples)",
         "Add samples to the end of a stream; false if it is not a stream, it is finished, or the samples are not whole frames\n"
         "@param stream The stream, as newStream gives it\n"
         "@param samples The 32-bit float samples, frame after frame, with the stream's channels; they are copied\n"
         "@return True if the samples are added"},
        {"queued", (ECSFunction)MixerQueued, "double(handle<mixer.sound> stream)",
         "Give the seconds of a stream's samples that wait to play\n"
         "@param stream The stream, as newStream gives it\n"
         "@return The seconds that wait to play; 0 for a sound that is not a stream"},
        {"finish", (ECSFunction)MixerFinish, "void(handle<mixer.sound> stream)",
         "Tell that no more samples come, so the stream's voice ends once it has played what waits\n"
         "@param stream The stream, as newStream gives it; a sound that is not a stream is left as it is"},
        {"length", (ECSFunction)MixerLength, "double(handle<mixer.sound> sound)",
         "Give a sound's length in seconds; 0 for a stream\n"
         "@param sound The sound\n"
         "@return The length in seconds"},
        {"play", (ECSFunction)MixerPlay, "int(handle<mixer.sound> sound, float volume, bool loop)",
         "Play a sound, and give the number of the voice that plays it; 0 if it cannot play\n"
         "@param sound The sound or stream to play; the voice keeps it while it plays\n"
         "@param volume The voice's gain: 1 is the sound's own, 0 is silent; below 0 counts as 0\n"
         "@param loop True: the sound starts again when it ends, until stopped; a stream does not loop\n"
         "@return The voice, a number above 0; 0 if there is no output, or the stream already plays in a voice"},
        {"stop", (ECSFunction)MixerStop, "void(int voice)",
         "Stop a voice\n"
         "@param voice The voice, as play gives it; a voice that has ended is left alone"},
        {"stopAll", (ECSFunction)MixerStopAll, "void()", "Stop every voice"},
        {"playing", (ECSFunction)MixerPlaying, "bool(int voice)",
         "Tell whether a voice still plays\n"
         "@param voice The voice, as play gives it\n"
         "@return True while it plays, its echo included; false once it has ended"},
        {"volume", (ECSFunction)MixerSetVolume, "void(int voice, float volume)",
         "Change a voice's volume; 1 is the sound's own\n"
         "@param voice The voice, as play gives it; a voice that has ended is left alone\n"
         "@param volume The new gain: 1 is the sound's own, 0 is silent; below 0 counts as 0"},
        {"pan", (ECSFunction)MixerSetPan, "void(int voice, float pan)",
         "Move a voice from left, -1, to right, 1; 0 is the middle\n"
         "@param voice The voice, as play gives it; a voice that has ended is left alone\n"
         "@param pan From -1, left, to 1, right; 0 is the middle, where both channels keep their whole gain"},
        {"filter", (ECSFunction)MixerSetFilter, "void(int voice, float lowpass, float highpass)",
         "Filter a voice: lowpass cuts the sounds above a frequency in hertz, and highpass those below one; 0 turns a filter off\n"
         "@param voice The voice, as play gives it; a voice that has ended is left alone\n"
         "@param lowpass The frequency in hertz above which sounds are cut; 0 turns it off\n"
         "@param highpass The frequency in hertz below which sounds are cut; 0 turns it off"},
        {"echo", (ECSFunction)MixerSetEcho, "void(int voice, float delay, float feedback, float mix)",
         "Give a voice an echo: what it played delay seconds ago, up to 2, comes back at mix, and again at feedback times as loud each time; a delay or mix of 0 takes it away\n"
         "@param voice The voice, as play gives it; a voice that has ended is left alone\n"
         "@param delay The seconds after which a sound comes back, up to 2; 0 takes the echo away\n"
         "@param feedback How loud each echo comes back next to the one before, from 0 to 0.95\n"
         "@param mix How loud the first echo is, from 0 to 1; 0 takes the echo away"},
        {"reverb", (ECSFunction)MixerSetReverb, "bool(float mix, float size)",
         "Add a reverb to everything that plays: mix from 0, none, to 1, and the room's size from 0 to 1; false if there is no output\n"
         "@param mix How much of the reverb is heard, from 0, none, to 1\n"
         "@param size The room's size, from 0 to 1; a larger room rings longer\n"
         "@return True if the reverb is set; false if there is no output"},
        {"keepSound", (ECSFunction)MixerKeepSound, "void(handle<mixer.sound> sound)",
         "Keep a sound alive, even when no one uses its handle, until releaseSound; for a plugin that feeds a stream it gave away\n"
         "@param sound The sound to keep; each keepSound needs its own releaseSound"},
        {"releaseSound", (ECSFunction)MixerReleaseSound, "void(handle<mixer.sound> sound)",
         "Let go of a sound that keepSound kept\n"
         "@param sound The sound that keepSound kept"},
        {"freeSound", (ECSFunction)MixerFreeSound, "void(handle<mixer.sound> sound)",
         "Free a sound now, instead of when no one uses its handle; voices that play it finish first\n"
         "@param sound The sound; its handle must not be used after"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description), MixerFree(nullptr););
    }

    // Lua calls these on a handle, such as handle:play(...); C calls the functions themselves (OpenECS DESIGN 10.6)
    const struct
    {
        const char *type;
        const char *method;
        const char *function;
    } methods[] = {
        {"sound", "play", "play"},
        {"sound", "length", "length"},
        {"sound", "queue", "queue"},
        {"sound", "queued", "queued"},
        {"sound", "finish", "finish"},
        {"sound", "keep", "keepSound"},
        {"sound", "release", "releaseSound"},
        {"sound", "free", "freeSound"},
    };

    for (usz i = 0; i < SDL_arraysize(methods); i++)
    {
        SHU_ReturnResult(ECSHandle_AddMethod(plugin, methods[i].type, methods[i].method, methods[i].function), MixerFree(nullptr););
    }

    return SHUResult_Ok;
}
