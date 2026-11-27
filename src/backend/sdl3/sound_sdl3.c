/* Sound: sounds held in memory (clicks and cues, or whole songs and soundtracks loaded from files), played on the
 * default output through SDL3's audio.
 *
 * Each play takes an idle voice from a small pool bound to one device, so sounds overlap and SDL mixes them; with
 * every voice busy the sound is skipped (a late UI sound is worse than none). A voice is fed from the sound's samples
 * as the device asks for them (a get callback), so a five-minute song isn't copied again to be played, and the voice
 * knows how far it is (bare_sound_position). The device opens on the first play and closes after a while with
 * nothing playing, so an app that's quiet keeps no audio thread running.
 *
 * On a fixed clock (BARE_RECORD, BARE_FIXED_DT) nothing is heard: plays are kept on that clock instead, so
 * bare_sound_position follows bare_time exactly and pictures timed by it match the sound in a recording. */
#include "bare.h"
#include "backend/media.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SOUNDS 64
#define VOICES 8
#define IDLE_MS 20000   /* close the device after this long without a sound */
#define FEED 2048       /* frames given to a voice at least at a time */

typedef struct Sound { float *data; int frames, channels, rate; } Sound;

/* A voice's fields change only with its stream locked: the device's thread reads them in feed(). */
typedef struct Voice {
    SDL_AudioStream *stream;
    BareSound sound;     /* playing this (0: idle) */
    long next;           /* the next frame to give it */
    Uint64 started;      /* play order: the latest play of a sound is the one bare_sound_position reports */
} Voice;

static Sound g_sounds[MAX_SOUNDS];
static SDL_AudioDeviceID g_dev;
static Voice g_voice[VOICES];
static SDL_Mutex *g_lock;
static SDL_TimerID g_timer;
static Uint64 g_last;            /* SDL_GetTicks when something last played */
static Uint64 g_plays;
static bool g_failed;            /* no audio here: don't keep trying */

/* Fixed-clock plays: which sound, from where, and when (bare_time). */
typedef struct Virtual { BareSound sound; double from, t0; Uint64 started; } Virtual;
static bool g_virtual;
static Virtual g_virt[VOICES];

void bare__sound_virtual(bool on) { g_virtual = on; }

static bool ensure_lock(void) { return g_lock || (g_lock = SDL_CreateMutex()) != NULL; }

BareSound bare_sound(const float *samples, int frames, int channels, int rate) {
    if (!samples || frames <= 0 || channels < 1 || channels > 2 || rate <= 0) return 0;
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if (g_sounds[i].data) continue;
        size_t n = (size_t)frames * (size_t)channels;
        float *d = malloc(n * sizeof *d);
        if (!d) return 0;
        memcpy(d, samples, n * sizeof *d);
        g_sounds[i] = (Sound){ d, frames, channels, rate };
        return (BareSound)(i + 1);
    }
    return 0;
}

/* WAV through SDL, for systems without a decoder of their own (and WAVs the system can't read). */
static bool load_wav(const char *path, float **out, int *frames, int *channels, int *rate) {
    SDL_AudioSpec spec;
    Uint8 *buf = NULL, *conv = NULL;
    Uint32 len = 0;
    int clen = 0;
    if (!SDL_LoadWAV(path, &spec, &buf, &len)) return false;
    SDL_AudioSpec want = { SDL_AUDIO_F32, spec.channels > 1 ? 2 : 1, spec.freq };
    bool ok = SDL_ConvertAudioSamples(&spec, buf, (int)len, &want, &conv, &clen) && clen > 0;
    SDL_free(buf);
    if (!ok) return false;
    *out = malloc((size_t)clen);
    if (*out) memcpy(*out, conv, (size_t)clen);
    SDL_free(conv);
    *channels = want.channels, *rate = want.freq;
    *frames = clen / (int)(sizeof(float) * (size_t)want.channels);
    return *out != NULL;
}

BareSound bare_sound_load(const char *path) {
    if (!path || !*path) return 0;
    int slot = -1;
    for (int i = 0; i < MAX_SOUNDS && slot < 0; i++) if (!g_sounds[i].data) slot = i;
    if (slot < 0) return 0;
    float *data = NULL;
    int frames = 0, channels = 0, rate = 0;
    bare__media_thread_begin();
    bool ok = bare__media_audio(path, &data, &frames, &channels, &rate);
    bare__media_thread_end();
    if (!ok) ok = load_wav(path, &data, &frames, &channels, &rate);
    if (!ok || frames <= 0) { free(data); return 0; }
    g_sounds[slot] = (Sound){ data, frames, channels, rate };
    return (BareSound)(slot + 1);
}

static const Sound *get(BareSound sound) {
    return sound >= 1 && sound <= MAX_SOUNDS && g_sounds[sound - 1].data ? &g_sounds[sound - 1] : NULL;
}

const float *bare_sound_samples(BareSound sound, int *frames, int *channels, int *rate) {
    const Sound *s = get(sound);
    if (frames) *frames = s ? s->frames : 0;
    if (channels) *channels = s ? s->channels : 0;
    if (rate) *rate = s ? s->rate : 0;
    return s ? s->data : NULL;
}

double bare_sound_length(BareSound sound) {
    const Sound *s = get(sound);
    return s ? (double)s->frames / s->rate : 0;
}

/* The device's thread, when a voice runs low: the next stretch of its sound. */
static void SDLCALL feed(void *user, SDL_AudioStream *stream, int additional, int total) {
    (void)total;
    Voice *v = user;
    const Sound *s = get(v->sound);
    if (!s || v->next >= s->frames) return;
    long want = additional / (int)(sizeof(float) * (size_t)s->channels);
    if (want < FEED) want = FEED;
    if (want > s->frames - v->next) want = s->frames - v->next;
    SDL_PutAudioStreamData(stream, s->data + (size_t)v->next * (size_t)s->channels,
                           (int)(want * s->channels * (long)sizeof(float)));
    v->next += want;
}

/* Frames given to the voice but not yet played (roughly: the device's own buffer isn't counted). */
static long queued_frames(const Voice *v, const Sound *s) {
    int bytes = SDL_GetAudioStreamQueued(v->stream);
    return bytes > 0 ? bytes / (long)(sizeof(float) * (size_t)s->channels) : 0;
}

static bool busy(Voice *v) {
    if (!v->stream || !v->sound) return false;
    const Sound *s = get(v->sound);
    if (!s || (v->next >= s->frames && SDL_GetAudioStreamQueued(v->stream) == 0)) {
        v->sound = 0;
        return false;
    }
    return true;
}

static void stop_voice(Voice *v) {
    if (!v->stream) return;
    SDL_LockAudioStream(v->stream);
    v->sound = 0;
    SDL_ClearAudioStream(v->stream);
    SDL_UnlockAudioStream(v->stream);
}

void bare_sound_free(BareSound sound) {
    if (sound < 1 || sound > MAX_SOUNDS) return;
    for (int i = 0; i < VOICES; i++) if (g_virt[i].sound == sound) g_virt[i].sound = 0;
    if (ensure_lock()) {
        SDL_LockMutex(g_lock);
        for (int i = 0; i < VOICES; i++) if (g_voice[i].sound == sound) stop_voice(&g_voice[i]);
        SDL_UnlockMutex(g_lock);
    }
    free(g_sounds[sound - 1].data);
    g_sounds[sound - 1] = (Sound){ 0 };
}

static void close_device(void) {
    for (int i = 0; i < VOICES; i++) {
        if (g_voice[i].stream) SDL_DestroyAudioStream(g_voice[i].stream);
        g_voice[i] = (Voice){ 0 };
    }
    if (g_dev) SDL_CloseAudioDevice(g_dev);
    g_dev = 0;
}

/* The timer thread: closes the device once nothing has played for IDLE_MS. */
static Uint32 SDLCALL idle_check(void *user, SDL_TimerID id, Uint32 interval) {
    (void)user; (void)id;
    SDL_LockMutex(g_lock);
    for (int i = 0; i < VOICES; i++) if (busy(&g_voice[i])) g_last = SDL_GetTicks();   /* a song still playing */
    bool idle = SDL_GetTicks() - g_last >= IDLE_MS;
    if (idle) {
        close_device();
        g_timer = 0;
    }
    SDL_UnlockMutex(g_lock);
    return idle ? 0 : interval;
}

static bool open_device(void) {
    if (g_dev) return true;
    if (g_failed) return false;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        g_failed = true;
        return false;
    }
    g_dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
    if (!g_dev) {
        g_failed = true;
        return false;
    }
    return true;
}

static double virtual_position(const Virtual *v) {
    const Sound *s = get(v->sound);
    if (!s) return -1;
    double at = v->from + (bare_time() - v->t0);
    return at < (double)s->frames / s->rate ? at : -1;
}

void bare_sound_play_at(BareSound sound, float volume, double seconds) {
    const Sound *s = get(sound);
    if (!s || volume <= 0 || !ensure_lock()) return;
    long from = seconds > 0 ? (long)(seconds * s->rate) : 0;
    if (from >= s->frames) return;
    if (g_virtual) {
        for (int i = 0; i < VOICES; i++) {
            if (g_virt[i].sound && virtual_position(&g_virt[i]) >= 0) continue;
            g_virt[i] = (Virtual){ sound, (double)from / s->rate, bare_time(), ++g_plays };
            break;
        }
        return;
    }
    SDL_LockMutex(g_lock);
    if (open_device()) {
        SDL_AudioSpec src = { SDL_AUDIO_F32, s->channels, s->rate }, dst;
        Voice *voice = NULL;
        if (SDL_GetAudioDeviceFormat(g_dev, &dst, NULL)) {
            for (int i = 0; i < VOICES && !voice; i++) {
                Voice *v = &g_voice[i];
                if (!v->stream) {
                    v->stream = SDL_CreateAudioStream(&src, &dst);
                    if (v->stream && (!SDL_SetAudioStreamGetCallback(v->stream, feed, v) ||
                                      !SDL_BindAudioStream(g_dev, v->stream))) {
                        SDL_DestroyAudioStream(v->stream);
                        v->stream = NULL;
                    }
                }
                if (v->stream && !busy(v)) voice = v;
            }
        }
        if (voice) {
            SDL_LockAudioStream(voice->stream);
            SDL_ClearAudioStream(voice->stream);
            SDL_SetAudioStreamFormat(voice->stream, &src, NULL);
            SDL_SetAudioStreamGain(voice->stream, volume > 1 ? 1 : volume);
            voice->sound = sound;
            voice->next = from;
            voice->started = ++g_plays;
            SDL_UnlockAudioStream(voice->stream);
        }
        g_last = SDL_GetTicks();
        if (!g_timer) g_timer = SDL_AddTimer(IDLE_MS / 4, idle_check, NULL);
    }
    SDL_UnlockMutex(g_lock);
}

void bare_sound_play(BareSound sound, float volume) { bare_sound_play_at(sound, volume, 0); }

void bare_sound_stop(BareSound sound) {
    if (!get(sound) || !ensure_lock()) return;
    for (int i = 0; i < VOICES; i++) if (g_virt[i].sound == sound) g_virt[i].sound = 0;
    SDL_LockMutex(g_lock);
    for (int i = 0; i < VOICES; i++) if (g_voice[i].sound == sound) stop_voice(&g_voice[i]);
    SDL_UnlockMutex(g_lock);
}

double bare_sound_position(BareSound sound) {
    const Sound *s = get(sound);
    if (!s || !ensure_lock()) return -1;
    double at = -1;
    Uint64 latest = 0;
    for (int i = 0; i < VOICES; i++) {
        const Virtual *v = &g_virt[i];
        double p = v->sound == sound ? virtual_position(v) : -1;
        if (p >= 0 && v->started >= latest) latest = v->started, at = p;
    }
    if (latest) return at;
    SDL_LockMutex(g_lock);
    for (int i = 0; i < VOICES; i++) {
        Voice *v = &g_voice[i];
        if (v->sound != sound || !busy(v) || v->started < latest) continue;
        SDL_LockAudioStream(v->stream);
        long played = v->next - queued_frames(v, s);
        SDL_UnlockAudioStream(v->stream);
        latest = v->started;
        at = (double)(played > 0 ? played : 0) / s->rate;
    }
    SDL_UnlockMutex(g_lock);
    return at;
}
