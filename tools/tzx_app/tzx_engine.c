#include "tzx_engine.h"

static void tzx_memzero(void *dst, size_t n) {
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = 0;
}

static void tzx_copy_clean_ascii(char *dst, size_t dst_cap, const uint8_t *src, size_t src_len) {
    size_t i = 0, o = 0;
    if (dst_cap == 0) return;
    while (i < src_len && o + 1 < dst_cap) {
        uint8_t c = src[i++];
        if (c == 0) break;
        if (c >= 0x20 && c <= 0x7e) {
            dst[o++] = (char)c;
        } else {
            dst[o++] = ' ';
        }
    }
    while (o > 0 && dst[o - 1] == ' ') o--;
    dst[o] = '\0';
}

static int tzx_read_cached(tzx_engine_t *eng, uint32_t pos, uint8_t *out, uint32_t len) {
    while (len > 0) {
        if (pos >= eng->file_size) return -1;
        if (eng->cache_len == 0 || pos < eng->cache_base || pos >= eng->cache_base + eng->cache_len) {
            uint32_t aligned = pos & ~511u;
            uint32_t to_read = 512u;
            if (aligned + to_read > eng->file_size) {
                to_read = eng->file_size - aligned;
            }
            int32_t got = eng->read_cb(eng->io_ctx, aligned, eng->cache_buf, to_read);
            if (got <= 0) return -1;
            eng->cache_base = aligned;
            eng->cache_len = (uint16_t)got;
            if (pos >= eng->cache_base + eng->cache_len) return -1;
        }
        uint32_t off_in_cache = pos - eng->cache_base;
        uint32_t avail = eng->cache_len - off_in_cache;
        uint32_t chunk = (len < avail) ? len : avail;
        for (uint32_t i = 0; i < chunk; i++) {
            out[i] = eng->cache_buf[off_in_cache + i];
        }
        pos += chunk;
        out += chunk;
        len -= chunk;
    }
    return 0;
}

static int tzx_read_u8(tzx_engine_t *eng, uint32_t pos, uint8_t *v) {
    return tzx_read_cached(eng, pos, v, 1);
}

static int tzx_read_u16(tzx_engine_t *eng, uint32_t pos, uint16_t *v) {
    uint8_t b[2];
    if (tzx_read_cached(eng, pos, b, 2) < 0) return -1;
    *v = (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
    return 0;
}

static int tzx_read_u24(tzx_engine_t *eng, uint32_t pos, uint32_t *v) {
    uint8_t b[3];
    if (tzx_read_cached(eng, pos, b, 3) < 0) return -1;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16);
    return 0;
}

static int tzx_read_u32(tzx_engine_t *eng, uint32_t pos, uint32_t *v) {
    uint8_t b[4];
    if (tzx_read_cached(eng, pos, b, 4) < 0) return -1;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

const char *tzx_block_type_short(uint8_t id) {
    switch (id) {
        case 0x10: return "STD DATA";
        case 0x11: return "TURBO";
        case 0x12: return "PURE TONE";
        case 0x13: return "PULSES";
        case 0x14: return "PURE DATA";
        case 0x15: return "DIRECT";
        case 0x20: return "PAUSE/STOP";
        case 0x21: return "GROUP";
        case 0x22: return "GRP END";
        case 0x24: return "LOOP";
        case 0x25: return "LOOP END";
        case 0x2A: return "48K STOP";
        case 0x2B: return "LEVEL";
        case 0x30: return "TEXT";
        case 0x32: return "INFO";
        default:   return "BLOCK";
    }
}

/* Pure 32-bit T-state duration helpers (avoids pulling in 64-bit __aeabi_uldivmod).
 * 3,500,000 T-states/s = 350,000 T-states per 0.1s (100ms).
 * For byte-counted data: 8 bits/byte -> (dlen * avg_t_per_bit) / 43750 = 0.1s units.
 */
static uint16_t tzx_clamp_ds(uint32_t ds) {
    return (ds > 65535u) ? 65535u : (uint16_t)ds;
}

static uint16_t tzx_data_dur_ds(uint32_t header_t, uint32_t dlen, uint32_t t_per_bit, uint16_t pause_ms) {
    /* Split dlen in chunks of <= 16384 bytes so dlen * t_per_bit never overflows uint32_t */
    uint32_t ds = (header_t + 175000u) / 350000u + ((uint32_t)pause_ms + 50u) / 100u;
    uint32_t rem_bytes = dlen;
    uint32_t rem_acc = 0;
    while (rem_bytes > 0) {
        uint32_t chunk = (rem_bytes > 16384u) ? 16384u : rem_bytes;
        uint32_t prod = chunk * t_per_bit;
        ds += prod / 43750u;
        rem_acc += prod % 43750u;
        if (rem_acc >= 43750u) {
            ds += rem_acc / 43750u;
            rem_acc %= 43750u;
        }
        rem_bytes -= chunk;
    }
    if (rem_acc >= 21875u) ds++;
    return tzx_clamp_ds(ds);
}

/* Compute exact byte length and estimated duration (in 0.1s units) of a TZX v1.20 block */
static int tzx_calc_block_len(tzx_engine_t *eng, uint32_t pos, uint8_t *out_id, uint16_t *out_pause, uint8_t *out_flags, uint32_t *out_len, uint16_t *out_dur_ds) {
    uint8_t id = 0;
    *out_pause = 0;
    *out_flags = 0;
    *out_dur_ds = 0;
    if (tzx_read_u8(eng, pos, &id) < 0) return -1;
    *out_id = id;

    switch (id) {
        case 0x10: { /* Standard Speed Data */
            uint16_t pause = 0, dlen = 0;
            uint8_t flag = 0xFF;
            if (tzx_read_u16(eng, pos + 1, &pause) < 0) return -1;
            if (tzx_read_u16(eng, pos + 3, &dlen) < 0) return -1;
            *out_pause = pause;
            if (dlen > 0) {
                (void)tzx_read_u8(eng, pos + 5, &flag);
            }
            if (dlen >= 19 && flag == 0x00) {
                *out_flags |= 0x04;
                if (eng->tape_title[0] == '\0') {
                    uint8_t hdr[11];
                    if (tzx_read_cached(eng, pos + 6, hdr, 11) == 0 && hdr[0] <= 3) {
                        tzx_copy_clean_ascii(eng->tape_title, sizeof(eng->tape_title), hdr + 1, 10);
                    }
                }
            }
            uint32_t pilot_t = (flag < 0x80u ? 8063u : 3223u) * 2168u + 667u + 735u;
            /* Avg bit = (2*855 + 2*1710)/2 = 2565 T-states */
            *out_dur_ds = tzx_data_dur_ds(pilot_t, dlen, 2565u, pause);
            *out_len = 5u + (uint32_t)dlen;
            return 0;
        }
        case 0x11: { /* Turbo Speed Data */
            uint16_t pilot_l = 0, sync1 = 0, sync2 = 0, zero_l = 0, one_l = 0, pilot_n = 0, pause = 0;
            uint32_t dlen = 0;
            if (tzx_read_u16(eng, pos + 1, &pilot_l) < 0) return -1;
            if (tzx_read_u16(eng, pos + 3, &sync1) < 0) return -1;
            if (tzx_read_u16(eng, pos + 5, &sync2) < 0) return -1;
            if (tzx_read_u16(eng, pos + 7, &zero_l) < 0) return -1;
            if (tzx_read_u16(eng, pos + 9, &one_l) < 0) return -1;
            if (tzx_read_u16(eng, pos + 11, &pilot_n) < 0) return -1;
            if (tzx_read_u16(eng, pos + 14, &pause) < 0) return -1;
            if (tzx_read_u24(eng, pos + 16, &dlen) < 0) return -1;
            *out_pause = pause;
            uint32_t hdr_t = (uint32_t)pilot_n * (uint32_t)pilot_l + sync1 + sync2;
            *out_dur_ds = tzx_data_dur_ds(hdr_t, dlen, (uint32_t)zero_l + (uint32_t)one_l, pause);
            *out_len = 19u + dlen;
            return 0;
        }
        case 0x12: { /* Pure Tone */
            uint16_t plen = 0, pcnt = 0;
            if (tzx_read_u16(eng, pos + 1, &plen) < 0) return -1;
            if (tzx_read_u16(eng, pos + 3, &pcnt) < 0) return -1;
            *out_dur_ds = tzx_data_dur_ds((uint32_t)plen * (uint32_t)pcnt, 0, 0, 0);
            *out_len = 5u;
            return 0;
        }
        case 0x13: { /* Pulse Sequence */
            uint8_t n = 0;
            if (tzx_read_u8(eng, pos + 1, &n) < 0) return -1;
            uint32_t sum_t = 0;
            for (uint8_t i = 0; i < n; i++) {
                uint16_t p = 0;
                if (tzx_read_u16(eng, pos + 2u + 2u * i, &p) == 0) sum_t += p;
            }
            *out_dur_ds = tzx_data_dur_ds(sum_t, 0, 0, 0);
            *out_len = 2u + 2u * (uint32_t)n;
            return 0;
        }
        case 0x14: { /* Pure Data */
            uint16_t zero_l = 0, one_l = 0, pause = 0;
            uint32_t dlen = 0;
            if (tzx_read_u16(eng, pos + 1, &zero_l) < 0) return -1;
            if (tzx_read_u16(eng, pos + 3, &one_l) < 0) return -1;
            if (tzx_read_u16(eng, pos + 6, &pause) < 0) return -1;
            if (tzx_read_u24(eng, pos + 8, &dlen) < 0) return -1;
            *out_pause = pause;
            *out_dur_ds = tzx_data_dur_ds(0, dlen, (uint32_t)zero_l + (uint32_t)one_l, pause);
            *out_len = 11u + dlen;
            return 0;
        }
        case 0x15: { /* Direct Recording */
            uint16_t t_per = 0, pause = 0;
            uint32_t dlen = 0;
            if (tzx_read_u16(eng, pos + 1, &t_per) < 0) return -1;
            if (tzx_read_u16(eng, pos + 3, &pause) < 0) return -1;
            if (tzx_read_u24(eng, pos + 6, &dlen) < 0) return -1;
            *out_pause = pause;
            *out_dur_ds = tzx_data_dur_ds(0, dlen, t_per, pause);
            *out_len = 9u + dlen;
            return 0;
        }
        case 0x18:
        case 0x19: { /* CSW / Generalized Data (4-byte block len at +1) */
            uint32_t blen = 0;
            if (tzx_read_u32(eng, pos + 1, &blen) < 0) return -1;
            *out_len = 5u + blen;
            return 0;
        }
        case 0x20: { /* Pause / Stop the Tape */
            uint16_t pause = 0;
            if (tzx_read_u16(eng, pos + 1, &pause) < 0) return -1;
            *out_pause = pause;
            if (pause == 0) *out_flags |= 0x01;
            else            *out_dur_ds = tzx_data_dur_ds(0, 0, 0, pause);
            *out_len = 3u;
            return 0;
        }
        case 0x21: /* Group Start */
        case 0x30: { /* Text Description */
            uint8_t slen = 0;
            if (tzx_read_u8(eng, pos + 1, &slen) < 0) return -1;
            if (id == 0x30 && eng->tape_title[0] == '\0' && slen > 0) {
                uint8_t tmp[20];
                uint8_t n = (slen < 20) ? slen : 20;
                if (tzx_read_cached(eng, pos + 2, tmp, n) == 0) {
                    tzx_copy_clean_ascii(eng->tape_title, sizeof(eng->tape_title), tmp, n);
                }
            }
            *out_len = 2u + (uint32_t)slen;
            return 0;
        }
        case 0x22: /* Group End */
        case 0x25: /* Loop End */
            *out_len = 1u;
            return 0;
        case 0x23: /* Jump to block */
        case 0x24: /* Loop Start */
            *out_len = 3u;
            return 0;
        case 0x26: { /* Call sequence */
            uint16_t cnt = 0;
            if (tzx_read_u16(eng, pos + 1, &cnt) < 0) return -1;
            *out_len = 3u + 2u * (uint32_t)cnt;
            return 0;
        }
        case 0x27: /* Return from sequence */
            *out_len = 1u;
            return 0;
        case 0x28: /* Select block */
        case 0x32: { /* Archive info */
            uint16_t blen = 0;
            if (tzx_read_u16(eng, pos + 1, &blen) < 0) return -1;
            *out_len = 3u + (uint32_t)blen;
            return 0;
        }
        case 0x2A: { /* Stop the tape if in 48K mode */
            uint32_t blen = 0;
            if (tzx_read_u32(eng, pos + 1, &blen) < 0) return -1;
            *out_flags |= 0x02;
            *out_len = 5u + blen;
            return 0;
        }
        case 0x2B: { /* Set signal level */
            uint32_t blen = 0;
            if (tzx_read_u32(eng, pos + 1, &blen) < 0) return -1;
            *out_len = 5u + blen;
            return 0;
        }
        case 0x31: { /* Message block */
            uint8_t slen = 0;
            if (tzx_read_u8(eng, pos + 2, &slen) < 0) return -1;
            *out_len = 3u + (uint32_t)slen;
            return 0;
        }
        case 0x33: { /* Hardware type */
            uint8_t n = 0;
            if (tzx_read_u8(eng, pos + 1, &n) < 0) return -1;
            *out_len = 2u + 3u * (uint32_t)n;
            return 0;
        }
        case 0x35: { /* Custom info */
            uint32_t clen = 0;
            if (tzx_read_u32(eng, pos + 17, &clen) < 0) return -1;
            *out_len = 21u + clen;
            return 0;
        }
        case 0x5A: /* Glue block ("XTape!\x1a" + ver) */
            *out_len = 10u;
            return 0;
        default: {
            /* Generic TZX v1.20 extension rule: 4-byte length at +1 */
            uint32_t blen = 0;
            if (tzx_read_u32(eng, pos + 1, &blen) < 0) return -1;
            *out_len = 5u + blen;
            return 0;
        }
    }
}

tzx_err_t tzx_open(tzx_engine_t *eng, tzx_read_fn read_cb, void *io_ctx, uint32_t file_size) {
    uint8_t hdr[10];
    uint8_t keep_48k = eng->stop_48k_enabled;
    uint8_t keep_inv = eng->invert_polarity;

    tzx_memzero(eng, sizeof(*eng));
    eng->read_cb = read_cb;
    eng->io_ctx = io_ctx;
    eng->file_size = file_size;
    eng->stop_48k_enabled = keep_48k;
    eng->invert_polarity = keep_inv;

    if (file_size < 10) return TZX_ERR_MAGIC;
    if (tzx_read_cached(eng, 0, hdr, 10) < 0) return TZX_ERR_IO;
    if (hdr[0] != 'Z' || hdr[1] != 'X' || hdr[2] != 'T' || hdr[3] != 'a' ||
        hdr[4] != 'p' || hdr[5] != 'e' || hdr[6] != '!' || hdr[7] != 0x1A) {
        return TZX_ERR_MAGIC;
    }
    eng->ver_major = hdr[8];
    eng->ver_minor = hdr[9];

    uint32_t pos = 10;
    eng->total_dur_ds = 0;
    while (pos < file_size && eng->block_count < TZX_MAX_BLOCKS) {
        uint8_t id = 0, flags = 0;
        uint16_t pause = 0, dur_ds = 0;
        uint32_t blen = 0;
        if (tzx_calc_block_len(eng, pos, &id, &pause, &flags, &blen, &dur_ds) < 0) {
            break;
        }
        if (blen == 0 || pos + blen > file_size) {
            break;
        }
        tzx_block_info_t *bi = &eng->blocks[eng->block_count++];
        bi->offset = pos;
        bi->id = id;
        bi->flags = flags;
        bi->pause_ms = pause;
        bi->dur_ds = dur_ds;
        eng->total_dur_ds += dur_ds;
        pos += blen;
    }

    if (eng->block_count == 0) return TZX_ERR_EMPTY;
    tzx_seek_block(eng, 0);
    eng->status = TZX_STATUS_PLAYING;
    return TZX_OK;
}

static void tzx_set_label_from_str(char *dst, const char *src) {
    size_t i = 0;
    while (src[i] && i + 1 < 22) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int tzx_load_first_data_byte(tzx_engine_t *eng) {
    if (eng->data_bytes_left == 0) return -1;
    if (tzx_read_u8(eng, eng->data_file_pos, &eng->cur_byte) < 0) return -1;
    eng->data_file_pos++;
    eng->data_bytes_left--;
    eng->bits_left_in_byte = (eng->data_bytes_left == 0) ? eng->used_bits_last : 8;
    if (eng->bits_left_in_byte == 0 || eng->bits_left_in_byte > 8) eng->bits_left_in_byte = 8;
    eng->cur_bit_pulse = (eng->cur_byte & 0x80) ? eng->one_len : eng->zero_len;
    eng->cur_byte <<= 1;
    eng->bits_left_in_byte--;
    return 0;
}

/* Prepare the current block (`eng->cur_block`) or advance past non-audio metadata blocks.
 * Returns:
 *   0 = active audio/pause phase armed (`eng->sub_t_rem > 0`)
 *   1 = auto-stop triggered (0x20 0ms or 0x2A with 48K stop ON)
 *   2 = EOF reached
 *  -1 = error
 */
static int tzx_arm_next_phase_or_block(tzx_engine_t *eng) {
    uint16_t guard = 0;
    while (eng->cur_block < eng->block_count && guard++ < 1024) {
        tzx_block_info_t *bi = &eng->blocks[eng->cur_block];
        uint32_t pos = bi->offset;
        uint8_t id = bi->id;
        eng->block_samples_rendered = 0;

        tzx_set_label_from_str(eng->block_label, tzx_block_type_short(id));

        switch (id) {
            case 0x10: { /* Standard Speed Data */
                uint16_t dlen = 0;
                uint8_t flag = 0;
                if (tzx_read_u16(eng, pos + 1, &eng->pause_ms) < 0) return -1;
                if (tzx_read_u16(eng, pos + 3, &dlen) < 0) return -1;
                if (dlen > 0) tzx_read_u8(eng, pos + 5, &flag);
                if (dlen >= 19 && flag == 0x00) {
                    uint8_t hdr[11];
                    if (tzx_read_cached(eng, pos + 6, hdr, 11) == 0 && hdr[0] <= 3) {
                        tzx_copy_clean_ascii(eng->block_label, sizeof(eng->block_label), hdr + 1, 10);
                    }
                }
                eng->pilot_len = 2168;
                eng->sync1_len = 667;
                eng->sync2_len = 735;
                eng->zero_len  = 855;
                eng->one_len   = 1710;
                eng->pilot_pulses_left = (flag < 0x80) ? 8063 : 3223;
                eng->used_bits_last = 8;
                eng->data_file_pos = pos + 5;
                eng->data_bytes_left = dlen;
                eng->silence_output = 0;
                eng->phase = TZX_PHASE_PILOT;
                eng->sub_t_rem = (uint32_t)eng->pilot_len * TZX_RATIO_DEN;
                eng->pilot_pulses_left--;
                return 0;
            }
            case 0x11: { /* Turbo Speed Data */
                uint32_t dlen = 0;
                uint8_t ub = 8;
                if (tzx_read_u16(eng, pos + 1, &eng->pilot_len) < 0) return -1;
                if (tzx_read_u16(eng, pos + 3, &eng->sync1_len) < 0) return -1;
                if (tzx_read_u16(eng, pos + 5, &eng->sync2_len) < 0) return -1;
                if (tzx_read_u16(eng, pos + 7, &eng->zero_len) < 0) return -1;
                if (tzx_read_u16(eng, pos + 9, &eng->one_len) < 0) return -1;
                if (tzx_read_u16(eng, pos + 11, &eng->pilot_pulses_left) < 0) return -1;
                if (tzx_read_u8(eng, pos + 13, &ub) < 0) return -1;
                if (tzx_read_u16(eng, pos + 14, &eng->pause_ms) < 0) return -1;
                if (tzx_read_u24(eng, pos + 16, &dlen) < 0) return -1;
                eng->used_bits_last = ub;
                eng->data_file_pos = pos + 19;
                eng->data_bytes_left = dlen;
                eng->silence_output = 0;
                if (eng->pilot_pulses_left > 0 && eng->pilot_len > 0) {
                    eng->phase = TZX_PHASE_PILOT;
                    eng->sub_t_rem = (uint32_t)eng->pilot_len * TZX_RATIO_DEN;
                    eng->pilot_pulses_left--;
                    return 0;
                } else if (eng->sync1_len > 0) {
                    eng->phase = TZX_PHASE_SYNC1;
                    eng->sub_t_rem = (uint32_t)eng->sync1_len * TZX_RATIO_DEN;
                    return 0;
                } else if (dlen > 0 && tzx_load_first_data_byte(eng) == 0) {
                    eng->phase = TZX_PHASE_DATA_P0;
                    eng->sub_t_rem = (uint32_t)eng->cur_bit_pulse * TZX_RATIO_DEN;
                    return 0;
                }
                eng->cur_block++;
                break;
            }
            case 0x12: { /* Pure Tone */
                uint16_t plen = 0, npulses = 0;
                if (tzx_read_u16(eng, pos + 1, &plen) < 0) return -1;
                if (tzx_read_u16(eng, pos + 3, &npulses) < 0) return -1;
                if (plen > 0 && npulses > 0) {
                    eng->pilot_len = plen;
                    eng->pilot_pulses_left = npulses - 1;
                    eng->silence_output = 0;
                    eng->phase = TZX_PHASE_TONE;
                    eng->sub_t_rem = (uint32_t)plen * TZX_RATIO_DEN;
                    return 0;
                }
                eng->cur_block++;
                break;
            }
            case 0x13: { /* Pulse Sequence */
                uint8_t n = 0;
                uint16_t p0 = 0;
                if (tzx_read_u8(eng, pos + 1, &n) < 0) return -1;
                if (n > 0 && tzx_read_u16(eng, pos + 2, &p0) == 0) {
                    eng->seq_pulses_left = n - 1;
                    eng->data_file_pos = pos + 4;
                    eng->silence_output = 0;
                    eng->phase = TZX_PHASE_PULSES;
                    eng->sub_t_rem = (uint32_t)(p0 ? p0 : 1) * TZX_RATIO_DEN;
                    return 0;
                }
                eng->cur_block++;
                break;
            }
            case 0x14: { /* Pure Data */
                uint8_t ub = 8;
                uint32_t dlen = 0;
                if (tzx_read_u16(eng, pos + 1, &eng->zero_len) < 0) return -1;
                if (tzx_read_u16(eng, pos + 3, &eng->one_len) < 0) return -1;
                if (tzx_read_u8(eng, pos + 5, &ub) < 0) return -1;
                if (tzx_read_u16(eng, pos + 6, &eng->pause_ms) < 0) return -1;
                if (tzx_read_u24(eng, pos + 8, &dlen) < 0) return -1;
                eng->used_bits_last = ub;
                eng->data_file_pos = pos + 11;
                eng->data_bytes_left = dlen;
                eng->silence_output = 0;
                if (dlen > 0 && tzx_load_first_data_byte(eng) == 0) {
                    eng->phase = TZX_PHASE_DATA_P0;
                    eng->sub_t_rem = (uint32_t)eng->cur_bit_pulse * TZX_RATIO_DEN;
                    return 0;
                } else if (eng->pause_ms > 0) {
                    eng->phase = TZX_PHASE_TAIL_1MS;
                    eng->sub_t_rem = 3500u * TZX_RATIO_DEN;
                    return 0;
                }
                eng->cur_block++;
                break;
            }
            case 0x15: { /* Direct Recording */
                uint8_t ub = 8;
                uint32_t dlen = 0;
                if (tzx_read_u16(eng, pos + 1, &eng->direct_t_per_sample) < 0) return -1;
                if (tzx_read_u16(eng, pos + 3, &eng->pause_ms) < 0) return -1;
                if (tzx_read_u8(eng, pos + 5, &ub) < 0) return -1;
                if (tzx_read_u24(eng, pos + 6, &dlen) < 0) return -1;
                if (eng->direct_t_per_sample == 0) eng->direct_t_per_sample = 79;
                eng->used_bits_last = ub;
                eng->data_file_pos = pos + 9;
                eng->data_bytes_left = dlen;
                if (dlen > 0 && tzx_read_u8(eng, eng->data_file_pos, &eng->cur_byte) == 0) {
                    eng->data_file_pos++;
                    eng->data_bytes_left--;
                    eng->bits_left_in_byte = (eng->data_bytes_left == 0) ? ub : 8;
                    if (eng->bits_left_in_byte == 0 || eng->bits_left_in_byte > 8) eng->bits_left_in_byte = 8;
                    eng->level = (eng->cur_byte & 0x80) ? 1 : 0;
                    eng->cur_byte <<= 1;
                    eng->bits_left_in_byte--;
                    eng->silence_output = 0;
                    eng->phase = TZX_PHASE_DIRECT;
                    eng->sub_t_rem = (uint32_t)eng->direct_t_per_sample * TZX_RATIO_DEN;
                    return 0;
                }
                eng->cur_block++;
                break;
            }
            case 0x20: { /* Pause (Silence) or 'Stop the Tape' */
                uint16_t pms = 0;
                if (tzx_read_u16(eng, pos + 1, &pms) < 0) return -1;
                if (pms == 0) {
                    /* Stop the Tape! Advance cur_block to next block and pause */
                    eng->cur_block++;
                    eng->phase = TZX_PHASE_IDLE;
                    eng->sub_t_rem = 0;
                    eng->silence_output = 1;
                    eng->level = 0;
                    eng->auto_stop_is_48k = 0;
                    eng->status = TZX_STATUS_AUTO_STOP;
                    return 1;
                } else {
                    eng->pause_ms = pms;
                    eng->silence_output = 0;
                    eng->phase = TZX_PHASE_TAIL_1MS;
                    eng->sub_t_rem = 3500u * TZX_RATIO_DEN;
                    return 0;
                }
            }
            case 0x21: { /* Group Start */
                uint8_t slen = 0;
                if (tzx_read_u8(eng, pos + 1, &slen) == 0 && slen > 0) {
                    uint8_t tmp[20];
                    uint8_t n = (slen < 20) ? slen : 20;
                    if (tzx_read_cached(eng, pos + 2, tmp, n) == 0) {
                        tzx_copy_clean_ascii(eng->block_label, sizeof(eng->block_label), tmp, n);
                    }
                }
                eng->cur_block++;
                break;
            }
            case 0x24: { /* Loop Start */
                uint16_t reps = 0;
                if (tzx_read_u16(eng, pos + 1, &reps) == 0 && reps > 1) {
                    eng->loop_start_block = eng->cur_block + 1;
                    eng->loop_repeats_left = reps - 1;
                }
                eng->cur_block++;
                break;
            }
            case 0x25: { /* Loop End */
                if (eng->loop_repeats_left > 0 && eng->loop_start_block < eng->block_count) {
                    eng->loop_repeats_left--;
                    eng->cur_block = eng->loop_start_block;
                } else {
                    eng->cur_block++;
                }
                break;
            }
            case 0x2A: { /* Stop the tape if in 48K mode */
                eng->cur_block++;
                if (eng->stop_48k_enabled) {
                    eng->phase = TZX_PHASE_IDLE;
                    eng->sub_t_rem = 0;
                    eng->silence_output = 1;
                    eng->level = 0;
                    eng->auto_stop_is_48k = 1;
                    eng->status = TZX_STATUS_AUTO_STOP;
                    return 1;
                }
                break;
            }
            case 0x2B: { /* Set Signal Level */
                uint8_t lvl = 0;
                if (tzx_read_u8(eng, pos + 5, &lvl) == 0) {
                    eng->level = lvl ? 1 : 0;
                }
                eng->cur_block++;
                break;
            }
            default:
                eng->cur_block++;
                break;
        }
    }

    eng->phase = TZX_PHASE_IDLE;
    eng->sub_t_rem = 0;
    eng->silence_output = 1;
    eng->status = TZX_STATUS_EOF;
    return 2;
}

/* Advance at pulse boundary inside an active block */
static int tzx_step_pulse_end(tzx_engine_t *eng) {
    switch (eng->phase) {
        case TZX_PHASE_PILOT:
            eng->level ^= 1;
            if (eng->pilot_pulses_left > 0) {
                eng->pilot_pulses_left--;
                eng->sub_t_rem += (uint32_t)eng->pilot_len * TZX_RATIO_DEN;
                return 0;
            }
            if (eng->sync1_len > 0) {
                eng->phase = TZX_PHASE_SYNC1;
                eng->sub_t_rem += (uint32_t)eng->sync1_len * TZX_RATIO_DEN;
                return 0;
            }
            /* fallthrough if sync1==0 */
            eng->phase = TZX_PHASE_SYNC2;
            eng->sub_t_rem += (uint32_t)(eng->sync2_len ? eng->sync2_len : 1) * TZX_RATIO_DEN;
            return 0;

        case TZX_PHASE_SYNC1:
            eng->level ^= 1;
            if (eng->sync2_len > 0) {
                eng->phase = TZX_PHASE_SYNC2;
                eng->sub_t_rem += (uint32_t)eng->sync2_len * TZX_RATIO_DEN;
                return 0;
            }
            /* fallthrough */
        case TZX_PHASE_SYNC2:
            eng->level ^= 1;
            if (eng->data_bytes_left > 0 && tzx_load_first_data_byte(eng) == 0) {
                eng->phase = TZX_PHASE_DATA_P0;
                eng->sub_t_rem += (uint32_t)eng->cur_bit_pulse * TZX_RATIO_DEN;
                return 0;
            }
            if (eng->pause_ms > 0) {
                eng->phase = TZX_PHASE_TAIL_1MS;
                eng->sub_t_rem += 3500u * TZX_RATIO_DEN;
                return 0;
            }
            eng->cur_block++;
            return tzx_arm_next_phase_or_block(eng);

        case TZX_PHASE_DATA_P0:
            eng->level ^= 1;
            eng->phase = TZX_PHASE_DATA_P1;
            eng->sub_t_rem += (uint32_t)eng->cur_bit_pulse * TZX_RATIO_DEN;
            return 0;

        case TZX_PHASE_DATA_P1:
            eng->level ^= 1;
            if (eng->bits_left_in_byte > 0) {
                eng->cur_bit_pulse = (eng->cur_byte & 0x80) ? eng->one_len : eng->zero_len;
                eng->cur_byte <<= 1;
                eng->bits_left_in_byte--;
                eng->phase = TZX_PHASE_DATA_P0;
                eng->sub_t_rem += (uint32_t)eng->cur_bit_pulse * TZX_RATIO_DEN;
                return 0;
            }
            if (eng->data_bytes_left > 0 && tzx_load_first_data_byte(eng) == 0) {
                eng->phase = TZX_PHASE_DATA_P0;
                eng->sub_t_rem += (uint32_t)eng->cur_bit_pulse * TZX_RATIO_DEN;
                return 0;
            }
            if (eng->pause_ms > 0) {
                eng->phase = TZX_PHASE_TAIL_1MS;
                eng->sub_t_rem += 3500u * TZX_RATIO_DEN;
                return 0;
            }
            eng->cur_block++;
            return tzx_arm_next_phase_or_block(eng);

        case TZX_PHASE_TONE:
            eng->level ^= 1;
            if (eng->pilot_pulses_left > 0) {
                eng->pilot_pulses_left--;
                eng->sub_t_rem += (uint32_t)eng->pilot_len * TZX_RATIO_DEN;
                return 0;
            }
            eng->cur_block++;
            return tzx_arm_next_phase_or_block(eng);

        case TZX_PHASE_PULSES:
            eng->level ^= 1;
            if (eng->seq_pulses_left > 0) {
                uint16_t p = 0;
                if (tzx_read_u16(eng, eng->data_file_pos, &p) < 0) return -1;
                eng->data_file_pos += 2;
                eng->seq_pulses_left--;
                eng->sub_t_rem += (uint32_t)(p ? p : 1) * TZX_RATIO_DEN;
                return 0;
            }
            eng->cur_block++;
            return tzx_arm_next_phase_or_block(eng);

        case TZX_PHASE_DIRECT:
            if (eng->bits_left_in_byte > 0) {
                eng->level = (eng->cur_byte & 0x80) ? 1 : 0;
                eng->cur_byte <<= 1;
                eng->bits_left_in_byte--;
                eng->sub_t_rem += (uint32_t)eng->direct_t_per_sample * TZX_RATIO_DEN;
                return 0;
            }
            if (eng->data_bytes_left > 0) {
                if (tzx_read_u8(eng, eng->data_file_pos, &eng->cur_byte) < 0) return -1;
                eng->data_file_pos++;
                eng->data_bytes_left--;
                eng->bits_left_in_byte = (eng->data_bytes_left == 0) ? eng->used_bits_last : 8;
                if (eng->bits_left_in_byte == 0 || eng->bits_left_in_byte > 8) eng->bits_left_in_byte = 8;
                eng->level = (eng->cur_byte & 0x80) ? 1 : 0;
                eng->cur_byte <<= 1;
                eng->bits_left_in_byte--;
                eng->sub_t_rem += (uint32_t)eng->direct_t_per_sample * TZX_RATIO_DEN;
                return 0;
            }
            if (eng->pause_ms > 0) {
                eng->silence_output = 1;
                eng->level = 0;
                eng->phase = TZX_PHASE_PAUSE;
                eng->sub_t_rem += (uint32_t)eng->pause_ms * 3500u * TZX_RATIO_DEN;
                return 0;
            }
            eng->cur_block++;
            return tzx_arm_next_phase_or_block(eng);

        case TZX_PHASE_TAIL_1MS:
            /* After holding opposite edge for 1ms to finish last bit cleanly, drop to 0 silence */
            eng->level = 0;
            eng->silence_output = 1;
            if (eng->pause_ms > 1) {
                uint32_t rem_ms = (uint32_t)(eng->pause_ms - 1);
                /* Cap single sub_t_rem chunk to avoid uint32 overflow (> 95000 ms) */
                if (rem_ms > 60000u) rem_ms = 60000u;
                eng->phase = TZX_PHASE_PAUSE;
                eng->sub_t_rem += rem_ms * 3500u * TZX_RATIO_DEN;
                return 0;
            }
            eng->cur_block++;
            return tzx_arm_next_phase_or_block(eng);

        case TZX_PHASE_PAUSE:
        case TZX_PHASE_IDLE:
        default:
            eng->silence_output = 0;
            eng->cur_block++;
            return tzx_arm_next_phase_or_block(eng);
    }
}

void tzx_seek_block(tzx_engine_t *eng, uint16_t block_idx) {
    if (eng->block_count == 0) return;
    if (block_idx >= eng->block_count) block_idx = eng->block_count - 1;
    eng->cur_block = block_idx;
    eng->phase = TZX_PHASE_IDLE;
    eng->sub_t_rem = 0;
    eng->level = 0;
    eng->silence_output = 0;
    eng->loop_repeats_left = 0;
    eng->auto_stop_is_48k = 0;
    eng->block_samples_rendered = 0;
    tzx_set_label_from_str(eng->block_label, tzx_block_type_short(eng->blocks[block_idx].id));
    if (eng->status == TZX_STATUS_EOF || eng->status == TZX_STATUS_AUTO_STOP) {
        eng->status = TZX_STATUS_PAUSED;
    }
}

uint32_t tzx_remaining_ds(const tzx_engine_t *eng) {
    if (eng->status == TZX_STATUS_EOF || eng->cur_block >= eng->block_count) {
        return 0;
    }
    uint32_t cur_dur = eng->blocks[eng->cur_block].dur_ds;
    uint32_t cur_elapsed = (eng->block_samples_rendered * 10u) / TZX_SAMPLE_RATE_HZ;
    uint32_t rem = (cur_elapsed < cur_dur) ? (cur_dur - cur_elapsed) : 0u;
    for (uint16_t i = (uint16_t)(eng->cur_block + 1u); i < eng->block_count; i++) {
        rem += eng->blocks[i].dur_ds;
    }
    return rem;
}

uint32_t tzx_remaining_seconds(const tzx_engine_t *eng) {
    uint32_t ds = tzx_remaining_ds(eng);
    return (ds + 9u) / 10u;
}

void tzx_resume(tzx_engine_t *eng) {
    if (eng->status == TZX_STATUS_EOF && eng->cur_block >= eng->block_count) {
        tzx_seek_block(eng, 0);
    }
    eng->auto_stop_is_48k = 0;
    eng->status = TZX_STATUS_PLAYING;
}

tzx_render_res_t tzx_render(tzx_engine_t *eng, int16_t *out_pcm, uint32_t max_samples, uint32_t *out_samples) {
    uint32_t n = 0;
    *out_samples = 0;

    if (eng->status != TZX_STATUS_PLAYING) {
        return (eng->status == TZX_STATUS_EOF) ? TZX_RENDER_EOF : TZX_RENDER_AUTO_STOP;
    }

    if (eng->phase == TZX_PHASE_IDLE) {
        int rc = tzx_arm_next_phase_or_block(eng);
        if (rc == 1) return TZX_RENDER_AUTO_STOP;
        if (rc == 2) return TZX_RENDER_EOF;
        if (rc < 0)  return TZX_RENDER_ERR;
    }

    while (n < max_samples) {
        int16_t sample;
        if (eng->silence_output) {
            sample = 0;
        } else {
            uint8_t eff_lvl = eng->level ^ (eng->invert_polarity ? 1u : 0u);
            sample = eff_lvl ? TZX_PCM_AMP : -TZX_PCM_AMP;
        }
        out_pcm[n++] = sample;
        eng->total_samples_rendered++;
        eng->block_samples_rendered++;

        if (eng->sub_t_rem > TZX_RATIO_NUM) {
            eng->sub_t_rem -= TZX_RATIO_NUM;
        } else {
            /* Current pulse expires within this sample step; carry fractional deficit! */
            uint32_t deficit = TZX_RATIO_NUM - eng->sub_t_rem;
            eng->sub_t_rem = 0;
            int rc = 0;
            while (eng->sub_t_rem <= deficit) {
                deficit -= eng->sub_t_rem;
                eng->sub_t_rem = 0;
                rc = tzx_step_pulse_end(eng);
                if (rc != 0) break;
            }
            if (rc == 0) {
                eng->sub_t_rem -= deficit;
            } else {
                *out_samples = n;
                if (rc == 1) return TZX_RENDER_AUTO_STOP;
                if (rc == 2) return TZX_RENDER_EOF;
                return TZX_RENDER_ERR;
            }
        }
    }

    *out_samples = n;
    return TZX_RENDER_CONT;
}
