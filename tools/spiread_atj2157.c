/*
 * Read-Only SPI NOR Dumper Payload for Actions ATJ2157 (ARMv7-M / Cortex-M)
 *
 * Loaded to SRAM @ 0x0011e000 and invoked via adfus.bin CMD_ADFU_EXEC (0x21).
 * Uses hardware registers & Mask Boot ROM helpers proven via Ghidra 12.1.4:
 *   - rom_spinor_init_all   : 0x00001ab4 (saves CMU/MFP, clocks SPI0 @ 0xc00a0000 + DMA0 @ 0xc0070000)
 *   - rom_spinor_pinmux     : 0x0000383c (pinmux 0: 0xc01c0034..0x40 = 0x3027)
 *   - rom_spinor_wakeup_rst : 0x00003280 (0xAB Release Power-Down, 0xE9 Exit 4B, 0xFF Reset)
 *   - rom_spinor_read_sec   : 0x00002224 (0x0B Fast Read + DMA0, honoring encrypt flag @ 0x10009c)
 *   - rom_spinor_pinmux_rst : 0x00003884
 *   - rom_spinor_deinit_all : 0x00001aec
 *   - rom_wait_bits         : 0x00004ca0
 *
 * Strictly READ-ONLY: issues only 0xAB, 0x9F, 0x05, 0x35, 0x15, 0x4B, 0x5A, 0x0B.
 * Never issues Write Enable (0x06) during data reads, Page Program (0x02), or Erase (0x20/0xD8/0xC7).
 */
#include <stdint.h>

#define MEM4(addr) (*(volatile uint32_t *)(addr))

#define RTC_WD_CTL   0xc003001cu
#define SPI0_BASE    0xc00a0000u
#define SPI0_CTL     MEM4(SPI0_BASE + 0x00u)
#define SPI0_STAT    MEM4(SPI0_BASE + 0x04u)
#define SPI0_TXDAT   MEM4(SPI0_BASE + 0x08u)
#define SPI0_RXDAT   (SPI0_BASE + 0x0cu)
#define SPI0_BC      MEM4(SPI0_BASE + 0x10u)

#define DMA0_MODE    MEM4(0xc0070100u)
#define DMA0_START   MEM4(0xc0070104u)
#define DMA0_SRC     MEM4(0xc0070108u)
#define DMA0_DST     MEM4(0xc0070110u)
#define DMA0_LEN     MEM4(0xc0070118u)

#define DATA_BUF     0x0011a000u
#define ARGS_BASE    0x0011ffe0u

#define DEF_ROM_FN(addr, ret, name, args) \
    static ret (* const name) args = (ret (*) args)((addr) | 1u)

DEF_ROM_FN(0x00001ab4u, void, rom_spinor_clk_init, (void));
DEF_ROM_FN(0x00001aecu, void, rom_spinor_clk_deinit, (void));
DEF_ROM_FN(0x0000383cu, void, rom_spinor_pinmux_set, (int group));
DEF_ROM_FN(0x00003884u, void, rom_spinor_pinmux_restore, (int group));
DEF_ROM_FN(0x00002224u, int,  rom_spinor_read_sector, (uint32_t addr, uint32_t len, uint32_t dst, uint32_t mode));
DEF_ROM_FN(0x000032c0u, int,  rom_spinor_send_cmd1, (uint32_t cmd));
DEF_ROM_FN(0x00004ca0u, int,  rom_wait_bits, (uint32_t reg, uint32_t mask, uint32_t val, uint32_t loops));

static inline void wd_pet(void) {
    MEM4(RTC_WD_CTL) |= 1u;
}

static inline void delay_loops(unsigned n) {
    do {
        __asm__ __volatile__("" ::: "memory");
    } while (--n);
}

/*
 * Generic read-only SPI command -> DMA0 receive helper (modeled 1:1 on FUN_00002224).
 * Sends tx_len command/address/dummy bytes, then clocks rx_len bytes into dst_sram via DMA0
 * with hardware descrambler DISABLED (raw bytes from the flash pins).
 */
static int spi_cmd_dma_read(const uint8_t *tx, unsigned tx_len, uint32_t dst_sram, unsigned rx_len) {
    unsigned i;
    int rc;

    /* CS0, raw mode (clear bit 9 CS1 and bit 12 scramble) */
    SPI0_CTL &= ~(0x200u | 0x1000u);
    /* Pulse CS# high then low */
    SPI0_CTL |= 0x8u;
    SPI0_CTL &= ~0x8u;
    /* Set TX-only mode (2) */
    SPI0_CTL = (SPI0_CTL & ~0x3u) | 0x2u;

    for (i = 0; i < tx_len; i++) {
        SPI0_TXDAT = tx[i];
    }
    rom_wait_bits(SPI0_BASE + 0x04u, 0x10u, 0x10u, 5u);
    rom_wait_bits(SPI0_BASE + 0x04u, 0x40u, 0x00u, 5u);

    /* Configure DMA0 <- SPI0_RXDAT */
    SPI0_BC = rx_len;
    DMA0_SRC = SPI0_RXDAT;
    DMA0_DST = dst_sram;
    DMA0_LEN = rx_len;
    DMA0_MODE = 0x00200088u;

    SPI0_CTL &= ~0x3u;
    SPI0_CTL &= ~0x30u;
    SPI0_CTL |= 0x30u;
    SPI0_CTL |= 0x80000040u;
    DMA0_START = 1u;
    SPI0_CTL = (SPI0_CTL & ~0x3u) + 1u;

    rc = rom_wait_bits(0xc0070104u, 1u, 0u, 0x5dcu);

    /* Deassert CS# and clear DMA/BC bits */
    SPI0_CTL |= 0x8u;
    SPI0_CTL &= 0x7fffefbcu;
    return rc;
}

void *entry_main(void) {
    volatile uint32_t *p = (volatile uint32_t *)ARGS_BASE;
    uint32_t cmd = p[4];

    wd_pet();

    /* Default safe return descriptor: points to p[2] (0x11ffe8), where p[2..3] = 0 so p[6] != 'N' */
    p[0] = ARGS_BASE + 8u;
    p[1] = 8u;
    p[2] = 0u;
    p[3] = 0u;

    if (cmd == 1u) {
        /* CMD 1: Safe Read-Only Init + Read JEDEC ID, Status Regs, 64-bit Unique ID, MMIO, and SFDP */
        uint8_t tx[8];
        unsigned i;
        volatile uint32_t *out32 = (volatile uint32_t *)DATA_BUF;

        for (i = 0; i < 0x200u / 4u; i++) {
            out32[i] = 0u;
        }

        rom_spinor_clk_init();
        rom_spinor_pinmux_set(0);
        /* Wake up from deep power-down (0xAB) + soft reset (0xFF) ONLY (no 0x06 WREN!) */
        rom_spinor_send_cmd1(0xabu);
        delay_loops(2000u);
        rom_spinor_send_cmd1(0xffu);
        delay_loops(1000u);

        /* +0x00: JEDEC ID (0x9F) -> 4 bytes */
        tx[0] = 0x9fu;
        spi_cmd_dma_read(tx, 1u, DATA_BUF + 0x00u, 4u);

        /* +0x04..+0x0f: Status Registers SR1 (0x05), SR2 (0x35), SR3 (0x15) */
        tx[0] = 0x05u;
        spi_cmd_dma_read(tx, 1u, DATA_BUF + 0x04u, 4u);
        tx[0] = 0x35u;
        spi_cmd_dma_read(tx, 1u, DATA_BUF + 0x08u, 4u);
        tx[0] = 0x15u;
        spi_cmd_dma_read(tx, 1u, DATA_BUF + 0x0cu, 4u);

        /* +0x10..+0x1f: 64-bit Unique ID (0x4B + 4 dummy bytes + 8 UID bytes -> read 12 bytes) */
        tx[0] = 0x4bu; tx[1] = 0u; tx[2] = 0u; tx[3] = 0u; tx[4] = 0u;
        spi_cmd_dma_read(tx, 5u, DATA_BUF + 0x10u, 12u);

        /* +0x20..+0x9f: Safe 32-bit snapshot of active clocked MMIO registers */
        out32[0x20u / 4u + 0] = MEM4(0xc0000000u); /* CMU_DEVRST */
        out32[0x20u / 4u + 1] = MEM4(0xc0000004u); /* CMU_DEVRST2 */
        out32[0x20u / 4u + 2] = MEM4(0xc0001000u); /* CMU_COREPLL */
        out32[0x20u / 4u + 3] = MEM4(0xc0001004u); /* CMU_DEVPLL */
        out32[0x20u / 4u + 4] = MEM4(0xc0001008u); /* CMU_DEVCLKEN */
        out32[0x20u / 4u + 5] = MEM4(0xc0001020u); /* CMU_SPI0CLK */
        out32[0x20u / 4u + 6] = MEM4(0xc0010000u); /* PMU_CTL */
        out32[0x20u / 4u + 7] = MEM4(0xc0010020u); /* PMU_SYS */
        out32[0x20u / 4u + 8] = MEM4(0xc0010028u); /* PMU_CHG */
        out32[0x20u / 4u + 9] = MEM4(0xc003001cu); /* RTC_WD_CTL */
        out32[0x20u / 4u + 10] = MEM4(0xc00a0000u); /* SPI0_CTL */
        out32[0x20u / 4u + 11] = MEM4(0xc00a0004u); /* SPI0_STAT */
        for (i = 0; i < 18u; i++) {
            out32[0x50u / 4u + i] = MEM4(0xc01c0000u + i * 4u); /* GPIO / MFP */
        }

        /* +0x100..+0x1ff: 256-byte JEDEC SFDP table (0x5A + 24-bit addr 0 + 1 dummy byte) */
        tx[0] = 0x5au; tx[1] = 0u; tx[2] = 0u; tx[3] = 0u; tx[4] = 0u;
        spi_cmd_dma_read(tx, 5u, DATA_BUF + 0x100u, 256u);

        p[2] = 0x53504930u; /* "0IPS" / magic ack */
    } else if (cmd == 2u) {
        /* CMD 2: Read N bytes (multiple of 512) from SPI NOR into DATA_BUF (0x11a000) */
        uint32_t spi_off = p[5];
        uint32_t total_len = p[6];
        uint32_t encrypt_flag = p[7];
        uint32_t done = 0u;
        int rc = 0;

        MEM4(0x0010009cu) = encrypt_flag;
        MEM4(0x001000a0u) = 0u;
        MEM4(0x001000a8u) = 0u;

        while (done < total_len) {
            wd_pet();
            rc |= rom_spinor_read_sector(spi_off + done, 0x200u, DATA_BUF + done, 0u);
            done += 0x200u;
        }
        p[2] = (uint32_t)rc;
        p[3] = done;
    } else if (cmd == 3u) {
        /* CMD 3: Restore MFP & Clocks */
        rom_spinor_pinmux_restore(0);
        rom_spinor_clk_deinit();
        p[2] = 0x444f4e45u;
    } else if (cmd == 4u) {
        /* CMD 4: Populate FWSC=1 response so adfus.bin 0x23 + 0xB0 triggers clean watchdog reboot */
        volatile uint8_t *b = (volatile uint8_t *)DATA_BUF;
        unsigned i;
        for (i = 0; i < 16u; i++) b[i] = 0u;
        b[8] = 'F'; b[9] = 'W'; b[10] = 'S'; b[11] = 'C'; b[12] = 1u;
        p[0] = DATA_BUF;
        p[1] = 16u;
    }

    return (void *)p;
}
