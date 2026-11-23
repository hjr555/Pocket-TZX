#include "atj2157_api.h"
#include "tzx_engine.h"
#include "tzx_font.h"

/* =========================================================================
 * Section 1: Exact ATJ2157 Foreground .AP CRT0 & SVC Trampolines
 * ========================================================================= */

extern uint8_t __bss_start[];
extern uint8_t __bss_end[];

int tzx_app_main(int argc);

/* Matches TOOLS.AP / RECORD.AP stack & priority in SRAM bank 0x00200000 */
uint32_t *g_ptos = (uint32_t *)0x00206AC0u;
uint8_t   g_prio = 0x07u;

void _thread_entry(int argc) {
    /* Actions kernel .AP loader does NOT zero .bss automatically */
    for (uint8_t *p = __bss_start; p < __bss_end; p++) {
        *p = 0;
    }
    int rc = tzx_app_main(argc);
    libc_op_entry((void *)(intptr_t)(int8_t)rc, (void *)0, (void *)0, LIBC_EXIT);
    while (1) {}
}

__attribute__((section(".text.crt0")))
int _start(int argc, void *argv) {
    int32_t proc_raw = (int32_t)(intptr_t)libc_op_entry(
        (void *)(intptr_t)argc, argv, (void *)0, LIBC_GET_PROC_STRUCT
    );
    int8_t proc_idx = (int8_t)proc_raw;
    if (proc_idx < 0) {
        return -1;
    }

    uint32_t pthread_param[3];
    pthread_param[0] = ((uint32_t)(uintptr_t)&_thread_entry) | 1u;
    pthread_param[1] = (uint32_t)argc;
    pthread_param[2] = (uint32_t)(uintptr_t)g_ptos;

    sys_op_entry((void *)0, (void *)0, (void *)0, KERN_OS_SCHED_LOCK);
    int32_t rc = (int32_t)(intptr_t)libc_op_entry(
        (void *)pthread_param,
        (void *)(uintptr_t)g_prio,
        (void *)(intptr_t)proc_idx,
        LIBC_PTHREAD_CREATE
    );
    if (rc < 0) {
        libc_op_entry((void *)(intptr_t)proc_idx, (void *)0, (void *)0, LIBC_FREE_PROC_STRUCT);
        sys_op_entry((void *)0, (void *)0, (void *)0, KERN_OS_SCHED_UNLOCK);
        return -1;
    }
    sys_op_entry((void *)0, (void *)0, (void *)0, KERN_OS_SCHED_UNLOCK);
    return 0;
}

#define UNUSED __attribute__((unused))

__attribute__((naked)) void *sys_op_entry(UNUSED void *p1, UNUSED void *p2, UNUSED void *p3, UNUSED uint32_t cmd) {
    __asm__ volatile ("svc #0x0f\n\tbx lr\n");
}

__attribute__((naked)) void *libc_op_entry(UNUSED void *p1, UNUSED void *p2, UNUSED void *p3, UNUSED uint32_t cmd) {
    __asm__ volatile ("svc #0x10\n\tbx lr\n");
}

__attribute__((naked)) void *audio_device_op_entry(UNUSED void *p1, UNUSED void *p2, UNUSED void *p3, UNUSED uint32_t cmd) {
    __asm__ volatile ("svc #0x0b\n\tbx lr\n");
}

__attribute__((naked)) void *lcd_op_entry(UNUSED void *p1, UNUSED void *p2, UNUSED void *p3, UNUSED uint32_t cmd) {
    __asm__ volatile ("svc #0x07\n\tbx lr\n");
}

__attribute__((naked)) void *key_op_entry(UNUSED void *p1, UNUSED void *p2, UNUSED void *p3, UNUSED uint32_t cmd) {
    __asm__ volatile ("svc #0x09\n\tbx lr\n");
}

__attribute__((naked)) void *vfs_op_entry(UNUSED void *mnt, UNUSED void *p1, UNUSED void *p2, UNUSED void *p3, UNUSED uint32_t cmd) {
    __asm__ volatile ("svc #0x11\n\tbx lr\n");
}

/* Provide compiler-rt / freestanding memcpy & memset if emitted by clang */
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

/* =========================================================================
 * Section 2: 128x64 Monochrome Framebuffer & DRV_LCD Flush
 * ========================================================================= */

static uint8_t  g_fb[128 * 8];      /* 1024 B: 8 vertical pages x 128 columns */
static uint16_t g_lcd_strip[128 * 8]; /* 2048 B scratch strip (128x8; DRV_LCD requires height % 8 == 0) */

static void fb_clear(void) {
    memset(g_fb, 0, sizeof(g_fb));
}

static void fb_set_px(int x, int y, int on) {
    if ((unsigned)x >= 128u || (unsigned)y >= 64u) return;
    uint8_t mask = (uint8_t)(1u << (y & 7));
    if (on) g_fb[(y >> 3) * 128 + x] |= mask;
    else    g_fb[(y >> 3) * 128 + x] &= (uint8_t)~mask;
}

static void fb_fill_rect(int x0, int y0, int w, int h, int on) {
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            fb_set_px(x, y, on);
        }
    }
}

static void fb_draw_rect(int x0, int y0, int w, int h, int on) {
    for (int x = x0; x < x0 + w; x++) {
        fb_set_px(x, y0, on);
        fb_set_px(x, y0 + h - 1, on);
    }
    for (int y = y0; y < y0 + h; y++) {
        fb_set_px(x0, y, on);
        fb_set_px(x0 + w - 1, y, on);
    }
}

static void fb_draw_char(int x, int y, char c, int invert) {
    uint8_t uc = (uint8_t)c;
    if (uc < 0x20u || uc > 0x7Eu) uc = '?';
    const uint8_t *glyph = g_font5x7[uc - 0x20u];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            int lit = (bits >> row) & 1;
            fb_set_px(x + col, y + row, invert ? !lit : lit);
        }
    }
    if (invert) {
        for (int row = 0; row < 7; row++) fb_set_px(x + 5, y + row, 1);
    }
}

static void fb_draw_str(int x, int y, const char *s, int max_chars, int invert) {
    int n = 0;
    while (*s && n < max_chars && x + 5 <= 128) {
        fb_draw_char(x, y, *s++, invert);
        x += 6;
        n++;
    }
}

static void fb_flush(void) {
    /* Push 8 strips of 128x8 uint16 pixels through DRV_LCD.DRV (0x0000=lit, 0xFFFF=unlit).
     * DRV_LCD.DRV lcd_flush_128x64 computes pages = height >> 3, so height MUST be 8. */
    for (uint16_t page = 0; page < 8; page++) {
        const uint8_t *src = &g_fb[page * 128];
        for (uint16_t py = 0; py < 8; py++) {
            uint8_t bit = (uint8_t)(1u << py);
            uint16_t *dst_row = &g_lcd_strip[py * 128];
            for (uint16_t x = 0; x < 128; x++) {
                dst_row[x] = (src[x] & bit) ? 0x0000u : 0xFFFFu;
            }
        }
        lcd_region_t rgn;
        rgn.x = 0;
        rgn.y = (uint16_t)(page * 8u);
        rgn.width = 128;
        rgn.height = 8;
        lcd_op_entry(&rgn, (void *)0, (void *)0, LCD_CMD_SET_WINDOW);
        lcd_op_entry(g_lcd_strip, (void *)(128u * 8u), (void *)0, LCD_CMD_TRANS_BUFFDATA);
    }
}

/* =========================================================================
 * Section 3: Formatting Helpers
 * ========================================================================= */

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static char to_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static int str_casecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = to_upper(*a++);
        char cb = to_upper(*b++);
        if (ca != cb) return (int)(uint8_t)ca - (int)(uint8_t)cb;
    }
    return (int)(uint8_t)to_upper(*a) - (int)(uint8_t)to_upper(*b);
}

static void fmt_u2(char *dst, uint32_t v) {
    if (v > 99) v = 99;
    dst[0] = (char)('0' + (v / 10u));
    dst[1] = (char)('0' + (v % 10u));
}

static void fmt_u3(char *dst, uint32_t v) {
    if (v > 999) v = 999;
    dst[0] = (char)('0' + (v / 100u));
    dst[1] = (char)('0' + ((v / 10u) % 10u));
    dst[2] = (char)('0' + (v % 10u));
}

/* =========================================================================
 * Section 4: Audio Subsystem (AUD_DEV.DRV 48 kHz Stereo L+R DAC DMA)
 * ========================================================================= */

/* Exact MENGINE.AP 32-step DAC digital gain curve (0..31 -> 0..191) */
static const uint8_t g_dac_vol_tbl[32] = {
    0, 47, 55, 63, 71, 79, 87, 95,
    111, 119, 127, 132, 135, 138, 143, 146,
    148, 151, 154, 156, 159, 160, 162, 164,
    166, 168, 170, 172, 175, 178, 183, 191
};

static uint8_t g_volume = 28;
static uint8_t g_pa_open = 0;
static uint8_t g_pcm_open = 0;
static int16_t g_pcm_chunk[256]; /* 256 samples @ 48 kHz = 5.33 ms per chunk */

static void audio_apply_volume(void) {
    if (g_volume > 31) g_volume = 31;
    G_APP_INFO_STATE->volume = g_volume;
    audio_device_op_entry((void *)(uintptr_t)g_dac_vol_tbl[g_volume], (void *)0, (void *)0, AUD_SET_DAC_VOL);
}

static void audio_ensure_pa(void) {
    if (!g_pa_open) {
        audio_device_op_entry((void *)0, (void *)0, (void *)0, AUD_ENABLE_PA);
        audio_device_op_entry((void *)5, (void *)0, (void *)0, AUD_SET_PA_VOL);
        g_pa_open = 1;
    }
}

static int audio_start_pcm_48k(void) {
    audio_ensure_pa();
    if (!g_pcm_open) {
        int32_t cfg[5];
        cfg[0] = 1;     /* DAC playback mode */
        cfg[1] = 0;     /* 16-bit signed PCM */
        cfg[2] = 2;     /* Stereo (identical L+R for TS/TRS cables) */
        cfg[3] = 512;   /* 512-byte DMA buffer quantum */
        cfg[4] = 4;     /* 4 KB pool */
        int32_t rc = (int32_t)(intptr_t)audio_device_op_entry((void *)0, (void *)48, cfg, AUD_PCM_INIT);
        if (rc < 0) return -1;
        g_pcm_open = 1;
    }
    audio_apply_volume();
    return 0;
}

static void audio_drain_and_stop_pcm(int drain_tail) {
    if (g_pcm_open) {
        if (drain_tail) {
            /* Flush partial DMA buffer and wait until final T-state edge finishes */
            audio_device_op_entry(g_pcm_chunk, g_pcm_chunk, (void *)0, AUD_PCM_WRITE);
            for (int i = 0; i < 30; i++) {
                int32_t rem = (int32_t)(intptr_t)audio_device_op_entry((void *)0, (void *)0, (void *)0, AUD_PCM_QUERY);
                if (rem <= 0) break;
                sys_op_entry((void *)1, (void *)0, (void *)0, KERN_OS_TIME_DLY);
            }
        }
        audio_device_op_entry((void *)0, (void *)0, (void *)0, AUD_PCM_CLOSE);
        g_pcm_open = 0;
    }
}

static void audio_shutdown_all(void) {
    audio_drain_and_stop_pcm(0);
    if (g_pa_open) {
        audio_device_op_entry((void *)0, (void *)0, (void *)0, AUD_DISABLE_PA);
        g_pa_open = 0;
    }
}

/* =========================================================================
 * Section 5: MicroSD VFS & .TZX Directory Browser
 * ========================================================================= */

#define MAX_BROWSER_ITEMS 80

typedef struct {
    uint8_t  is_dir;
    uint16_t raw_idx;       /* 0-based index within VFS_EXT_ALL_DIR or VFS_EXT_ALL_FILE */
    char     name[37];
} browser_item_t;

static int32_t        g_vfs_mnt = -1;
static uint8_t        g_drv_installed = 0;
static uint8_t        g_dir_depth = 0;
static uint16_t       g_item_count = 0;
static uint16_t       g_sel_idx = 0;
static uint16_t       g_scroll_top = 0;
static browser_item_t g_items[MAX_BROWSER_ITEMS];
static uint8_t        g_name_raw[96];
static char           g_cur_dir_label[22];

static uint32_t       g_open_fhandle = 0;
static tzx_engine_t   g_tzx;

static void decode_vfs_name(const uint8_t *raw, int is_dir, char *out, int out_cap) {
    if (raw[0] == 0xFFu && raw[1] == 0xFEu) {
        /* UTF-16LE Long Filename */
        int o = 0;
        for (int i = 2; i + 1 < 94 && o + 1 < out_cap; i += 2) {
            uint16_t ch = (uint16_t)(raw[i] | ((uint16_t)raw[i + 1] << 8));
            if (ch == 0) break;
            out[o++] = (ch >= 0x20u && ch <= 0x7Eu) ? (char)ch : '_';
        }
        out[o] = '\0';
        return;
    }
    /* FAT 8.3 short name: bytes 0..7 = base, 8..10 = ext */
    int end_base = 7;
    while (end_base >= 0 && (raw[end_base] == ' ' || raw[end_base] == 0)) end_base--;
    int o = 0;
    for (int i = 0; i <= end_base && o + 1 < out_cap; i++) {
        uint8_t c = raw[i];
        out[o++] = (c >= 0x20u && c <= 0x7Eu) ? (char)c : '_';
    }
    if (!is_dir) {
        int end_ext = 10;
        while (end_ext >= 8 && (raw[end_ext] == ' ' || raw[end_ext] == 0)) end_ext--;
        if (end_ext >= 8 && o + 2 < out_cap) {
            out[o++] = '.';
            for (int i = 8; i <= end_ext && o + 1 < out_cap; i++) {
                uint8_t c = raw[i];
                out[o++] = (c >= 0x20u && c <= 0x7Eu) ? (char)c : '_';
            }
        }
    }
    out[o] = '\0';
}

static int has_tzx_ext(const char *name) {
    int n = str_len(name);
    if (n < 5) return 0;
    return (name[n - 4] == '.' &&
            to_upper(name[n - 3]) == 'T' &&
            to_upper(name[n - 2]) == 'Z' &&
            to_upper(name[n - 1]) == 'X');
}

static void sort_browser_items(void) {
    for (uint16_t i = 1; i < g_item_count; i++) {
        browser_item_t key = g_items[i];
        int j = (int)i - 1;
        while (j >= 0) {
            int swap = 0;
            if (key.is_dir && !g_items[j].is_dir) {
                swap = 1;
            } else if (key.is_dir == g_items[j].is_dir && str_casecmp(key.name, g_items[j].name) < 0) {
                swap = 1;
            }
            if (!swap) break;
            g_items[j + 1] = g_items[j];
            j--;
        }
        g_items[j + 1] = key;
    }
}

static void vfs_scan_current_dir(void) {
    g_item_count = 0;
    g_sel_idx = 0;
    g_scroll_top = 0;
    if (g_vfs_mnt < 0) return;

    /* 1. Enumerate subdirectories */
    uint32_t dir_cmd = VFS_DIR_HEAD;
    uint16_t raw_idx = 0;
    while (g_item_count < MAX_BROWSER_ITEMS) {
        uint32_t r = (uint32_t)(uintptr_t)vfs_op_entry(
            (void *)(intptr_t)g_vfs_mnt, (void *)dir_cmd, (void *)0, (void *)VFS_EXT_ALL_DIR, VFS_CMD_DIR
        );
        if (r == 0) break;
        memset(g_name_raw, 0, sizeof(g_name_raw));
        vfs_op_entry((void *)(intptr_t)g_vfs_mnt, g_name_raw, (void *)44, (void *)0, VFS_CMD_GET_NAME);
        char decoded[37];
        decode_vfs_name(g_name_raw, 1, decoded, sizeof(decoded));
        /* Skip "." / ".." / hidden System Volume Information */
        if (decoded[0] != '\0' && decoded[0] != '.' &&
            str_casecmp(decoded, "SYSTEM VOLUME INFORMATION") != 0) {
            browser_item_t *it = &g_items[g_item_count++];
            it->is_dir = 1;
            it->raw_idx = raw_idx;
            memcpy(it->name, decoded, sizeof(it->name));
        }
        raw_idx++;
        dir_cmd = VFS_DIR_NEXT;
    }

    /* 2. Enumerate *.TZX files */
    dir_cmd = VFS_DIR_HEAD;
    raw_idx = 0;
    while (g_item_count < MAX_BROWSER_ITEMS) {
        uint32_t r = (uint32_t)(uintptr_t)vfs_op_entry(
            (void *)(intptr_t)g_vfs_mnt, (void *)dir_cmd, (void *)0, (void *)VFS_EXT_ALL_FILE, VFS_CMD_DIR
        );
        if (r == 0) break;
        memset(g_name_raw, 0, sizeof(g_name_raw));
        vfs_op_entry((void *)(intptr_t)g_vfs_mnt, g_name_raw, (void *)44, (void *)0, VFS_CMD_GET_NAME);
        char decoded[37];
        decode_vfs_name(g_name_raw, 0, decoded, sizeof(decoded));
        if (has_tzx_ext(decoded)) {
            browser_item_t *it = &g_items[g_item_count++];
            it->is_dir = 0;
            it->raw_idx = raw_idx;
            memcpy(it->name, decoded, sizeof(it->name));
        }
        raw_idx++;
        dir_cmd = VFS_DIR_NEXT;
    }

    sort_browser_items();
}

static int vfs_position_at_raw(uint8_t is_dir, uint16_t target_raw_idx) {
    uint32_t filter = is_dir ? VFS_EXT_ALL_DIR : VFS_EXT_ALL_FILE;
    uint32_t cmd = VFS_DIR_HEAD;
    for (uint16_t i = 0; i <= target_raw_idx; i++) {
        uint32_t r = (uint32_t)(uintptr_t)vfs_op_entry(
            (void *)(intptr_t)g_vfs_mnt, (void *)cmd, (void *)0, (void *)filter, VFS_CMD_DIR
        );
        if (r == 0) return -1;
        cmd = VFS_DIR_NEXT;
    }
    return 0;
}

static void vfs_unmount_card(void) {
    if (g_open_fhandle != 0 && g_vfs_mnt >= 0) {
        vfs_op_entry((void *)(intptr_t)g_vfs_mnt, (void *)(uintptr_t)g_open_fhandle, (void *)0, (void *)0, VFS_CMD_FILE_CLOSE);
        g_open_fhandle = 0;
    }
    if (g_vfs_mnt >= 0) {
        sys_op_entry((void *)(intptr_t)g_vfs_mnt, (void *)0, (void *)0, KERN_UNMOUNT_FS);
        g_vfs_mnt = -1;
    }
    if (g_drv_installed) {
        sys_op_entry((void *)DRV_GROUP_STG_CARD, (void *)0, (void *)0, KERN_DRV_UNINSTALL);
        g_drv_installed = 0;
    }
}

static int vfs_mount_card_and_init(void) {
    vfs_unmount_card();
    g_dir_depth = 0;
    memcpy(g_cur_dir_label, "/", 2);

    int32_t det = (int32_t)(intptr_t)sys_op_entry((void *)DRV_GROUP_STG_CARD, (void *)0, (void *)0, KERN_DETECT_DISK);
    if (det < 0) return -1;

    int32_t already = (int32_t)(intptr_t)sys_op_entry((void *)DRV_GROUP_STG_CARD, (void *)0, (void *)0, KERN_GET_DRV_INFO);
    if (already <= 0) {
        int32_t rc = (int32_t)(intptr_t)sys_op_entry((void *)DRV_GROUP_STG_CARD, (void *)0, (void *)"card.drv", KERN_DRV_INSTALL);
        if (rc != 0) return -1;
        g_drv_installed = 1;
    }

    g_vfs_mnt = (int32_t)(intptr_t)sys_op_entry((void *)DRV_GROUP_STG_CARD, (void *)DISK_H, (void *)0, KERN_MOUNT_FS);
    if (g_vfs_mnt < 0) {
        if (g_drv_installed) {
            sys_op_entry((void *)DRV_GROUP_STG_CARD, (void *)0, (void *)0, KERN_DRV_UNINSTALL);
            g_drv_installed = 0;
        }
        return -1;
    }

    vfs_op_entry((void *)(intptr_t)g_vfs_mnt, (void *)VFS_CD_ROOT, (void *)0, (void *)0, VFS_CMD_CD);
    vfs_scan_current_dir();

    /* Auto-enter /TZX folder on microSD if present! */
    for (uint16_t i = 0; i < g_item_count; i++) {
        if (g_items[i].is_dir && str_casecmp(g_items[i].name, "TZX") == 0) {
            if (vfs_position_at_raw(1, g_items[i].raw_idx) == 0) {
                uint32_t ok = (uint32_t)(uintptr_t)vfs_op_entry(
                    (void *)(intptr_t)g_vfs_mnt, (void *)VFS_CD_SUB, (void *)0, (void *)0, VFS_CMD_CD
                );
                if (ok) {
                    g_dir_depth = 1;
                    memcpy(g_cur_dir_label, "/TZX", 5);
                    vfs_scan_current_dir();
                }
            }
            break;
        }
    }
    return 0;
}

static int32_t tzx_vfs_read_cb(void *ctx, uint32_t offset, void *buf, uint32_t len) {
    (void)ctx;
    if (g_vfs_mnt < 0 || g_open_fhandle == 0) return -1;
    uint32_t ok = (uint32_t)(uintptr_t)vfs_op_entry(
        (void *)(intptr_t)g_vfs_mnt, (void *)(uintptr_t)offset, (void *)VFS_SEEK_SET, (void *)(uintptr_t)g_open_fhandle, VFS_CMD_FILE_SEEK
    );
    if (!ok) return -1;
    uint32_t got = (uint32_t)(uintptr_t)vfs_op_entry(
        (void *)(intptr_t)g_vfs_mnt, buf, (void *)(uintptr_t)len, (void *)(uintptr_t)g_open_fhandle, VFS_CMD_FILE_READ
    );
    return (int32_t)got;
}

/* =========================================================================
 * Section 6: UI Screens (Browser & ZX Playback HUD)
 * ========================================================================= */

static void ui_draw_top_bar(const char *left_txt) {
    fb_fill_rect(0, 0, 128, 9, 1);
    fb_draw_str(2, 1, left_txt, 11, 1);

    /* Right side: 48K / 128K mode + Vxx */
    char rhs[12];
    memcpy(rhs, g_tzx.stop_48k_enabled ? "48K  V" : "128K V", 6);
    fmt_u2(rhs + 6, g_volume);
    rhs[8] = '\0';
    fb_draw_str(128 - 8 * 6 - 1, 1, rhs, 8, 1);
}

static void ui_draw_browser(void) {
    fb_clear();
    char hdr[16];
    memcpy(hdr, "TZX ", 4);
    int i = 0;
    while (g_cur_dir_label[i] && i < 7) {
        hdr[4 + i] = g_cur_dir_label[i];
        i++;
    }
    hdr[4 + i] = '\0';
    ui_draw_top_bar(hdr);

    if (g_vfs_mnt < 0) {
        fb_draw_str(10, 22, "NO MICROSD CARD!", 20, 0);
        fb_draw_str(4,  34, "Insert SD + PLAY", 20, 0);
        fb_draw_str(4,  55, "STOP: Main Menu",  20, 0);
        fb_flush();
        return;
    }

    if (g_item_count == 0) {
        fb_draw_str(13, 24, "NO .TZX FILES", 20, 0);
        fb_draw_str(4,  55, g_dir_depth > 0 ? "STOP: Up Folder" : "STOP: Main Menu", 20, 0);
        fb_flush();
        return;
    }

    /* 5 visible rows (y = 11, 20, 29, 38, 47) + bottom hint bar (y = 56) */
    if (g_sel_idx < g_scroll_top) g_scroll_top = g_sel_idx;
    if (g_sel_idx >= g_scroll_top + 5) g_scroll_top = (uint16_t)(g_sel_idx - 4);

    for (uint16_t r = 0; r < 5; r++) {
        uint16_t idx = (uint16_t)(g_scroll_top + r);
        if (idx >= g_item_count) break;
        int y = 11 + (int)r * 9;
        int sel = (idx == g_sel_idx);
        if (sel) {
            fb_fill_rect(0, y - 1, 128, 9, 1);
        }
        char line[22];
        int p = 0;
        if (g_items[idx].is_dir) {
            line[p++] = '[';
            for (int k = 0; g_items[idx].name[k] && p < 19; k++) {
                line[p++] = g_items[idx].name[k];
            }
            line[p++] = ']';
        } else {
            for (int k = 0; g_items[idx].name[k] && p < 21; k++) {
                line[p++] = g_items[idx].name[k];
            }
        }
        line[p] = '\0';
        fb_draw_str(2, y, line, 21, sel);
    }

    fb_fill_rect(0, 55, 128, 1, 1);
    fb_draw_str(2, 57, "PLAY:Load  M:48/128K", 21, 0);
    fb_flush();
}

static void ui_draw_playback(const char *filename) {
    fb_clear();

    /* Top bar: B:01/12 + 48K+/- + V28 */
    char blk_hdr[12];
    blk_hdr[0] = 'B';
    uint16_t disp_blk = (g_tzx.cur_block < g_tzx.block_count) ? (uint16_t)(g_tzx.cur_block + 1u) : g_tzx.block_count;
    fmt_u3(blk_hdr + 1, disp_blk);
    blk_hdr[4] = '/';
    fmt_u3(blk_hdr + 5, g_tzx.block_count);
    blk_hdr[8] = '\0';
    ui_draw_top_bar(blk_hdr);

    /* Line 1 (y=11): .TZX filename */
    fb_draw_str(2, 11, filename, 21, 0);

    /* Line 2 (y=20): ZX Header / Block Type + Polarity badge */
    char info_line[22];
    const char *sub = (g_tzx.tape_title[0] != '\0') ? g_tzx.tape_title : g_tzx.block_label;
    int p = 0;
    while (sub[p] && p < 15) {
        info_line[p] = sub[p];
        p++;
    }
    while (p < 16) info_line[p++] = ' ';
    memcpy(info_line + 16, g_tzx.invert_polarity ? "INV " : "NORM", 4);
    info_line[20] = '\0';
    fb_draw_str(2, 20, info_line, 20, 0);

    /* Line 3 (y=29): Current block type + countdown -MM:SS */
    char time_line[22];
    const char *btype = (g_tzx.cur_block < g_tzx.block_count)
                        ? tzx_block_type_short(g_tzx.blocks[g_tzx.cur_block].id)
                        : "END";
    p = 0;
    while (btype[p] && p < 13) {
        time_line[p] = btype[p];
        p++;
    }
    while (p < 14) time_line[p++] = ' ';
    uint32_t rem_secs = tzx_remaining_seconds(&g_tzx);
    time_line[14] = '-';
    fmt_u2(time_line + 15, (rem_secs / 60u) % 100u);
    time_line[17] = ':';
    fmt_u2(time_line + 18, rem_secs % 60u);
    time_line[20] = '\0';
    fb_draw_str(2, 29, time_line, 20, 0);

    /* Smooth progress bar (y=39..45) across total tape duration */
    fb_draw_rect(2, 39, 124, 6, 1);
    if (g_tzx.total_dur_ds > 0) {
        uint32_t rem_ds = tzx_remaining_ds(&g_tzx);
        uint32_t done_ds = (rem_ds < g_tzx.total_dur_ds) ? (g_tzx.total_dur_ds - rem_ds) : g_tzx.total_dur_ds;
        uint32_t fill_w = (done_ds * 120u) / g_tzx.total_dur_ds;
        if (fill_w > 120u) fill_w = 120u;
        if (fill_w > 0) fb_fill_rect(4, 41, (int)fill_w, 2, 1);
    } else if (g_tzx.block_count > 0) {
        uint32_t done = (g_tzx.status == TZX_STATUS_EOF) ? g_tzx.block_count : g_tzx.cur_block;
        uint32_t fill_w = (done * 120u) / g_tzx.block_count;
        if (fill_w > 120u) fill_w = 120u;
        if (fill_w > 0) fb_fill_rect(4, 41, (int)fill_w, 2, 1);
    }

    /* Bottom status banner (y=48..63) */
    if (g_tzx.status == TZX_STATUS_AUTO_STOP) {
        fb_fill_rect(0, 48, 128, 16, 1);
        fb_draw_str(4, 49, g_tzx.auto_stop_is_48k ? "48K STOP - PRESS PLAY" : "STOPPED - PRESS PLAY", 21, 1);
        fb_draw_str(1, 57, ">>Skip M:48/128K STOP", 21, 1);
    } else if (g_tzx.status == TZX_STATUS_PAUSED) {
        fb_fill_rect(0, 48, 128, 9, 1);
        fb_draw_str(16, 49, "[PAUSED - PLAY]", 19, 1);
        fb_draw_str(2, 57, "<</>>Blk   M:48/128K", 21, 0);
    } else if (g_tzx.status == TZX_STATUS_EOF) {
        fb_fill_rect(0, 48, 128, 9, 1);
        fb_draw_str(13, 49, "[TAPE COMPLETE]", 19, 1);
        fb_draw_str(4, 57, "PLAY:Replay STOP:List", 21, 0);
    } else {
        fb_draw_str(2, 49, "PLAYING 48kHz -> EAR", 20, 0);
        fb_draw_str(2, 57, "PLAY:Pause M:48/128K", 21, 0);
    }

    fb_flush();
}

/* =========================================================================
 * Section 7: Key Input, OS IPC, and Main Event Loop
 * ========================================================================= */

static uint8_t g_seen_key_down = 0;

static int poll_key_event(uint8_t *out_val, uint16_t *out_type) {
    uint32_t raw_key = 0;
    int32_t rc = (int32_t)(intptr_t)sys_op_entry((void *)MQ_ID_GUI, &raw_key, (void *)0, KERN_MQ_RECEIVE);
    if (rc != 0) return 0;
    uint8_t msg_type = (uint8_t)(raw_key & 0xFFu);
    if (msg_type != 0) return 0;
    uint8_t kval = (uint8_t)((raw_key >> 8) & 0xFFu);
    uint16_t ktype = (uint16_t)((raw_key >> 16) & 0xFFFFu);
    if (ktype == KEY_TYPE_DOWN) {
        g_seen_key_down = 1;
    } else if (!g_seen_key_down) {
        /* Ignore residual key-up from MAINMENU launch */
        return 0;
    }
    *out_val = kval;
    *out_type = ktype;
    return 1;
}

static void send_async_to_manager(uint32_t msg_type, uint8_t d0, uint8_t d1) {
    private_msg_t pmsg;
    memset(&pmsg, 0, sizeof(pmsg));
    pmsg.msg.type = msg_type;
    pmsg.msg.data[0] = d0;
    pmsg.msg.data[1] = d1;
    sys_op_entry((void *)MQ_ID_MNG, &pmsg, (void *)0, KERN_MQ_SEND);
}

/* Returns 1 if OS requested immediate app termination (power switch / USB) */
static int poll_system_messages(int *out_sd_changed) {
    uint32_t sys_ev = 0;
    if ((int32_t)(intptr_t)sys_op_entry((void *)MQ_ID_SYS, &sys_ev, (void *)0, KERN_MQ_RECEIVE) == 0) {
        uint16_t stype = (uint16_t)(sys_ev & 0xFFFFu);
        if (stype == MSG_POWER_OFF || stype == MSG_USB_STICK) {
            /* Forward system power-off / USB-plug event to MANAGER.AP */
            send_async_to_manager(stype, 0, 0);
        } else if (stype == MSG_SD_IN || stype == MSG_SD_OUT) {
            if (out_sd_changed) *out_sd_changed = 1;
        }
    }

    private_msg_t desk_msg;
    if ((int32_t)(intptr_t)sys_op_entry((void *)MQ_ID_DESK, &desk_msg, (void *)0, KERN_MQ_RECEIVE) == 0) {
        if (desk_msg.msg.type == MSG_APP_QUIT) {
            return 1;
        }
    }
    return 0;
}

/* Stop background MENGINE / FMENGINE if running so AUD_DEV DAC is exclusively ours */
static void stop_background_audio_engine_if_any(void) {
    if (G_APP_INFO_VECTOR[2].used != 0) {
        uint8_t bg_id = G_APP_INFO_VECTOR[2].app_id;
        private_msg_t kill_msg;
        memset(&kill_msg, 0, sizeof(kill_msg));
        kill_msg.msg.type = MSG_APP_QUIT;
        sys_op_entry((void *)MQ_ID_EGN, &kill_msg, (void *)0, KERN_MQ_SEND);
        for (int i = 0; i < 30; i++) {
            if (G_APP_INFO_VECTOR[2].used == 0) break;
            sys_op_entry((void *)2, (void *)0, (void *)0, KERN_OS_TIME_DLY);
        }
        (void)bg_id;
    }
}

static int run_playback_scene(const browser_item_t *file_item) {
    if (vfs_position_at_raw(0, file_item->raw_idx) < 0) return 0;
    g_open_fhandle = (uint32_t)(uintptr_t)vfs_op_entry(
        (void *)(intptr_t)g_vfs_mnt, (void *)0, (void *)VFS_R_NORMAL_SEEK, (void *)0, VFS_CMD_FILE_OPEN
    );
    if (g_open_fhandle == 0) return 0;

    uint32_t fsize = 0;
    vfs_op_entry(
        (void *)(intptr_t)g_vfs_mnt, &fsize, (void *)(uintptr_t)g_open_fhandle, (void *)0, VFS_CMD_FILE_GET_SIZE
    );
    if (fsize < 10) {
        vfs_op_entry((void *)(intptr_t)g_vfs_mnt, (void *)(uintptr_t)g_open_fhandle, (void *)0, (void *)0, VFS_CMD_FILE_CLOSE);
        g_open_fhandle = 0;
        return 0;
    }

    tzx_err_t terr = tzx_open(&g_tzx, tzx_vfs_read_cb, (void *)0, fsize);
    if (terr != TZX_OK) {
        vfs_op_entry((void *)(intptr_t)g_vfs_mnt, (void *)(uintptr_t)g_open_fhandle, (void *)0, (void *)0, VFS_CMD_FILE_CLOSE);
        g_open_fhandle = 0;
        return 0;
    }

    G_APP_INFO_STATE->app_state = 2; /* APP_STATE_PLAYING_ALWAYS */
    audio_start_pcm_48k();

    int need_redraw = 1;
    uint16_t last_drawn_blk = 0xFFFFu;
    uint32_t last_drawn_sec = 0xFFFFFFFFu;
    int os_quit = 0;

    while (1) {
        int sd_changed = 0;
        if (poll_system_messages(&sd_changed)) {
            os_quit = 1;
            break;
        }
        if (sd_changed) {
            break;
        }

        uint8_t kval = 0;
        uint16_t ktype = 0;
        while (poll_key_event(&kval, &ktype)) {
            if (kval == KEY_PLAY && ktype == KEY_TYPE_SHORT_UP) {
                if (g_tzx.status == TZX_STATUS_PLAYING) {
                    g_tzx.status = TZX_STATUS_PAUSED;
                    audio_drain_and_stop_pcm(0);
                } else {
                    tzx_resume(&g_tzx);
                    audio_start_pcm_48k();
                }
                need_redraw = 1;
            } else if ((kval == KEY_RETURN || kval == KEY_POWER) && ktype == KEY_TYPE_SHORT_UP) {
                if (kval == KEY_POWER) {
                    send_async_to_manager(MSG_POWER_OFF, 0, 0);
                }
                goto exit_playback;
            } else if (kval == KEY_MODE && ktype == KEY_TYPE_SHORT_UP) {
                g_tzx.stop_48k_enabled ^= 1u;
                need_redraw = 1;
            } else if (kval == KEY_REC && ktype == KEY_TYPE_SHORT_UP) {
                g_tzx.invert_polarity ^= 1u;
                need_redraw = 1;
            } else if (kval == KEY_NEXT && (ktype == KEY_TYPE_SHORT_UP || ktype == KEY_TYPE_HOLD)) {
                if (g_tzx.cur_block + 1 < g_tzx.block_count) {
                    audio_drain_and_stop_pcm(0);
                    tzx_seek_block(&g_tzx, (uint16_t)(g_tzx.cur_block + 1u));
                    if (g_tzx.status == TZX_STATUS_PLAYING) audio_start_pcm_48k();
                    need_redraw = 1;
                }
            } else if (kval == KEY_PREV && (ktype == KEY_TYPE_SHORT_UP || ktype == KEY_TYPE_HOLD)) {
                audio_drain_and_stop_pcm(0);
                uint16_t prev = (g_tzx.cur_block > 0) ? (uint16_t)(g_tzx.cur_block - 1u) : 0;
                tzx_seek_block(&g_tzx, prev);
                if (g_tzx.status == TZX_STATUS_PLAYING) audio_start_pcm_48k();
                need_redraw = 1;
            } else if (kval == KEY_VADD && (ktype == KEY_TYPE_DOWN || ktype == KEY_TYPE_HOLD)) {
                if (g_volume < 31) g_volume++;
                audio_apply_volume();
                need_redraw = 1;
            } else if (kval == KEY_VSUB && (ktype == KEY_TYPE_DOWN || ktype == KEY_TYPE_HOLD)) {
                if (g_volume > 0) g_volume--;
                audio_apply_volume();
                need_redraw = 1;
            }
        }

        if (g_tzx.status == TZX_STATUS_PLAYING) {
            if (!g_pcm_open) {
                audio_start_pcm_48k();
            }
            /* Stream 4 chunks (~21.3 ms) per UI/key poll iteration for zero underrun */
            for (int c = 0; c < 4 && g_tzx.status == TZX_STATUS_PLAYING; c++) {
                uint32_t got = 0;
                tzx_render_res_t r = tzx_render(&g_tzx, g_pcm_chunk, 256, &got);
                if (got > 0) {
                    audio_device_op_entry(
                        g_pcm_chunk, g_pcm_chunk, (void *)(uintptr_t)(got * 2u), AUD_PCM_WRITE
                    );
                }
                if (r == TZX_RENDER_AUTO_STOP || r == TZX_RENDER_EOF || r == TZX_RENDER_ERR) {
                    audio_drain_and_stop_pcm(1);
                    need_redraw = 1;
                    break;
                }
            }
            uint32_t cur_sec = tzx_remaining_seconds(&g_tzx);
            if (g_tzx.cur_block != last_drawn_blk || cur_sec != last_drawn_sec) {
                need_redraw = 1;
            }
        } else {
            sys_op_entry((void *)2, (void *)0, (void *)0, KERN_OS_TIME_DLY);
        }

        if (need_redraw) {
            last_drawn_blk = g_tzx.cur_block;
            last_drawn_sec = tzx_remaining_seconds(&g_tzx);
            ui_draw_playback(file_item->name);
            need_redraw = 0;
        }
    }

exit_playback:
    audio_shutdown_all();
    if (g_open_fhandle != 0) {
        vfs_op_entry((void *)(intptr_t)g_vfs_mnt, (void *)(uintptr_t)g_open_fhandle, (void *)0, (void *)0, VFS_CMD_FILE_CLOSE);
        g_open_fhandle = 0;
    }
    G_APP_INFO_STATE->app_state = 0;
    return os_quit;
}

int tzx_app_main(int argc) {
    (void)argc;

    /* Register foreground GUI app (slot 1 = APP_TYPE_GUI, MQ_ID_DESK = 1) */
    sys_op_entry((void *)0, (void *)0, (void *)0, KERN_OS_SCHED_LOCK);
    volatile app_info_t *my_app = &G_APP_INFO_VECTOR[1];
    my_app->used = 1;
    my_app->app_id = APP_ID_TOOLS;
    my_app->app_type = 1;
    my_app->mq_id = MQ_ID_DESK;
    my_app->screensaver_forbidden = 1;
    my_app->auto_standby_forbidden = 1;
    my_app->timed_shutdown_forbidden = 1;
    my_app->soft_watchdog_forbidden = 1;
    my_app->soft_watchdog_timeout_expires = 0;
    sys_op_entry((void *)0, (void *)0, (void *)0, KERN_OS_SCHED_UNLOCK);

    stop_background_audio_engine_if_any();

    /* Save & mute keytone flag while inside TZX Mode */
    uint8_t saved_sound_state = G_APP_INFO_STATE->sound_state;
    G_APP_INFO_STATE->sound_state &= 0x7Fu;
    if (G_APP_INFO_STATE->volume <= 31 && G_APP_INFO_STATE->volume > 0) {
        g_volume = G_APP_INFO_STATE->volume;
    } else {
        g_volume = 28;
    }

    /* Default 48K STOP = ON per user spec */
    g_tzx.stop_48k_enabled = 1;
    g_tzx.invert_polarity = 0;

    /* Flush residual GUI messages from MAINMENU button press */
    sys_op_entry((void *)MQ_ID_GUI, (void *)0, (void *)0, KERN_MQ_FLUSH);
    g_seen_key_down = 0;

    /* Immediate splash before mounting SD so LCD updates right away */
    fb_clear();
    fb_fill_rect(0, 0, 128, 10, 1);
    fb_draw_str(2, 2, "TZX MODE", 20, 1);
    fb_draw_str(10, 28, "Mounting microSD...", 20, 0);
    fb_flush();

    vfs_mount_card_and_init();

    int need_redraw = 1;
    int os_quit = 0;

    while (1) {
        int sd_changed = 0;
        if (poll_system_messages(&sd_changed)) {
            os_quit = 1;
            break;
        }
        if (sd_changed) {
            vfs_mount_card_and_init();
            need_redraw = 1;
        }

        if (need_redraw) {
            ui_draw_browser();
            need_redraw = 0;
        }

        uint8_t kval = 0;
        uint16_t ktype = 0;
        if (poll_key_event(&kval, &ktype)) {
            if (kval == KEY_NEXT && (ktype == KEY_TYPE_DOWN || ktype == KEY_TYPE_HOLD)) {
                if (g_item_count > 0) {
                    g_sel_idx = (uint16_t)((g_sel_idx + 1u) % g_item_count);
                    need_redraw = 1;
                }
            } else if (kval == KEY_PREV && (ktype == KEY_TYPE_DOWN || ktype == KEY_TYPE_HOLD)) {
                if (g_item_count > 0) {
                    g_sel_idx = (g_sel_idx > 0) ? (uint16_t)(g_sel_idx - 1u) : (uint16_t)(g_item_count - 1u);
                    need_redraw = 1;
                }
            } else if (kval == KEY_VADD && (ktype == KEY_TYPE_DOWN || ktype == KEY_TYPE_HOLD)) {
                if (g_volume < 31) g_volume++;
                G_APP_INFO_STATE->volume = g_volume;
                need_redraw = 1;
            } else if (kval == KEY_VSUB && (ktype == KEY_TYPE_DOWN || ktype == KEY_TYPE_HOLD)) {
                if (g_volume > 0) g_volume--;
                G_APP_INFO_STATE->volume = g_volume;
                need_redraw = 1;
            } else if (kval == KEY_MODE && ktype == KEY_TYPE_SHORT_UP) {
                g_tzx.stop_48k_enabled ^= 1u;
                need_redraw = 1;
            } else if (kval == KEY_PLAY && ktype == KEY_TYPE_SHORT_UP) {
                if (g_vfs_mnt < 0) {
                    vfs_mount_card_and_init();
                    need_redraw = 1;
                } else if (g_item_count > 0 && g_sel_idx < g_item_count) {
                    browser_item_t chosen = g_items[g_sel_idx];
                    if (chosen.is_dir) {
                        if (vfs_position_at_raw(1, chosen.raw_idx) == 0) {
                            uint32_t ok = (uint32_t)(uintptr_t)vfs_op_entry(
                                (void *)(intptr_t)g_vfs_mnt, (void *)VFS_CD_SUB, (void *)0, (void *)0, VFS_CMD_CD
                            );
                            if (ok) {
                                g_dir_depth++;
                                g_cur_dir_label[0] = '/';
                                int k = 0;
                                while (chosen.name[k] && k < 18) {
                                    g_cur_dir_label[1 + k] = chosen.name[k];
                                    k++;
                                }
                                g_cur_dir_label[1 + k] = '\0';
                                vfs_scan_current_dir();
                                need_redraw = 1;
                            }
                        }
                    } else {
                        if (run_playback_scene(&chosen)) {
                            os_quit = 1;
                            break;
                        }
                        need_redraw = 1;
                    }
                }
            } else if (kval == KEY_RETURN && ktype == KEY_TYPE_SHORT_UP) {
                if (g_vfs_mnt >= 0 && g_dir_depth > 0) {
                    uint32_t ok = (uint32_t)(uintptr_t)vfs_op_entry(
                        (void *)(intptr_t)g_vfs_mnt, (void *)VFS_CD_UP, (void *)0, (void *)0, VFS_CMD_CD
                    );
                    if (ok) {
                        g_dir_depth--;
                        if (g_dir_depth == 0) memcpy(g_cur_dir_label, "/", 2);
                        else                  memcpy(g_cur_dir_label, "..", 3);
                        vfs_scan_current_dir();
                        need_redraw = 1;
                    } else {
                        g_dir_depth = 0;
                        break;
                    }
                } else {
                    break;
                }
            } else if (kval == KEY_POWER && ktype == KEY_TYPE_SHORT_UP) {
                send_async_to_manager(MSG_POWER_OFF, 0, 0);
                os_quit = 1;
                break;
            }
        } else {
            sys_op_entry((void *)2, (void *)0, (void *)0, KERN_OS_TIME_DLY);
        }
    }

    audio_shutdown_all();
    vfs_unmount_card();

    /* Restore system sound_state & unregister foreground app */
    G_APP_INFO_STATE->sound_state = saved_sound_state;
    G_APP_INFO_STATE->app_state = 0;

    sys_op_entry((void *)0, (void *)0, (void *)0, KERN_OS_SCHED_LOCK);
    my_app->used = 0;
    my_app->app_id = 0x19u;
    sys_op_entry((void *)0, (void *)0, (void *)0, KERN_OS_SCHED_UNLOCK);

    if (!os_quit) {
        send_async_to_manager(MSG_CREAT_APP, APP_ID_MAINMENU, PARAM_FROM_TOOLS);
    }

    return 0;
}
