# Hardware, Pinout, SPI NOR Layout & `ATJ2157` Register Reference

## 1. Confirmed Live Hardware & Silicon Identity

- **Main SoC**: **Actions Semiconductor `ATJ2157`**
  - Proven via ADFU Boot ROM `0xCC` (`adfu_info`): `00 43 41 44 46 55 44 30 51 41 00...` (`"\0CADFUD\x30\x51A"` $\rightarrow$ Chip ID **`0x3051` = `ATJ2157`**).
  - **CPU Core**: **ARMv7-M / Thumb-2 Cortex-M** (`VTOR = 0xe000ed08`, `SHCSR = 0xe000ed24`, `NVIC ISER0 = 0xe000e100`), with standalone C/Thumb-2 compilation supported out-of-the-box via `zig cc -target thumb-freestanding-eabi -mcpu=cortex_m3`.
  - **eFuse Boot Config Word (`SRAM 0x00107ef8`)**: `0x109f09ab` (read from `0xc0020000` by Boot ROM `FUN_0000082c`; bit 4 `0x10 = 1` skips parallel NAND scan at reset and boots straight from `SPI0` NOR flash).
- **Internal Boot Flash**: **4.00 MiB (`4,194,304` bytes) 3.0V SPI NOR Flash**
  - Proven via JEDEC SFDP v1.8 (`0x5A` in `spi_nor_info_512b.bin`): Signature `"SFDP"`, Basic Flash Parameter Table `DWORD2 = 0x01ffffff` (`33,554,432` bits = `4,194,304` bytes), Vendor Parameter Table ID **`0x5E`** (Zbit Semiconductor `ZB25VQ32` / `ZB25D32` 32-Mbit SPI NOR).
- **User Media Storage**: **External 4-bit microSD Card Slot** (`CARD.DRV` at `0xc0160000`; `GET_MAX_LUN = 0`).
  - Note: When connecting USB in normal mode, the recorder only launches `UDISK.AP` (`10d6:1101`, LCD `'connected'`) when a microSD card is inserted in the slot; without a card, it charges on USB while staying in the standalone menu.

---

## 2. Exact PCB GPIO & Multi-Function Pinout (from Live `CONFIG.TXT`)

Extracted directly from `CONFIG.TXT` inside our recorder's `lfi_stock.bin`:

| Subsystem | Signal | `ATJ2157` Pin | Notes |
| :--- | :--- | :--- | :--- |
| **SPI0 NOR Flash** | `SPI0_MISO` / `MOSI` / `SCLK` / `SS#` | `GPIO12` / `13` / `14` / `15` | Pinmux Group 0 (`0xc01c0034..0x40 = 0x3027`) |
| **Debug UART0** | `UART0_TX` / `UART0_RX` | `GPIO18` / *(unassigned `255`)* | Hardware serial debug output pad (`115,200` baud) |
| **microSD Slot** | `SD_CLK0` / `SD_CMD` | `GPIO30` / `GPIO32` | `CARD_CONTROLER_INDEX = 0`, 4-bit bus (`CARD_DATA_WIDTH = 4`) |
| **microSD Data** | `SD_D0` / `D1` / `D2` / `D3` | `GPIO33` / `29` / `28` / `27` | `SD_DET0 = 255` (polled via card command, not mechanical switch) |
| **8080 Parallel LCD** | `LCM_D0..D7` | `GPIO0 .. GPIO7` | 8-bit parallel data bus (`DRV_LCD.DRV` @ `0xc01a0000`) |
| **8080 Parallel LCD** | `RS` / `RDB` / `WRB` / `CE` / `RST` | `GPIO8` / `10` / `11` / `43` / `21` | `LCM_BL_PIN = GPIO22` driven by hardware **`PWM4`** (`1..11` brightness) |
| **Buttons (`LRADC`)** | `KEY_LRADC` | `GPIO55` (`LRADC9`) | 10-slot key table `CFG_KEY_ARRAY = [5,3,4,6,128,8,7,9,128,10]` |
| **FM / Aux I2C** | `I2C_SCL` / `I2C_SDA` | `GPIO24` / `GPIO25` | `DRV_I2C.DRV` (`FMDRV.DRV` absent from this voice-recorder build) |
| **ARM SWD Debug** | `MFP_SWD_MAP` | `255` *(disabled in config)* | Can be enabled in `CONFIG.TXT` (`0`: `GPIO62/63`, `1`: `GPIO55/56`, `2`: `GPIO16/17`)! |

---

## 3. 4.00 MiB SPI NOR Partition Map & Hardware Scrambler

Decompiling the `ATJ2157` Mask Boot ROM (`FUN_00001b14` $\rightarrow$ `FUN_00001984` $\rightarrow$ `FUN_00002224`, REA `ev_6a145c63...`, `ev_8a4dcc52...`) revealed that **`SPI0_CTL` (`0xc00a0000`) bit 12 (`0x1000`)** enables an on-chip **self-synchronizing hardware sector scrambler/descrambler**. Both the raw physical bits (`spi_nor_raw_4m.bin`, `encrypt=0`) and hardware-descrambled logical image (`spi_nor_decrypted_4m.bin`, `encrypt=1`) have been dumped and verified:

| Byte Range (`0x000000..0x3fffff`) | Sectors (`512 B`) | Size | Partition & Verified Contents |
| :--- | :--- | :--- | :--- |
| `0x000000 .. 0x0001ff` | `0` (`1` sec) | `512 B` | **`MBREC`** (`mbrec_512b.bin`): Checksum `0xe40e` (**VALID**). Loads `0x38` sectors of `BREC` to `0x0010e900` and jumps to `0x0010eb01`. |
| `0x000200 .. 0x00b3ff` | `1 .. 89` (`0x59` sec) | `45,568 B` | **`BREC`** (`brec_45k.bin`): Stage-2 NOR bootloader, PMU/LRADC ADFU check (`0x10f900`), SRAM bank init, and `KERNEL.DRV` loader (`fw_sec = 0x1e9b`). |
| `0x00b400 .. 0x3de9ff` | `90 .. 7924` (`0x1e9b` sec) | `4,011,520 B` | **`LFI` (`lfi_stock.bin`)**: Magic `0x0ff0aa55`, header csum `0xf5c0` (**VALID**), **75 files** (`0` file checksum errors!), FW `1.101.56`. |
| `0x3dea00 .. 0x3fffff` | `7925 .. 8191` (`267` sec) | `136,704 B` | **`VRAM / NVRAM`** (`vram_nvram_tail_133k.bin`): Persistent runtime settings (`VM_*`), user state, and **133 KB of free/headroom space** at the top of the 4 MiB flash! |

---

## 4. `ATJ2157` Memory Map & Hardware Peripheral Bases

### 4.1 CPU Address Space (ARMv7-M)
| Address Range | Size | Description |
| :--- | :--- | :--- |
| `0x00000000 .. 0x0000ffff` | `64 KB` | **Mask Boot ROM** (`SP = 0x00101b18`, `Reset_Handler = 0x00000189`; exports callback table at `0x00100000`) |
| `0x00100000 .. 0x0013ffff` | `256 KB` | **Primary On-Chip SRAM** (`0x110000` Foreground `.AP` code, `0x110800` `MENGINE.AP`, `0x118000` `adfus.bin`, `0x119f80` `KERNEL.DRV`) |
| `0x00200000 .. 0x00203fff` | `16 KB` | **Data SRAM Bank** (`.AP` `RDATA`/`BSS` at `0x200000`/`0x200800`, driver data at `0x201e00..0x203670`) |
| `0x00300000 .. 0x00307fff` | `32 KB` | **Audio / Codec / Driver SRAM Bank** (`FAT32N/EXFATN.DRV` `@ 0x301200`, `MMM_MP.AL` `@ 0x303600`, `AD*.AL` codecs `@ 0x303e00`, `AUD_DEV.DRV` `@ 0x307140`) |
| `0xc0000000 .. 0xc020ffff` | — | **On-Chip Peripheral MMIO** |
| `0xe000e000 .. 0xe000edff` | — | **ARMv7-M System Control Space (`SCS` / `NVIC` / `VTOR` / `SysTick`)** |

### 4.2 Verified `ATJ2157` Peripheral Register Map
| Base Address | Peripheral | Key Observed Registers & Bitfields |
| :--- | :--- | :--- |
| `0xc0000000` | `CMU_DEVRST` | Module Reset (`bit 0` = DMA, `bit 1` = NAND, `bit 2` = SD, `bit 6` = SPI0, `bits 9..10` = SRAM `0x200000`/`0x300000`) |
| `0xc0001000` | `CMU_CLK` | `COREPLL` (`+0x00`), `DEVPLL` (`+0x04`), `DEVCLKEN` (`+0x08`), `SPI0CLK` (`+0x20`), **`AUDIOPLL` / Audio Clocks (`+0x80..+0x94`)** |
| `0xc0010000` | `PMU / LRADC` | `PMU_CTL` (`+0x00`), `SYSTEM_CTL` (`+0x20`, bit 0/1 power hold), `CHG/WAKE_PD` (`+0x28`, bit 30 `DC5V_ON`), **`LRADC9_DATA` (`+0x70`)** |
| `0xc0020000` | `EFUSE` | OTP eFuse controller |
| `0xc0030000` | `RTC / WD / TMR` | **`WD_CTL` (`+0x1c`)**: `|= 1` pets watchdog; `(val & ~0x2e) | 0x11` triggers hardware reset. Hardware timers at `+0x100..+0x120` |
| `0xc0070000` | `DMA` | `DMA0` (`+0x100..+0x118`: `MODE`, `START`, `SRC`, `DST`, `LEN`), `DMA1` (`+0x200..+0x218`) |
| `0xc00a0000` | **`SPI0`** | `SPI0_CTL` (`+0x00`: bit 3 `CS#`, bit 12 `SCRAMBLE_EN`, bit 31 `BC_EN`), `STAT` (`+0x04`), `TXDAT` (`+0x08`), `RXDAT` (`+0x0c`), `BC` (`+0x10`) |
| `0xc0140000` | `USB_OTG` | USB 2.0 High-Speed controller (`EP1 IN` / `EP2 OUT` DMA at `+0x308..+0x330`) |
| `0xc0160000` | `SD / MMC` | 4-bit microSD controller (`CARD.DRV`) |
| **`0xc0180000`** | **`DAC / ADC / PA`** | **Audio Subsystem (`AUD_DEV.DRV`)**: DAC FIFO, Direct-Drive Headphone PA (`+0x1000..+0x310c`), Mic/Line-In ADC |
| `0xc01a0000` | `LCDC` | 8080 8-bit parallel LCD controller (`DRV_LCD.DRV`) |
| `0xc01c0000` | `GPIO / MFP / PWM` | Multi-function pin mux (`+0x00..+0xe0`), `PWM` controller (`+0x200..+0x210`, `PWM4` LCD backlight) |

---

## 5. ADFU Persistence & Clean Reboot Mechanism (`0x00130000 = 0xadf0adf0`)

Decompiling `BREC` (`FUN_0010f900` at `0x10f900`, REA `ev_e51400b2...`) uncovered two critical boot behaviors on this soft-switch recorder:
1. **Software ADFU Magic in SRAM (`0x00130000`)**: When `UDISK.AP` handles `SwitchToAdfu` (`0xCB 0x21`), it writes **`0xadf0adf0`** to `SRAM[0x00130000]` before triggering a Watchdog warm reset. Because SRAM retains contents across Watchdog resets, `BREC` checks `if (*(uint32_t *)0x00130000 == 0xadf0adf0)` at boot and immediately jumps back into Boot ROM ADFU (`0x000048c5`)!
   - **Rule**: To reboot cleanly out of ADFU back to normal firmware, **always zero `0x00130000` (`write_ram(0x00130000, b"\x00"*16)`)** and arm `fwsc_flag = 1` before sending `0xB0` (`reset`) — automated in [adfu_dump_all.py](../tools/adfu_dump_all.py).
2. **Cold-Boot Hardware Recovery Key (`LRADC9` on `GPIO55`)**: Whenever USB 5V (`DC5V_ON`) is present at boot, `BREC` samples `LRADC9` (`0xc0010070 & 0x3ff`) 5 times; if a front/side button holding ADC count **`161..319` (`0xa1..0x13f`)** is held while plugging in USB, `BREC` enters ADFU (`"welcome adfu"`) before `KERNEL.DRV` or any `.AP` module ever runs!
