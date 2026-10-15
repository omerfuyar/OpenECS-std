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

/// @brief A sample that has faded to almost nothing becomes 0, as in Freeverb: numbers that small are denormal, which many processors work on far slower.
static f32 MixerFlush(f32 sample)
{
    return SDL_fabsf(sample) < 1e-20f ? 0.0f : sample;
}

/// @brief Tunes a filter to a frequency, keeping what it heard: a Butterworth low-pass or high-pass of the Audio EQ Cookbook, or, at 0 hertz, one that lets everything through.
static void MixerTune(MixerBiquad *filter, i32 hertz, i32 rate, bool high)
{
    if (hertz <= 0)
    {
        filter->b0 = 1.0f;
        filter->b1 = filter->b2 = filter->a1 = filter->a2 = 0.0f;
        return;
    }

    // a frequency at or past half the rate cannot be filtered, so it is kept just below; Q is 1/sqrt(2), for no peak at the edge
    f32 frequency = SDL_min((f32)hertz, 0.49f * (f32)rate);
    f32 omega = 2.0f * SDL_PI_F * frequency / (f32)rate;
    f32 alpha = SDL_sinf(omega) / (2.0f * 0.70710678f);
    f32 cosine = SDL_cosf(omega);
    f32 a0 = 1.0f + alpha;
    f32 edge = high ? (1.0f + cosine) / 2.0f : (1.0f - cosine) / 2.0f;
    filter->b0 = edge / a0;
    filter->b1 = (high ? -2.0f : 2.0f) * edge / a0;
    filter->b2 = edge / a0;
    filter->a1 = -2.0f * cosine / a0;
    filter->a2 = (1.0f - alpha) / a0;
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
            MixerTune(&voice->filters[2 * channel], lowpass, voice->sound->spec.freq, false);
            MixerTune(&voice->filters[2 * channel + 1], highpass, voice->sound->spec.freq, true);
        }

        voice->tunedLowpass = lowpass;
        voice->tunedHighpass = highpass;
    }

    for (u64 i = 0; i < count; i++)
    {
        for (i32 channel = 0; channel < channels; channel++)
        {
            f32 sample = frames[i * (u64)channels + (u64)channel];

            for (i32 k = 0; k < 2; k++)
            {
                MixerBiquad *f = &voice->filters[2 * channel + k];
                f32 out = f->b0 * sample + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
                f->x2 = f->x1;
                f->x1 = sample;
                // a filter that hears silence fades toward 0 through denormal numbers, so its memory is flushed as the reverb's is
                f->y2 = f->y1;
                f->y1 = MixerFlush(out);
                sample = out;
            }

            frames[i * (u64)channels + (u64)channel] = sample;
        }
    }
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

        // a filtered voice changes its samples, so they go through its room for samples even when nothing else changes them
        bool filtered = voice->tunedLowpass != 0 || voice->tunedHighpass != 0 || SDL_GetAtomicInt(&voice->lowpass) != 0 || SDL_GetAtomicInt(&voice->highpass) != 0;

        if (out == in && (out != 2 || pan == 0.0f) && !filtered)
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

            if (out == in && (out != 2 || pan == 0.0f))
            {
                SDL_memmove(panned, frames, (usz)count * outFrameSize);
            }
            else
            {
                for (u64 i = 0; i < count; i++)
                {
                    f32 l = frames[i * (u64)in];
                    f32 r = in == 1 ? l : frames[i * (u64)in + 1];
                    panned[2 * i] = l * left;
                    panned[2 * i + 1] = r * right;
                }
            }

            if (filtered)
            {
                MixerFilter(voice, panned, count);
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
    SDL_free(voice->filters);
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

/// @brief Makes a reverb for an output's channels and rate, with every delay silent.
/// @return The reverb, to free with MixerFreeReverb, or nullptr if out of memory.
static MixerReverb *MixerNewReverb(i32 channels, i32 rate)
{
    const usz filters = SDL_arraysize(MIXER_COMBS) + SDL_arraysize(MIXER_ALLPASSES);
    MixerReverb *reverb = SDL_calloc(1, sizeof(MixerReverb));
    MixerDelay *delays = reverb == nullptr ? nullptr : SDL_calloc((usz)channels * filters, sizeof(MixerDelay));
    usz total = 0;

    // the delays are as long in seconds at any rate
    for (i32 channel = 0; delays != nullptr && channel < channels; channel++)
    {
        for (usz i = 0; i < filters; i++)
        {
            i32 frames = i < SDL_arraysize(MIXER_COMBS) ? MIXER_COMBS[i] : MIXER_ALLPASSES[i - SDL_arraysize(MIXER_COMBS)];
            MixerDelay *delay = &delays[(usz)channel * filters + i];
            delay->length = SDL_max(1, (i32)((i64)(frames + (channel % 2) * MIXER_SPREAD) * rate / 44100));
            total += (usz)delay->length;
        }
    }

    f32 *memory = delays == nullptr ? nullptr : SDL_calloc(total, sizeof(f32));

    if (memory == nullptr)
    {
        SDL_free(delays);
        SDL_free(reverb);
        return nullptr;
    }

    for (usz i = 0, at = 0; i < (usz)channels * filters; i++)
    {
        delays[i].samples = memory + at;
        at += (usz)delays[i].length;
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
            f32 out = 0.0f;

            for (usz i = 0; i < combs; i++)
            {
                MixerDelay *comb = &delays[i];
                f32 delayed = comb->samples[comb->at];
                comb->kept = MixerFlush(delayed * (1.0f - damp) + comb->kept * damp);
                comb->samples[comb->at] = MixerFlush(input + comb->kept * feedback);
                comb->at = comb->at + 1 == comb->length ? 0 : comb->at + 1;
                out += delayed;
            }

            for (usz i = combs; i < filters; i++)
            {
                MixerDelay *allpass = &delays[i];
                f32 delayed = allpass->samples[allpass->at];
                allpass->samples[allpass->at] = MixerFlush(out + delayed * 0.5f);
                allpass->at = allpass->at + 1 == allpass->length ? 0 : allpass->at + 1;
                out = delayed - out;
            }

            sample[channel] += out * wet;
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

/// @brief Sets the reverb on the output: mix is how much of it is added, from 0, none, to 1, and size the room's, from 0 to 1.
/// @return false if there is no output, or memory for the reverb.
static bool MixerSetReverb(f32 mix, f32 size)
{
    SDL_AudioSpec spec = {0};

    if (!MixerOpen() || !SDL_GetAudioDeviceFormat(MIXER.device, &spec, nullptr))
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

/// @brief Keeps a sound alive for a plugin that uses it after giving its handle away, such as one that feeds a stream, until releaseSound.
static void MixerKeepSound(MixerSound *sound)
{
    sound->users++;
}

/// @brief Lets go of a sound that keepSound kept.
static void MixerReleaseSound(MixerSound *sound)
{
    MixerRelease(sound);
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
        {"filter", (ECSFunction)MixerSetFilter, "void(int voice, float lowpass, float highpass)", "Filter a voice: lowpass cuts the sounds above a frequency in hertz, and highpass those below one; 0 turns a filter off"},
        {"reverb", (ECSFunction)MixerSetReverb, "bool(float mix, float size)", "Add a reverb to everything that plays: mix from 0, none, to 1, and the room's size from 0 to 1; false if there is no output"},
        {"keepSound", (ECSFunction)MixerKeepSound, "void(handle<mixer.sound> sound)", "Keep a sound alive, even when no one uses its handle, until releaseSound; for a plugin that feeds a stream it gave away"},
        {"releaseSound", (ECSFunction)MixerReleaseSound, "void(handle<mixer.sound> sound)", "Let go of a sound that keepSound kept"},
        {"freeSound", (ECSFunction)MixerFreeSound, "void(handle<mixer.sound> sound)", "Free a sound now, instead of when no one uses its handle; voices that play it finish first"},
    };

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description), MixerFree(nullptr););
    }

    return SHUResult_Ok;
}
