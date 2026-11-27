/* bare_sound_loop and bare_sound_volume on SDL's dummy audio driver (no speakers): a looped sound is still playing
 * after its length has gone by, and its position wraps back to the start; a plain play of the same sound has
 * ended by then. */
#include "bare_test.h"
#include "bare.h"
#include <SDL3/SDL.h>
#include <stdlib.h>

#define RATE 48000
#define FRAMES (RATE / 5)   /* 0.2 s */

static BareSound make_tone(void) {
    static float s[FRAMES];
    for (int i = 0; i < FRAMES; i++) s[i] = 0.25f * (float)sin(2 * 3.14159265358979 * 440 * i / RATE);
    return bare_sound(s, FRAMES, 1, RATE);
}

TEST(loop_keeps_playing_and_wraps) {
    BareSound s = make_tone();
    CHECK(s != 0);
    bare_sound_loop(s, 0.5f);
    SDL_Delay(100);
    CHECK(bare_sound_position(s) >= 0);
    bare_sound_volume(s, 0.1f);
    SDL_Delay(500);   /* 2.5 lengths */
    double at = bare_sound_position(s);
    CHECK(at >= 0);
    CHECK(at < 0.2 + 1e-6);
    bare_sound_stop(s);
    CHECK(bare_sound_position(s) < 0);
    bare_sound_free(s);
}

TEST(plain_play_ends) {
    BareSound s = make_tone();
    CHECK(s != 0);
    bare_sound_play(s, 0.5f);
    SDL_Delay(100);
    CHECK(bare_sound_position(s) >= 0);
    SDL_Delay(500);
    CHECK(bare_sound_position(s) < 0);
    bare_sound_free(s);
}

int main(void) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    RUN(loop_keeps_playing_and_wraps);
    RUN(plain_play_ends);
    return test_report();
}
