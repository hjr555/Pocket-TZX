/*
 * SPI NOR Erase/Program/Read payload for Actions ATJ2157 (ARMv7-M), run from ADFU SRAM.
 *
 * Loaded to SRAM @ 0x0011e000 (after adfus.bin @ 0x00118000) and invoked via CMD_ADFU_EXEC.
 * Args/return block @ 0x0011ffe0: p[4]=cmd, p[5]=addr, p[6]=len/buf_off, p[7]=flags.
 * Data buffer @ 0x0011a000 (16 KB).
 *
 * Low-level SPI sequences mirror the shipped code verified with REA/Ghidra 12.1.4:
 *   - Mask ROM : FUN_000032c0 (1-byte cmd), FUN_000038f4 (RDSR), FUN_00002224 (0x0B read + DMA0,
 *                SPI0_CTL bit12 = hardware descrambler on read)
 *   - BREC     : FUN_00114118 (512 B sector program = SPI0_CTL |= 0x2000|0x8000, two 0x02 page
 *                programs of 256 B, then clear), FUN_00113f2e (bit15 released only during the data
 *                phase when scrambling), FUN_00113d96 (TX DMA, mode 0x208800, SPI0_CTL bit31|bit7),
 *                FUN_0011401a (poll RDSR WIP), FUN_00114040(6) (WREN).
 *
 * HARD ADDRESS GUARD (independent of the host script):
 *   - 4 KB erase only for 0x0000b000 <= addr <= 0x003df000 (4 KB aligned).
 *       0x000000..0x00afff (MBREC + BREC) and 0x3e0000.. (VRAM) can never be erased.
 *   - 512 B program only for 0x0000b000 <= addr and addr + 512 <= 0x003e0000.
 *   - Scrambled program additionally requires addr >= 0x0000b400 (BREC tail is raw-restored only).
 */
#include <stdint.h>

#define MEM4(addr) (*(volatile uint32_t *)(addr))

#define RTC_WD_CTL   0xc003001cu
#define SPI0_BASE    0xc00a0000u
#define SPI0_CTL     MEM4(SPI0_BASE + 0x00u)
#define SPI0_STAT_A  (SPI0_BASE + 0x04u)
#define SPI0_STAT    MEM4(SPI0_STAT_A)
#define SPI0_TXDAT_A (SPI0_BASE + 0x08u)
#define SPI0_TXDAT   MEM4(SPI0_TXDAT_A)
#define SPI0_RXDAT   MEM4(SPI0_BASE + 0x0cu)
#define SPI0_BC      MEM4(SPI0_BASE + 0x10u)

#define DMA0_MODE    MEM4(0xc0070100u)
#define DMA0_START_A 0xc0070104u
#define DMA0_START   MEM4(DMA0_START_A)
#define DMA0_SRC     MEM4(0xc0070108u)
#define DMA0_DST     MEM4(0xc0070110u)
#define DMA0_LEN     MEM4(0xc0070118u)

#define DATA_BUF     0x0011a000u
#define DATA_BUF_SZ  0x4000u
#define ARGS_BASE    0x0011ffe0u

#define ERASE_MIN    0x0000b000u
#define ERASE_MAX    0x003df000u
#define PROG_MIN     0x0000b000u
#define PROG_END     0x003e0000u
#define SCRAMBLE_MIN 0x0000b400u

#define FLAG_SCRAMBLE 0x1u
#define FLAG_PIO      0x2u

#define DEF_ROM_FN(addr, ret, name, args) \
    static ret (* const name) args = (ret (*) args)((addr) | 1u)

DEF_ROM_FN(0x00001ab4u, void, rom_spinor_clk_init, (void));
DEF_ROM_FN(0x00001aecu, void, rom_spinor_clk_deinit, (void));
DEF_ROM_FN(0x0000383cu, void, rom_spinor_pinmux_set, (int group));
DEF_ROM_FN(0x00003884u, void, rom_spinor_pinmux_restore, (int group));
DEF_ROM_FN(0x00002224u, int,  rom_spinor_read_sector, (uint32_t addr, uint32_t len, uint32_t dst, uint32_t mode));
DEF_ROM_FN(0x00004ca0u, int,  rom_wait_bits, (uint32_t reg, uint32_t mask, uint32_t val, uint32_t loops));

void *entry_main(void);

__attribute__((naked, section(".text._start"))) void _start(void) {
    __asm__ volatile("b entry_main\n");
}

static inline void wd_pet(void) { MEM4(RTC_WD_CTL) |= 1u; }

static void delay_loops(unsigned n) {
    do { __asm__ __volatile__("" ::: "memory"); } while (--n);
}

/* ROM FUN_000032c0 prologue: CS low (clear bit3), mode = 2 (TX) */
static void cs_low_tx(void) { SPI0_CTL = (SPI0_CTL & ~0xBu) | 2u; }
/* ROM FUN_000032c0 epilogue: mode = 0, CS high (bit3) */
static void cs_high(void)   { SPI0_CTL = (SPI0_CTL & ~0x3u) | 0x8u; }

/* ROM FUN_00003c8c: wait TX FIFO empty (bit4) then not busy (bit6) */
static void tx_byte(uint8_t b) {
    SPI0_TXDAT = b;
    rom_wait_bits(SPI0_STAT_A, 0x10u, 0x10u, 5u);
    rom_wait_bits(SPI0_STAT_A, 0x40u, 0x00u, 5u);
}

static void spi_cmd1(uint8_t cmd) {
    cs_low_tx();
    tx_byte(cmd);
    cs_high();
}

/* ROM FUN_000038f4 (RDSR) */
static uint8_t read_sr(void) {
    cs_low_tx();
    tx_byte(0x05u);
    SPI0_BC = 1u;
    SPI0_CTL = ((SPI0_CTL & ~0x33u) | 0x30u) + 1u;
    rom_wait_bits(SPI0_STAT_A, 0x4u, 0x0u, 5u);
    cs_high();
    return (uint8_t)(SPI0_RXDAT & 0xffu);
}

/* BREC FUN_0011401a: poll WIP; returns final SR (>=0) or -1 on timeout */
static int wait_wip(void) {
    for (uint32_t i = 0; i < 400000u; i++) {
        uint8_t sr = read_sr();
        if ((sr & 1u) == 0u) return sr;
        if ((i & 0xffu) == 0u) wd_pet();
        delay_loops(200u);
    }
    return -1;
}

static int write_enable(void) {
    spi_cmd1(0x06u);
    return (read_sr() & 0x2u) ? 0 : -2;
}

/* BREC FUN_00113d96: TX DMA of n bytes from SRAM into SPI0_TXDAT (CS already low) */
static int tx_dma(uint32_t src, uint32_t n) {
    int rc;
    SPI0_CTL &= ~0x80000000u;
    SPI0_CTL |= 0x80000000u;
    SPI0_CTL &= ~0x80u;
    SPI0_CTL |= 0x80u;
    SPI0_CTL = (SPI0_CTL & ~0x3u) | 0x2u;
    DMA0_MODE = 0x00208800u;
    DMA0_SRC = src;
    DMA0_DST = SPI0_TXDAT_A;
    DMA0_LEN = n;
    DMA0_START = 1u;
    rc = rom_wait_bits(DMA0_START_A, 1u, 0u, 0x5dcu);
    delay_loops(16u);
    rom_wait_bits(SPI0_STAT_A, 0x10u, 0x10u, 5u);
    rom_wait_bits(SPI0_STAT_A, 0x40u, 0x00u, 5u);
    SPI0_CTL &= ~0x80u;
    SPI0_CTL &= ~0x80000000u;
    SPI0_CTL &= ~0x3u;
    return rc;
}

/* BREC FUN_00113c2a: PIO TX (wait TX-full bit5 clear per byte) */
static void tx_pio(const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        while (SPI0_STAT & 0x20u) {}
        SPI0_TXDAT = src[i];
    }
    rom_wait_bits(SPI0_STAT_A, 0x10u, 0x10u, 5u);
    rom_wait_bits(SPI0_STAT_A, 0x40u, 0x00u, 5u);
}

static int erase_4k(uint32_t addr) {
    if ((addr & 0xfffu) != 0u || addr < ERASE_MIN || addr > ERASE_MAX) return -100;
    if (write_enable() != 0) return -2;
    cs_low_tx();
    tx_byte(0x20u);
    tx_byte((uint8_t)(addr >> 16));
    tx_byte((uint8_t)(addr >> 8));
    tx_byte((uint8_t)addr);
    cs_high();
    return wait_wip();
}

static int program_page(uint32_t addr, uint32_t src, uint32_t flags) {
    int rc;
    if (write_enable() != 0) return -2;
    cs_low_tx();
    tx_byte(0x02u);
    tx_byte((uint8_t)(addr >> 16));
    tx_byte((uint8_t)(addr >> 8));
    tx_byte((uint8_t)addr);
    if (flags & FLAG_SCRAMBLE) SPI0_CTL &= ~0x8000u;   /* release bypass for data phase only */
    if (flags & FLAG_PIO) {
        tx_pio((const uint8_t *)src, 256u);
        rc = 0;
    } else {
        rc = tx_dma(src, 256u);
    }
    if (flags & FLAG_SCRAMBLE) SPI0_CTL |= 0x8000u;
    cs_high();
    if (rc != 0) { wait_wip(); return -3; }
    return wait_wip();
}

static int program_sector(uint32_t addr, uint32_t src, uint32_t flags) {
    int rc;
    if ((addr & 0x1ffu) != 0u || addr < PROG_MIN || addr + 0x200u > PROG_END) return -100;
    if ((flags & FLAG_SCRAMBLE) && addr < SCRAMBLE_MIN) return -101;
    if (src < DATA_BUF || src + 0x200u > DATA_BUF + DATA_BUF_SZ) return -102;

    SPI0_CTL &= ~0x1000u;                 /* never descramble while writing */
    if (flags & FLAG_SCRAMBLE) {
        SPI0_CTL |= 0x2000u;              /* BREC FUN_00114118: scrambler on, held */
        SPI0_CTL |= 0x8000u;
    } else {
        SPI0_CTL &= ~0x2000u;             /* raw write: scrambler off */
    }
    rc = program_page(addr, src, flags);
    if (rc >= 0) rc = program_page(addr + 0x100u, src + 0x100u, flags);
    if (flags & FLAG_SCRAMBLE) {
        SPI0_CTL &= ~0x8000u;
        SPI0_CTL &= ~0x2000u;
    }
    return rc;
}

__attribute__((used)) void *entry_main(void) {
    volatile uint32_t *p = (volatile uint32_t *)ARGS_BASE;
    uint32_t cmd = p[4];
    uint32_t a = p[5];
    uint32_t b = p[6];
    uint32_t f = p[7];

    wd_pet();
    p[0] = ARGS_BASE + 8u;
    p[1] = 8u;
    p[2] = 0u;
    p[3] = 0u;

    if (cmd == 1u) {
        /* INIT: identical to the proven read-only dumper (clk + pinmux + 0xAB wake + 0xFF reset) */
        rom_spinor_clk_init();
        rom_spinor_pinmux_set(0);
        spi_cmd1(0xabu);
        delay_loops(2000u);
        spi_cmd1(0xffu);
        delay_loops(1000u);
        SPI0_CTL &= ~(0x1000u | 0x2000u | 0x8000u | 0x200u);
        p[2] = 0x48534c46u; /* "FLSH" */
        p[3] = read_sr();
    } else if (cmd == 2u) {
        /* READ len (multiple of 512, <= 16 KB) into DATA_BUF; f = descramble flag */
        uint32_t done = 0u;
        int rc = 0;
        if (b > DATA_BUF_SZ || (b & 0x1ffu) || (a & 0x1ffu)) {
            p[2] = (uint32_t)-100;
        } else {
            SPI0_CTL &= ~(0x2000u | 0x8000u);
            MEM4(0x0010009cu) = f ? 1u : 0u;
            MEM4(0x001000a0u) = 0u;
            MEM4(0x001000a8u) = 0u;
            while (done < b) {
                wd_pet();
                rc |= rom_spinor_read_sector(a + done, 0x200u, DATA_BUF + done, 0u);
                done += 0x200u;
            }
            MEM4(0x0010009cu) = 0u;
            p[2] = (uint32_t)rc;
            p[3] = done;
        }
    } else if (cmd == 3u) {
        SPI0_CTL &= ~(0x1000u | 0x2000u | 0x8000u);
        rom_spinor_pinmux_restore(0);
        rom_spinor_clk_deinit();
        p[2] = 0x444f4e45u;
    } else if (cmd == 4u) {
        /* FWSC=1 response so adfus.bin 0x23 + 0xB0 triggers clean watchdog reboot */
        volatile uint8_t *buf = (volatile uint8_t *)DATA_BUF;
        for (unsigned i = 0; i < 16u; i++) buf[i] = 0u;
        buf[8] = 'F'; buf[9] = 'W'; buf[10] = 'S'; buf[11] = 'C'; buf[12] = 1u;
        p[0] = DATA_BUF;
        p[1] = 16u;
    } else if (cmd == 5u) {
        p[2] = (uint32_t)erase_4k(a);
        p[3] = read_sr();
    } else if (cmd == 6u) {
        p[2] = (uint32_t)program_sector(a, DATA_BUF + b, f);
        p[3] = read_sr();
    } else if (cmd == 7u) {
        p[2] = read_sr();
    } else {
        p[2] = (uint32_t)-1;
    }
    return (void *)p;
}
