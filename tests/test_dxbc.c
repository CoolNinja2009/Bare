/* DXBC edits (src/backend/dxbc.c), on the pass vertex shader FXC made for the
 * D3D12 backend: the checksum matches FXC's, and making it strict clears only
 * the refactoringAllowed flag and leaves a valid checksum. */
#include "bare_test.h"
#include "backend/dxbc.h"
#include "bare_shaders.h"
#include <string.h>

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Finds the dcl_globalFlags token (assumes FXC's layout: it's the first declaration). */
static uint32_t global_flags(const unsigned char *blob) {
    uint32_t chunks = rd32(blob + 28);
    for (uint32_t k = 0; k < chunks; k++) {
        const unsigned char *c = blob + rd32(blob + 32 + 4 * k);
        if (memcmp(c, "SHEX", 4) == 0 || memcmp(c, "SHDR", 4) == 0) return rd32(c + 16);
    }
    return 0;
}

TEST(checksum_matches_fxc) {
    uint32_t sum[4];
    bare_dxbc_checksum(pass_vert_dxbc, sizeof pass_vert_dxbc, sum);
    for (int i = 0; i < 4; i++) CHECK(sum[i] == rd32(pass_vert_dxbc + 4 + 4 * i));
    bare_dxbc_checksum(pass_nouv_vert_dxbc, sizeof pass_nouv_vert_dxbc, sum);
    for (int i = 0; i < 4; i++) CHECK(sum[i] == rd32(pass_nouv_vert_dxbc + 4 + 4 * i));
}

TEST(strict_clears_only_the_flag) {
    unsigned char blob[sizeof pass_vert_dxbc];
    memcpy(blob, pass_vert_dxbc, sizeof blob);
    uint32_t before = global_flags(blob);
    CHECK((before & 0x7ff) == 0x6a);
    CHECK(before & (1u << 11));
    CHECK(bare_dxbc_make_strict(blob, sizeof blob));
    CHECK(global_flags(blob) == (before & ~(1u << 11)));

    uint32_t sum[4];
    bare_dxbc_checksum(blob, sizeof blob, sum);
    for (int i = 0; i < 4; i++) CHECK(sum[i] == rd32(blob + 4 + 4 * i));
    CHECK(memcmp(blob + 4, pass_vert_dxbc + 4, 16) != 0);
    int changed = 0;   /* besides the checksum, one token changed */
    for (size_t i = 20; i < sizeof blob; i++) changed += blob[i] != pass_vert_dxbc[i];
    CHECK(changed == 1);

    unsigned char again[sizeof blob];   /* idempotent */
    memcpy(again, blob, sizeof blob);
    CHECK(bare_dxbc_make_strict(again, sizeof again));
    CHECK(memcmp(again, blob, sizeof blob) == 0);
}

TEST(strict_rejects_what_it_doesnt_know) {
    unsigned char blob[sizeof pass_vert_dxbc];
    memcpy(blob, pass_vert_dxbc, sizeof blob);
    CHECK(!bare_dxbc_make_strict(blob, sizeof blob - 4));   /* size disagrees with the header */
    blob[0] = 'X';
    CHECK(!bare_dxbc_make_strict(blob, sizeof blob));
    unsigned char tiny[8] = { 'D', 'X', 'B', 'C' };
    CHECK(!bare_dxbc_make_strict(tiny, sizeof tiny));
}

int main(void) {
    RUN(checksum_matches_fxc);
    RUN(strict_clears_only_the_flag);
    RUN(strict_rejects_what_it_doesnt_know);
    return test_report();
}
