/* BARE_RECORD's encoder (src/backend/record_win32.c), without a window or a
 * GPU: frames in both channel orders make a playable MP4 of the right length;
 * a path that can't be written fails cleanly. Elsewhere than Windows it only
 * checks that opening fails. */
#include "bare_test.h"
#include "backend/record.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 48

static long file_size(const char *path, unsigned char head[8]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t got = fread(head, 1, 8, f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return got == 8 ? n : -1;
}

TEST(writes_an_mp4) {
    char path[512];
    const char *tmp = getenv("TEMP");
    snprintf(path, sizeof path, "%s/bare_test_record.mp4", tmp ? tmp : ".");
    remove(path);
    BareRecorder *r = bare__record_open(path, W, H, 30);
#ifdef _WIN32
    CHECK(r != NULL);
    if (!r) return;
    static unsigned char px[H][W + 3][4];   /* a pitch wider than the row */
    for (int f = 0; f < 30; f++) {
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                px[y][x][0] = (unsigned char)(x * 4), px[y][x][1] = (unsigned char)(y * 5);
                px[y][x][2] = (unsigned char)(f * 8), px[y][x][3] = 255;
            }
        CHECK(bare__record_frame(r, px, (W + 3) * 4, f % 2 == 0));   /* BGRA and RGBA in turn */
    }
    CHECK(bare__record_close(r) == 30);
    unsigned char head[8];
    long n = file_size(path, head);
    CHECK(n > 1000);
    CHECK(memcmp(head + 4, "ftyp", 4) == 0);   /* an MP4 starts with its ftyp box */
    remove(path);
#else
    CHECK(r == NULL);
#endif
}

TEST(bad_path_fails_cleanly) {
    CHECK(bare__record_open("Z:/no/such/folder/at/all/x.mp4", W, H, 30) == NULL);
    CHECK(!bare__record_frame(NULL, NULL, 0, true));
    CHECK(bare__record_close(NULL) == 0);
}

int main(void) {
    RUN(writes_an_mp4);
    RUN(bad_path_fails_cleanly);
    return test_report();
}
