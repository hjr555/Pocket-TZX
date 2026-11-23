# Actions Semiconductor (`10d6:1101`) Digital Voice Recorder — RE & TZX Mode Project

Reverse-engineering workspace, complete factory firmware backup, hardware/OS specifications, tooling, and **ZX Spectrum Next `.TZX` tape-player firmware design** for the generic Chinese `"DIGITAL VOICE RECORDER"` built on the **Actions Semiconductor `ATJ2157` (ARMv7-M / Cortex-M)** platform.

## Verified Live Hardware & Firmware Fingerprint

| Property | Verified Value |
| :--- | :--- |
| **Main SoC** | **Actions Semiconductor `ATJ2157`** (`adfu_info` ID `0x3051`, ARMv7-M Thumb-2 Cortex-M core) |
| **USB VID:PID** | `10d6:1101` (Normal High-Speed MSC) / `10d6:10d6` (Mask Boot ROM ADFU) |
| **Internal Boot Flash** | **4.00 MiB (`4,194,304 B`) SPI NOR** (Zbit Semiconductor, SFDP v1.8 Vendor `0x5E`, hardware-scrambled via `SPI0_CTL` bit 12) |
| **User Storage** | External **4-bit microSD** (`GET_MAX_LUN = 0`; `FAT16N.DRV` + `FAT32N.DRV` + `EXFATN.DRV` all present!) |
| **Stock Firmware (`LFI`)** | Version **`1.101.56`**, SDK **`20140616_SVN14664_4S_AFI`**, **75 files** (`0` checksum errors), `3.83 MiB` (`0x3d3600 B`) |
| **SRAM Architecture** | `256 KB` main SRAM (`0x00100000..0x0013ffff`) + `0x00200000` data bank + `32 KB` Audio/DSP SRAM (`0x00300000..0x00307fff`) |
| **Display & Controls** | 8-bit 8080 parallel LCD (`GPIO0..8, 10, 11, 21, 22 PWM4, 43`), `LRADC9` resistor-ladder keys (`GPIO55`), soft power switch (`~0.4s "BYE"`) |
| **Audio Subsystem** | Hardware Audio PLL (`0xc0001080`) + 24-bit DAC / Direct-Drive Headphone PA (`0xc0180000`) + 3.5 mm Earphone & Mic jacks |
| **OS & App Stack** | **µC/OS-II RTOS** + Actions **PSP** (`KERNEL.DRV`) + **CASE** framework (`YQHX` v4.0 ARM Thumb-2 `.AP` / `.DRV` / `.AL` overlays) |

## Repository Layout

- **Factory Backups & Recovery**:
  - [firmware-stock/RECOVERY.md](firmware-stock/RECOVERY.md) — Multi-tier software, `LRADC9` button cold-boot, and external SOIC-8 hardware recovery runbook.
  - [firmware-stock/SHA256SUMS](firmware-stock/SHA256SUMS) — SHA-256 manifest covering all 14 dumped factory artifacts (`bootrom_64k.bin`, `sram_untouched_256k.bin`, `spi_nor_raw_4m.bin`, `spi_nor_decrypted_4m.bin`, `mbrec_512b.bin`, `brec_45k.bin`, `lfi_stock.bin`, `vram_nvram_tail_133k.bin`, `spi_nor_info_512b.bin`, `mmio_atj2157.json`, USB descriptors, and signatures).
- **Technical Documentation**:
  - [docs/HARDWARE_AND_USB.md](docs/HARDWARE_AND_USB.md) — `ATJ2157` ARM memory & peripheral map, exact PCB GPIO pinout (from unpacked `CONFIG.TXT`), 4 MiB SPI NOR layout & hardware scrambler, and ADFU exit mechanics (`0x00130000 = 0xadf0adf0`).
  - [docs/USB_VENDOR_SCSI_API.md](docs/USB_VENDOR_SCSI_API.md) — Complete SCSI dispatch table, hidden vendor opcodes (`0xB0` RTC sync, `0xCB` ADFU switch, `0xCC` `IsActions`, `0xCD`, `0xDD`), and `ATJ2157` ADFU Boot ROM + `adfus.bin` protocol.
  - [docs/FIRMWARE_OS_ARCHITECTURE.md](docs/FIRMWARE_OS_ARCHITECTURE.md) — Complete 75-file `LFI` inventory, µC/OS-II + PSP + CASE OS layers, `YQHX` v4.0 ARM Thumb-2 overlay format, and `CONFIG.TXT` / `CONFIG.BIN` customization parameters.
  - [docs/TZX_MODE_FEASIBILITY.md](docs/TZX_MODE_FEASIBILITY.md) — Concrete architecture & implementation options for native **ZX Spectrum Next `.TZX` / `.TAP` playback** on `ATJ2157`.
- **Tools & Bare-Metal Payloads**:
  - [tools/70-actions.rules](tools/70-actions.rules) — Fedora/systemd `udev` rule granting non-root `uaccess` to `10d6:1101` and `10d6:10d6`.
  - [tools/adfu_dump_all.py](tools/adfu_dump_all.py) — End-to-end read-only `ATJ2157` Boot ROM, SRAM, and 4 MiB SPI NOR dumper + clean ADFU reboot utility.
  - [tools/spiread_atj2157.c](tools/spiread_atj2157.c) — Bare-metal ARMv7-M Thumb-2 read-only SPI NOR & SFDP dumper payload (`0x0011e000`).
  - [tools/yqhx_to_elf.py](tools/yqhx_to_elf.py) — Converts `ATJ2157` (ARM Thumb-2) and `ATJ2127` (MIPS) `YQHX` modules into `ET_EXEC` ELFs for Ghidra 12.1.4.
  - [tools/probe_actions_usb.py](tools/probe_actions_usb.py) & [tools/sync_rtc.py](tools/sync_rtc.py) — Non-destructive MSC probe and `0xB0` host-to-device RTC clock sync.
