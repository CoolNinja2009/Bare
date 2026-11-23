/* id.c: FNV-1a hashing for element IDs (ARCHITECTURE §4). */
#include "internal.h"

#define FNV_PRIME 0x100000001b3ull

uint64_t bare__hash_bytes(uint64_t h, const void *data, size_t size) {
    const unsigned char *p = data;
    for (size_t i = 0; i < size; i++) {
        h ^= p[i];
        h *= FNV_PRIME;
    }
    return h;
}

uint64_t bare__hash_str(uint64_t h, const char *s) {
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= FNV_PRIME;
    }
    return h;
}

uint64_t bare__hash_u64(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        h ^= (v >> (i * 8)) & 0xff;
        h *= FNV_PRIME;
    }
    return h;
}

/* 0 is reserved for "automatic", so a hash that lands on 0 is nudged. */
static BareId nonzero(uint64_t h) { return h ? h : 1; }

BareId bare_id(const char *name) {
    return nonzero(bare__hash_str(BARE__FNV_OFFSET, name ? name : ""));
}

BareId bare_id_index(const char *name, uint32_t index) {
    uint64_t h = bare__hash_str(BARE__FNV_OFFSET, name ? name : "");
    h ^= 0xff;                       /* separates ("a", 1) from bare_id("a\x01") */
    h *= FNV_PRIME;
    return nonzero(bare__hash_u64(h, index));
}
