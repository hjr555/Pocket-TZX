#ifndef TZX_ENGINE_H
#define TZX_ENGINE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ZX Spectrum clock = 3,500,000 Hz; ATJ2157 DAC rate = 48,000 Hz.
 * Exact ratio: 3,500,000 / 48,000 = 875 / 12 T-states per sample.
 */
#define TZX_ZX_CLOCK_HZ       3500000u
#define TZX_SAMPLE_RATE_HZ    48000u
#define TZX_RATIO_NUM         875u
#define TZX_RATIO_DEN         12u

#define TZX_MAX_BLOCKS        192
#define TZX_PCM_AMP           26000

typedef enum {
    TZX_OK = 0,
    TZX_ERR_IO = -1,
    TZX_ERR_MAGIC = -2,
    TZX_ERR_EMPTY = -3,
    TZX_ERR_CORRUPT = -4,
} tzx_err_t;

typedef enum {
    TZX_STATUS_STOPPED = 0,
    TZX_STATUS_PLAYING = 1,
    TZX_STATUS_PAUSED  = 2,
    TZX_STATUS_AUTO_STOP = 3,   /* Stopped by 0x20 (0ms) or 0x2A (48K Stop) */
    TZX_STATUS_EOF     = 4,
} tzx_status_t;

typedef enum {
    TZX_RENDER_CONT = 0,        /* Buffer filled, keep streaming */
    TZX_RENDER_AUTO_STOP = 1,   /* Hit 0x20(0ms) or 0x2A; caller should drain & pause */
    TZX_RENDER_EOF = 2,         /* Reached end of tape */
    TZX_RENDER_ERR = -1,
} tzx_render_res_t;

typedef int32_t (*tzx_read_fn)(void *io_ctx, uint32_t offset, void *buf, uint32_t len);

typedef struct {
    uint32_t offset;        /* Byte offset of block ID in .TZX file */
    uint16_t pause_ms;      /* Trailing pause in ms if applicable */
    uint16_t dur_ds;        /* Estimated block duration in 0.1s (100ms) units */
    uint8_t  id;            /* TZX block ID (0x10, 0x11, 0x20, 0x2A, etc.) */
    uint8_t  flags;         /* bit0: stop_tape, bit1: stop_48k, bit2: header */
    uint16_t _pad;
} tzx_block_info_t;

typedef enum {
    TZX_PHASE_IDLE = 0,
    TZX_PHASE_PILOT,
    TZX_PHASE_SYNC1,
    TZX_PHASE_SYNC2,
    TZX_PHASE_DATA_P0,
    TZX_PHASE_DATA_P1,
    TZX_PHASE_TONE,
    TZX_PHASE_PULSES,
    TZX_PHASE_DIRECT,
    TZX_PHASE_TAIL_1MS,
    TZX_PHASE_PAUSE,
} tzx_phase_t;

typedef struct {
    tzx_read_fn read_cb;
    void       *io_ctx;
    uint32_t    file_size;

    uint8_t     ver_major;
    uint8_t     ver_minor;

    uint16_t    block_count;
    uint16_t    cur_block;
    tzx_block_info_t blocks[TZX_MAX_BLOCKS];

    /* Config toggles */
    uint8_t     stop_48k_enabled;   /* Default 1 (ON) */
    uint8_t     invert_polarity;    /* Default 0 (NORM) */
    uint8_t     auto_stop_is_48k;   /* 1 if current auto-stop came from 0x2A */
    uint8_t     level;              /* Current square wave level (0 or 1) */
    uint8_t     silence_output;     /* 1 during silent pause after 1ms tail */

    tzx_status_t status;
    tzx_phase_t  phase;

    /* Exact rational T-state accumulator (units = 1/12th of a T-state) */
    uint32_t    sub_t_rem;          /* Remaining (T-states * 12) in current pulse */

    /* Active block parameters */
    uint16_t    pilot_len;
    uint16_t    sync1_len;
    uint16_t    sync2_len;
    uint16_t    zero_len;
    uint16_t    one_len;
    uint16_t    pilot_pulses_left;
    uint8_t     used_bits_last;
    uint16_t    pause_ms;

    uint32_t    data_file_pos;
    uint32_t    data_bytes_left;
    uint8_t     cur_byte;
    uint8_t     bits_left_in_byte;
    uint16_t    cur_bit_pulse;

    /* For 0x13 Pulse Sequence & 0x15 Direct Recording */
    uint16_t    seq_pulses_left;
    uint16_t    direct_t_per_sample;

    /* For 0x24 / 0x25 Loop */
    uint16_t    loop_start_block;
    uint16_t    loop_repeats_left;

    /* Cached read window for zero-syscall inner synthesis loop */
    uint8_t     cache_buf[512];
    uint32_t    cache_base;
    uint16_t    cache_len;

    /* Metadata extracted from ZX 0x10 header / 0x21 group / 0x30 text */
    char        tape_title[22];
    char        block_label[22];
    uint32_t    total_samples_rendered;
    uint32_t    block_samples_rendered;
    uint32_t    total_dur_ds;       /* Total estimated tape duration in 0.1s units */
} tzx_engine_t;

tzx_err_t tzx_open(tzx_engine_t *eng, tzx_read_fn read_cb, void *io_ctx, uint32_t file_size);
void      tzx_seek_block(tzx_engine_t *eng, uint16_t block_idx);
void      tzx_resume(tzx_engine_t *eng);
const char *tzx_block_type_short(uint8_t id);
uint32_t  tzx_remaining_ds(const tzx_engine_t *eng);
uint32_t  tzx_remaining_seconds(const tzx_engine_t *eng);

/* Synthesize up to `max_samples` 16-bit signed mono PCM samples into `out_pcm`.
 * Sets `*out_samples` to actual samples written (0..max_samples).
 */
tzx_render_res_t tzx_render(tzx_engine_t *eng, int16_t *out_pcm, uint32_t max_samples, uint32_t *out_samples);

#ifdef __cplusplus
}
#endif

#endif /* TZX_ENGINE_H */
