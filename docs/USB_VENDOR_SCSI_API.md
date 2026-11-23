# USB Mass Storage & Hidden Vendor SCSI API Specification

All findings below combine **live USB probing & full ADFU extraction** on your `ATJ2157` voice recorder with static binary decompilation in **REA MCP + Ghidra 12.1.4**.

### Key REA MCP Evidence Ledger
| REA Evidence ID | Target Artifact | Verified Finding |
| :--- | :--- | :--- |
| `ev_1f3677019e22e11251655af4711a6c35d3862a12d02fd9cb5d5acf3a703fad5b` | `atj2157_bootrom_sram.elf` | `ATJ2157` ARM Mask Boot ROM `entry()` (`0x00000188`): eFuse check, NAND/SPI0/SD boot sequence, and ADFU fallback (`0x000048c4`). |
| `ev_6a145c63544a453ab61c6ac91aa373a8b733f83a17c2cdc2221bfbbee5f941d0` | `atj2157_bootrom_sram.elf` | `FUN_00001b14`: `ATJ2157` SPI0 NOR boot loader (`0x0`, `0x1000`, `0x10000` sector scan, `encrypt` toggle `@ 0x10009c`). |
| `ev_8a4dcc52213a9ac6d342875c934c17ce91b032d17e21c782c10ecb761556e709` | `atj2157_bootrom_sram.elf` | `FUN_00002224`: `ATJ2157` SPI0 `0x0B` Fast Read + `DMA0` (`0xc0070100`) + `SPI0_CTL` bit 12 (`0x1000`) hardware descrambler. |
| `ev_e51400b23aa1cc81cbc7814fa07f449a81c93384c5a6361af76c74151044a3dd` | `brec_atj2157.elf` | `BREC` `FUN_0010f900`: Proves `SRAM[0x00130000] == 0xadf0adf0` ADFU warm-reboot magic and `LRADC9` (`0xa1..0x13f`) USB recovery button check. |
| `ev_62e6ba26086e86e779d94f5102fde6fda2b8b5d100299af7ad7f1fdfbe68eee9` | `adfus.elf` | Pre-flight Thumb-2 verification of `adfus.bin` (`0x00118000`), confirming `fwsc_flag == 1` requirement for `0xB0` Watchdog reset. |
| `ev_8d2c789d617a07125ac54b8c44166ec68b67bd2098396164e4539b85b7cb4fa0` | `spiread_atj2157.elf` | Pre-flight Thumb-2 verification of read-only `spiread_atj2157.bin` (`0x0011e000`). |
| `ev_8e32a998...` / `ev_13eee06e...` / `ev_4a0791f7...` / `ev_658eff19...` | `udisk_ap` | Decompilation of vendor SCSI `0xB0` (`set_time_and_date`), `0xCB` (`SwitchToAdfu`), `0xCC` (`IsActions`), `0xCD`, and `0xDD`. |

---

## 1. Normal USB Mass Storage Mode (`10d6:1101`, `UDISK.AP`)

Both standard SFF-8070i block commands and hidden Actions vendor commands use Bulk-Only Transport (`CBW` `"USBC"` on `EP2 OUT 0x02`, `CSW` `"USBS"` on `EP1 IN 0x81`). In unpacked `UDISK.AP` (`0x00108800`), the vendor opcode dispatcher at `0x0010a16c` (`cmp r0, #0xcb`) handles:

| `CDB[0]` | Handler | Direction | Description |
| :--- | :--- | :--- | :--- |
| **`0xB0`** | `set_time_and_date` | `DATA-OUT` (`7 B`) | Syncs device RTC (`<uint16_le Year, u8 Month, Day, Hour, Min, Sec>`). Implemented in [sync_rtc.py](../tools/sync_rtc.py). |
| **`0xCA`** | *(ping / no-op)* | None | Immediate return. |
| **`0xCB`** | `SwitchToAdfu` | `DATA-IN` (`2 B`) | After `0xCC(11)` `"ACTIONSUSBD"`, `CB 21 00...` returns `ff 00`, writes `0xadf0adf0` to `SRAM[0x00130000]`, and reboots into `10d6:10d6` ADFU. |
| **`0xCC`** | `IsActions` | `DATA-IN` (`11/20 B`) | `len=11` $\rightarrow$ `"ACTIONSUSBD"`; `len=20` $\rightarrow$ `\0CUSBD \xffv\0\0A\0...`. |
| **`0xCD`** | `deal_encrypt` | Mixed | Hidden/encrypted partition management (`CDB[1] = 0xE0`). |
| **`0xCE`** | `autorunread` | Mixed | Virtual CD-ROM autorun toggle. |
| **`0xDD`** | `GetPhilipsInfo` | `DATA-IN` (`2048 B`) | Reads 2 KB via `base_sd_read`. |

---

## 2. `ATJ2157` ADFU Protocol (`10d6:10d6`)

All ADFU commands use 31-byte `USBC` CBWs with **`dCBWTag = 0`**.

### 2.1 Stage 1 — Mask Boot ROM (`0x00000000`)
Supports **only** 4 primitives (max chunk `0x200` bytes; rejects `0x21` `EXEC` with `CSW status = 2`):
1. `CDB[0] = 0xCC` (`len = 0x12`, `DATA-IN`): Returns `00 43 41 44 46 55 44 30 51 41 00...` (`0x3051` = `ATJ2157`).
2. `CDB[0] = 0xCD, subcmd = 0x13` (`WRITERAM`): Write `len` bytes to SRAM `addr`.
3. `CDB[0] = 0xCD, subcmd = 0x93` (`READRAM`): Read `len` bytes from SRAM `addr` (`0x00000000..0x0000ffff` is masked by Boot ROM).
4. `CDB[0] = 0xCD, subcmd = 0x20` (`SWITCH`, `addr = 0x00118000`): Transfers USB control to `adfus.bin` in SRAM.

### 2.2 Stage 2 — Secondary ADFU Server (`adfus.bin` @ `0x00118000`)
Unlocks 16 KB (`0x4000`) DMA transfers and custom Thumb-2 code execution:
- **Important**: Do **not** send `0xCC` or `0x12` once `adfus.bin` is active (`adfus.bin` marks unknown opcodes with `usbs_error = 2` and exits its loop on the next command).
- `0xCD` subcommands:
  - `0x13` / `0x93`: `WRITERAM` / `READRAM` (unmasked — can read anywhere or pair with Thumb-2 stub @ `0x0011e001`).
  - `0x21` (`EXEC`, `addr | 1`): Calls Thumb-2 payload `void *entry_main(void)` which **must** return an SRAM pointer `r0` where `((uint32_t *)r0)[0]` is a valid SRAM buffer address and `((uint32_t *)r0)[1]` is byte length.
  - `0x22` / `0x23` (`RETSIZE` / `READRET`): Reads return size / buffer. If `buf[8..12] == "FWSC\x01"`, sets `fwsc_flag = 1`.
- **Clean Exit / Hardware Reboot (`0xB0`)**:
  1. Write `16` zero bytes to `0x00130000` (clears `0xadf0adf0` so `BREC` boots normal firmware instead of re-entering ADFU).
  2. Set `fwsc_flag = 1` via `0x23` returning `"........FWSC\x01..."`.
  3. Send `CDB[0] = 0xB0` (`len = 0`) $\rightarrow$ returns `USBS` cleanly and triggers `(MEM4(0xc003001c) & ~0x2e) | 0x11` Watchdog reset.
