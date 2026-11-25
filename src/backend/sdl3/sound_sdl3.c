/* Sound: short sounds held in memory (clicks, cues), played on the default output through SDL3's audio.
 *
 * Each play takes an idle stream from a small pool bound to one device, so sounds overlap and SDL mixes them;
 * with every stream busy the sound is skipped (a late UI sound is worse than none). The device opens on the first
 * play and closes after a while with nothing to play, so an app that's quiet keeps no audio thread running. */
#include "bare.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SOUNDS 64
#define VOICES 8
#define IDLE_MS 20000   /* close the device after this long without a sound */

typedef struct Sound { float *data; int frames, channels, rate; } Sound;

static Sound g_sounds[MAX_SOUNDS];
static SDL_AudioDeviceID g_dev;
static SDL_AudioStream *g_voice[VOICES];
static SDL_Mutex *g_lock;
static SDL_TimerID g_timer;
static Uint64 g_last;            /* SDL_GetTicks of the last play */
static bool g_failed;            /* no audio here: don't keep trying */

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

void bare_sound_free(BareSound sound) {
    if (sound < 1 || sound > MAX_SOUNDS) return;
    free(g_sounds[sound - 1].data);
    g_sounds[sound - 1] = (Sound){ 0 };
}

static void close_device(void) {
    for (int i = 0; i < VOICES; i++) {
        if (g_voice[i]) SDL_DestroyAudioStream(g_voice[i]);
        g_voice[i] = NULL;
    }
    if (g_dev) SDL_CloseAudioDevice(g_dev);
    g_dev = 0;
}

/* The timer thread: closes the device once nothing has played for IDLE_MS. */
static Uint32 SDLCALL idle_check(void *user, SDL_TimerID id, Uint32 interval) {
    (void)user; (void)id;
    SDL_LockMutex(g_lock);
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

void bare_sound_play(BareSound sound, float volume) {
    if (sound < 1 || sound > MAX_SOUNDS || !g_sounds[sound - 1].data || volume <= 0) return;
    if (!g_lock && !(g_lock = SDL_CreateMutex())) return;
    SDL_LockMutex(g_lock);
    const Sound *s = &g_sounds[sound - 1];
    if (open_device()) {
        SDL_AudioSpec src = { SDL_AUDIO_F32, s->channels, s->rate }, dst;
        SDL_AudioStream *voice = NULL;
        if (SDL_GetAudioDeviceFormat(g_dev, &dst, NULL)) {
            for (int i = 0; i < VOICES && !voice; i++) {
                if (!g_voice[i]) {
                    g_voice[i] = SDL_CreateAudioStream(&src, &dst);
                    if (g_voice[i] && !SDL_BindAudioStream(g_dev, g_voice[i])) {
                        SDL_DestroyAudioStream(g_voice[i]);
                        g_voice[i] = NULL;
                    }
                }
                if (g_voice[i] && SDL_GetAudioStreamQueued(g_voice[i]) == 0) voice = g_voice[i];
            }
        }
        if (voice) {
            SDL_SetAudioStreamFormat(voice, &src, NULL);
            SDL_SetAudioStreamGain(voice, volume > 1 ? 1 : volume);
            SDL_PutAudioStreamData(voice, s->data, s->frames * s->channels * (int)sizeof(float));
            SDL_FlushAudioStream(voice);
        }
        g_last = SDL_GetTicks();
        if (!g_timer) g_timer = SDL_AddTimer(IDLE_MS / 4, idle_check, NULL);
    }
    SDL_UnlockMutex(g_lock);
}
