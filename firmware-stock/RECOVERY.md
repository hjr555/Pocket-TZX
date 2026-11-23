# Actions `ATJ2157` (`10d6:1101` / `10d6:10d6`) Factory Backup & Recovery Runbook

## 1. Verified Factory Backup Inventory (`firmware-stock/`)

All checksums are recorded in [SHA256SUMS](SHA256SUMS):

| Backup File | Size | Description & Restoration Role |
| :--- | :--- | :--- |
| `spi_nor_raw_4m.bin` | `4,194,304 B` | **Bit-for-bit raw physical SPI NOR image** (`SPI0_CTL` bit 12 = `0`). Direct image for external 3.3V SOIC-8 hardware programmer (`flashrom`). |
| `spi_nor_decrypted_4m.bin` | `4,194,304 B` | **Full 4 MiB hardware-descrambled logical image** (`SPI0_CTL` bit 12 = `1`). Image written via `ATJ2157` `SPI0` with hardware scrambler enabled. |
| `mbrec_512b.bin` | `512 B` | Stage-1 Master Boot Record (`0x000000..0x0001ff`, checksum `0xe40e` valid). **Keep untouched.** |
| `brec_45k.bin` | `45,568 B` | Stage-2 Boot Record (`0x000200..0x00b3ff`, 89 sectors). **Keep untouched.** |
| `lfi_stock.bin` | `4,011,520 B` | Factory `LFI` OS & application filesystem (`0x00b400..0x3de9ff`, `1.101.56`, 75 files, `0` checksum errors). |
| `vram_nvram_tail_133k.bin` | `136,704 B` | Factory `VRAM` / `NVRAM` settings & tail (`0x3dea00..0x3fffff`). |
| `bootrom_64k.bin` | `65,536 B` | `ATJ2157` immutable 64 KB ARMv7-M Mask Boot ROM (`0x00000000..0x0000ffff`) for offline decompilation. |
| `sram_untouched_256k.bin` | `262,144 B` | Untouched 256 KB SRAM snapshot (`0x00100000..0x0013ffff`) captured immediately upon ADFU entry. |
| `spi_nor_info_512b.bin` | `512 B` | Raw JEDEC SFDP v1.8 table (`0x5A`, Vendor `0x5E` Zbit 32-Mbit) + clocked MMIO register dump (`mmio_atj2157.json`). |

---

## 2. SD Card Partitioning & Format Rules
- **Supported Filesystems**: `FAT16N.DRV`, `FAT32N.DRV`, and `EXFATN.DRV` are all present in `lfi_stock.bin`. Standard **MBR single primary partition `FAT32`** (`0x0c` LBA) is recommended for clean compatibility with both the recorder and ZX Spectrum Next.
- **USB Enumeration Note**: A microSD card **must** be inserted in the recorder's slot for `MANAGER.AP` to launch `UDISK.AP` (`10d6:1101`, LCD `'connected'`) when plugged into USB; ensure the USB plug is seated all the way into the recessed jack so `D+`/`D-` engage rather than 5V charge-only mode.

---

## 3. Three-Tier Hardware & Software Recovery Triggers

### Tier 1 — Normal Software ADFU Entry (`10d6:1101` $\rightarrow$ `10d6:10d6`)
With a microSD card inserted and the recorder showing `'connected'` (`10d6:1101`):
```bash
python3 tools/adfu_dump_all.py --stay-in-adfu
```
*(Ensure `/etc/udev/rules.d/70-actions.rules` from [tools/70-actions.rules](../tools/70-actions.rules) is installed so non-root user tools retain `rw` access to `10d6:10d6`.)*

### Tier 2 — Cold-Boot Hardware Button Recovery (`BREC` `LRADC9` / Mask ROM `10d6:10d6`)
Proven by decompiling `BREC` (`FUN_0010f900` @ `0x10f900`):
1. Unplug USB and slide/switch the recorder **OFF** (wait past the ~0.4s `"BYE"` screen).
2. Press and **hold the recovery key** (`LRADC9` on `GPIO55` reading ADC `161..319`, e.g. Play/Menu key) **while plugging in the USB data cable**.
3. `BREC` detects `DC5V_ON` + `LRADC9` in range and jumps straight to Boot ROM ADFU (`10d6:10d6`) **before** `LFI` (`KERNEL.DRV` / `.AP` apps) is loaded! Because we keep `MBREC` (`0x000000`) and `BREC` (`0x000200..0x00b3ff`) 100% factory untouched during userland `LFI` tweaks, Tier 2 recovery remains intact even if `LFI` fails to boot.

### Exiting ADFU Cleanly on Soft-Switch Hardware
- **Software Exit**: `adfu_dump_all.py` clears `SRAM[0x00130000]` (`0xadf0adf0` $\rightarrow$ `0`), sets `fwsc_flag = 1`, and sends `0xB0` (`reset`) to trigger a clean Watchdog reboot (`0xc003001c`).
- **If `0x00130000` is NOT cleared**: `BREC` sees `0xadf0adf0` in SRAM across warm resets and loops right back into `10d6:10d6`!

### Tier 3 — External 3.3V SOIC-8 Hardware SPI Programmer (Ultimate Disaster Recovery)
- Open case, disconnect battery (`B+`), clip a 3.3V SOIC-8 clip (`CH341A` / Raspberry Pi SPI / Tigard) onto the 8-pin Zbit `25VQ32` SPI NOR (`GPIO12..15`), verify `sha256sum -c SHA256SUMS`, and write **`spi_nor_raw_4m.bin`** (`a9197b566d1d817fbb88e3d0c00fd03038017d79fea13d687a7707e415978f5c`).
