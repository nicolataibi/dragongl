/*
 * DRAGON GL - Unit tests: wire protocol invariants (protocol.h)
 *
 * The client and the server are compiled from the SAME header; these
 * tests pin down the binary layout so a stray struct edit cannot
 * silently desynchronize the two ends of the connection.
 * Run via `ctest` (see CMakeLists.txt).
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include "protocol.h"

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

int main(void) {
    /*--- header layout ---*/
    CHECK(sizeof(MsgHeader) == 12); /*type + length + version + reserved*/
    CHECK(offsetof(MsgHeader, version) == 8);

    /*msg_hdr() must fill version and zero the reserved field — a
     * hand-written {type, len} initializer would send version 0 and
     * the peer would reject the message.*/
    MsgHeader h = msg_hdr(MSG_STATE, (int)sizeof(MsgState));
    CHECK(h.type == MSG_STATE);
    CHECK(h.length == (int)sizeof(MsgState));
    CHECK(h.version == PROTOCOL_VERSION);
    CHECK(h.reserved == 0);

    CHECK(PROTOCOL_VERSION > 0);

    /*--- batch message: the payload formula used by the server's
     * flush_entity_batch and by the client's MSG_ENTITY_UPDATE
     * handler must agree ---*/
    /*v2: hp is int32_t (boss HP on deep floors exceeds int16); the
     * layout stays 12 B (4 + 2 + 2 + 4, hp 4-aligned, no padding).*/
    CHECK(sizeof(EntityUpdateRec) == 12);
    CHECK(offsetof(EntityUpdateRec, hp) == 8);
    for (int n = 1; n <= 512; n += 7) {
        int expect = (int)(sizeof(int32_t) + (size_t)n * sizeof(EntityUpdateRec));
        CHECK(expect == 4 + 12 * n);
    }

    /*--- every fixed-size payload is exactly what the peers check ---*/
    CHECK(sizeof(MsgMove) == 4 * 4);        /*entity_id, dx, dy, dz*/
    CHECK(sizeof(MsgTimeSync) == 3 * 4);    /*hour, minute, turns*/
    CHECK(sizeof(MsgTombstoneRemove) == 4);
    CHECK(sizeof(MsgAuthFail) == 128);      /*reason[128]*/
    CHECK(sizeof(MsgLogin) == 32 * 3 + 15 * 4); /*3 strings + 15 ints*/

    /*message ids stay unique and dense (the client's valid_msg table
     * switches on them)*/
    CHECK(MSG_LOGIN == 1 && MSG_WELCOME == 2 && MSG_MOVE == 3 &&
          MSG_STATE == 4 && MSG_MAP_CHUNK == 5 && MSG_TEXT_CMD == 6 &&
          MSG_TEXT == 7 && MSG_AUTH_FAIL == 8 && MSG_SPELL_VFX == 9 &&
          MSG_TOMBSTONE_REMOVE == 10 && MSG_TIME_SYNC == 11 &&
          MSG_ENTITY_UPDATE == 12);

    if (g_failures == 0) {
        printf("test_protocol: all passed\n");
        return 0;
    }
    printf("test_protocol: %d FAILURES\n", g_failures);
    return 1;
}
