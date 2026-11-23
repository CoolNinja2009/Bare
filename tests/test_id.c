#include "bare_test.h"
#include "bare.h"

TEST(id_is_stable) {
    CHECK(BARE_ID("save") == BARE_ID("save"));
    CHECK(BARE_IDI("row", 7) == BARE_IDI("row", 7));
}

TEST(id_names_differ) {
    CHECK(BARE_ID("save") != BARE_ID("load"));
    CHECK(BARE_ID("a") != BARE_ID("b"));
    CHECK(BARE_ID("") != BARE_ID(" "));
}

TEST(id_index_variants_differ) {
    CHECK(BARE_IDI("row", 0) != BARE_IDI("row", 1));
    CHECK(BARE_IDI("row", 0) != BARE_ID("row"));
    CHECK(BARE_IDI("a", 1) != BARE_IDI("b", 1));
}

TEST(id_never_zero) {
    CHECK(BARE_ID("") != 0);
    for (uint32_t i = 0; i < 10000; i++) CHECK(BARE_IDI("x", i) != 0);
}

TEST(id_no_collisions_in_a_big_list) {
    /* 10,000 list ids: no two equal (sorted-free check via a small open hash set). */
    enum { N = 10000, CAP = 32768 };
    static BareId set[CAP];
    for (uint32_t i = 0; i < N; i++) {
        BareId id = BARE_IDI("item", i);
        uint32_t slot = (uint32_t)(id & (CAP - 1));
        while (set[slot]) {
            CHECK(set[slot] != id);
            slot = (slot + 1) & (CAP - 1);
        }
        set[slot] = id;
    }
}

int main(void) {
    RUN(id_is_stable);
    RUN(id_names_differ);
    RUN(id_index_variants_differ);
    RUN(id_never_zero);
    RUN(id_no_collisions_in_a_big_list);
    return test_report();
}
