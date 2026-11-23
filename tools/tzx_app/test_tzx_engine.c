#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "tzx_engine.h"

typedef struct {
    const uint8_t *data;
    uint32_t size;
} mem_file_t;

static int32_t mem_read_cb(void *ctx, uint32_t offset, void *buf, uint32_t len) {
    mem_file_t *mf = (mem_file_t *)ctx;
    if (offset >= mf->size) return 0;
    if (offset + len > mf->size) len = mf->size - offset;
    memcpy(buf, mf->data + offset, len);
    return (int32_t)len;
}

/* Build a multi-block test .TZX covering:
 *  0: 0x30 Text Description ("JETPAC ZX")
 *  1: 0x10 Standard Speed Header (flag=0x00, Program "JETPAC    ", pause=10ms)
 *  2: 0x20 Stop the Tape (pause=0ms!)
 *  3: 0x2A Stop if 48K Mode
 *  4: 0x24 Loop Start (3 iterations)
 *  5: 0x12 Pure Tone (pulse=1000 T, count=4)
 *  6: 0x25 Loop End
 *  7: 0x11 Turbo Speed Data (pilot=1500x2, sync=500/600, zero=400, one=800, 1 byte 0xA5, pause=5ms)
 */
static size_t build_test_tzx(uint8_t *out) {
    size_t p = 0;
    memcpy(out + p, "ZXTape!\x1a\x01\x14", 10); p += 10;

    /* Block 0: 0x30 Text */
    out[p++] = 0x30;
    out[p++] = 9;
    memcpy(out + p, "JETPAC ZX", 9); p += 9;

    /* Block 1: 0x10 Standard Header (19 bytes) */
    out[p++] = 0x10;
    out[p++] = 10; out[p++] = 0;   /* pause = 10 ms */
    out[p++] = 19; out[p++] = 0;   /* len = 19 */
    uint8_t hdr[19] = {0x00, 0x00, 'J','E','T','P','A','C',' ',' ',' ',' ', 0,0,0,0,0,0, 0};
    uint8_t x = 0;
    for (int i = 0; i < 18; i++) x ^= hdr[i];
    hdr[18] = x;
    memcpy(out + p, hdr, 19); p += 19;

    /* Block 2: 0x20 Stop the Tape (0 ms) */
    out[p++] = 0x20;
    out[p++] = 0; out[p++] = 0;

    /* Block 3: 0x2A Stop if 48K Mode */
    out[p++] = 0x2A;
    out[p++] = 0; out[p++] = 0; out[p++] = 0; out[p++] = 0;

    /* Block 4..6: Loop 3x Pure Tone (1000 T x 4 pulses = 12 edges total!) */
    out[p++] = 0x24; out[p++] = 3; out[p++] = 0;
    out[p++] = 0x12; out[p++] = 0xE8; out[p++] = 0x03; out[p++] = 4; out[p++] = 0;
    out[p++] = 0x25;

    /* Block 7: 0x11 Turbo Speed Data (1 byte = 0xA5, 8 bits, pause = 5ms) */
    out[p++] = 0x11;
    out[p++] = 0xDC; out[p++] = 0x05; /* pilot = 1500 */
    out[p++] = 0xF4; out[p++] = 0x01; /* sync1 = 500 */
    out[p++] = 0x58; out[p++] = 0x02; /* sync2 = 600 */
    out[p++] = 0x90; out[p++] = 0x01; /* zero = 400 */
    out[p++] = 0x20; out[p++] = 0x03; /* one = 800 */
    out[p++] = 2;    out[p++] = 0;    /* 2 pilot pulses */
    out[p++] = 8;                     /* used bits = 8 */
    out[p++] = 5;    out[p++] = 0;    /* pause = 5ms */
    out[p++] = 1; out[p++] = 0; out[p++] = 0; /* len = 1 */
    out[p++] = 0xA5;

    return p;
}

static uint32_t count_zero_crossings(const int16_t *pcm, uint32_t n) {
    uint32_t edges = 0;
    int prev = 0;
    for (uint32_t i = 0; i < n; i++) {
        int s = (pcm[i] > 0) ? 1 : (pcm[i] < 0 ? -1 : 0);
        if (s != 0 && prev != 0 && s != prev) edges++;
        if (s != 0) prev = s;
    }
    return edges;
}

int main(void) {
    uint8_t tzx_buf[512];
    size_t tzx_len = build_test_tzx(tzx_buf);
    mem_file_t mf = { tzx_buf, (uint32_t)tzx_len };

    tzx_engine_t eng;
    memset(&eng, 0, sizeof(eng));
    eng.stop_48k_enabled = 1;

    tzx_err_t err = tzx_open(&eng, mem_read_cb, &mf, (uint32_t)tzx_len);
    assert(err == TZX_OK);
    assert(eng.block_count == 8);
    assert(strcmp(eng.tape_title, "JETPAC") == 0 || strcmp(eng.tape_title, "JETPAC ZX") == 0);
    printf("[PASS] Parsed %u TZX blocks, title='%s'\n", eng.block_count, eng.tape_title);

    /* Render until first Stop the Tape (0x20 0ms) */
    int16_t *pcm = malloc(48000 * 10 * sizeof(int16_t));
    uint32_t total1 = 0;
    tzx_render_res_t r;
    while (1) {
        uint32_t got = 0;
        r = tzx_render(&eng, pcm + total1, 256, &got);
        total1 += got;
        if (r != TZX_RENDER_CONT) break;
    }
    assert(r == TZX_RENDER_AUTO_STOP);
    assert(eng.auto_stop_is_48k == 0);
    assert(eng.cur_block == 3); /* Positioned right at block 3 (0x2A) */
    /* Expected edges in 0x10 header (19 bytes = 152 bits) with 10ms pause:
     * 8063 (pilot) + 2 (sync) + 304 (data) = 8369 pulses, and the 1ms tail hold
     * completes the trailing edge of the 8369th pulse -> 8369 zero-crossings!
     */
    uint32_t edges1 = count_zero_crossings(pcm, total1);
    printf("[PASS] 0x10 Header + 0x20(0ms) auto-stopped cleanly after %u samples, %u edges (expected 8369)\n", total1, edges1);
    assert(edges1 == 8369);

    /* Resume with 48K STOP = ON -> should immediately trigger 0x2A 48K Stop! */
    tzx_resume(&eng);
    uint32_t got2 = 0;
    r = tzx_render(&eng, pcm, 256, &got2);
    assert(r == TZX_RENDER_AUTO_STOP);
    assert(eng.auto_stop_is_48k == 1);
    assert(got2 == 0);
    printf("[PASS] 0x2A Stop-if-48K triggered cleanly when 48K STOP=ON\n");

    /* Resume again -> plays 3x loop of 4x1000T pure tone + 0x11 Turbo block -> EOF! */
    tzx_resume(&eng);
    uint32_t total3 = 0;
    while (1) {
        uint32_t got = 0;
        r = tzx_render(&eng, pcm + total3, 256, &got);
        total3 += got;
        if (r != TZX_RENDER_CONT) break;
    }
    assert(r == TZX_RENDER_EOF);
    /* Expected pulses in part 3:
     * Loop 3x (4 pulses of 1000T) = 12 pulses
     * Turbo 0x11: 2 pilot + 2 sync + 16 data = 20 pulses + 1ms tail hold = 32 completed pulses -> 32 edges!
     */
    uint32_t edges3 = count_zero_crossings(pcm, total3);
    printf("[PASS] Loop(3x) + Turbo(0x11) rendered %u samples (expected 593), %u edges (expected 32)\n", total3, edges3);
    assert(edges3 == 32);
    assert(total3 >= 592 && total3 <= 594);

    /* Test 48K STOP = OFF bypasses 0x2A without stopping! */
    eng.stop_48k_enabled = 0;
    tzx_seek_block(&eng, 3);
    tzx_resume(&eng);
    uint32_t total4 = 0;
    while (1) {
        uint32_t got = 0;
        r = tzx_render(&eng, pcm + total4, 256, &got);
        total4 += got;
        if (r != TZX_RENDER_CONT) break;
    }
    assert(r == TZX_RENDER_EOF);
    assert(total4 == total3);
    assert(tzx_remaining_seconds(&eng) == 0);
    tzx_seek_block(&eng, 0);
    uint32_t rem0 = tzx_remaining_seconds(&eng);
    assert(rem0 >= 5 && rem0 <= 7);
    printf("[PASS] 0x2A bypassed seamlessly when 48K STOP=OFF! Countdown: %us -> 0s at EOF\n", rem0);

    free(pcm);
    printf("ALL TZX ENGINE TESTS PASSED!\n");
    return 0;
}
