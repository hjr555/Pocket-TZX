# ZX Spectrum Next `.TZX` Mode — Architecture & Implementation Plan (`ATJ2157`)

## Executive Verdict: **100% Feasible — Open Boot Chain + ARM Thumb-2 + 133 KB Free Flash**

Now that we have dumped and verified the complete `ATJ2157` Boot ROM, `BREC`, and 75-file `1.101.56` `LFI` firmware image, **every single technical requirement for a native `.TZX` / `.TAP` mode is confirmed**:

1. **Zero Cryptographic Lockout**: `MBREC`, `BREC`, and `LFI` use plain additive checksums (`fwhelper` validates all 75 files with `0` errors), and `SPI0_CTL` bit 12 (`0x1000`) handles SPI NOR scrambling in hardware.
2. **Modern Compiler Ready**: The `ATJ2157` runs standard **ARMv7-M Thumb-2** code; our local `vendor/toolchain/zig` (`-target thumb-freestanding-eabi -mcpu=cortex_m3`) already compiled and executed bare-metal Thumb-2 payloads cleanly on your unit.
3. **Ample Code & Flash Headroom**:
   - **32 KB Audio/Codec SRAM (`0x00300000..0x00307fff`)**: `ADWAV.AL` (`0x303e00`, `init = 0x304bd1`) uses only **`4,028` bytes (`0xfbc`)** of resident code!
   - **133 KB (`136,704 B`) Unused SPI NOR Space**: `LFI` ends at `0x3dea00` out of `0x400000`, leaving plenty of room for new overlay banks or a dedicated `.AL` / `.AP` module without removing a single stock feature.
4. **Clean Square-Wave Audio Path (`0xc0180000`)**:
   - Stock `CONFIG.TXT` already defaults to `EQ_SETTING = 0`, `FULLSOUND = 0`, `KEYTONE_ENABLE = 0`, and 31-step hardware PA gain (`VOLUME_CURRENT = 28..31`), driving clean rail-to-rail 44.1/48 kHz square-wave pulses into the ZX Spectrum Next `EAR` jack.

---

## Recommended Implementation Options

### Option 1 (Recommended — Best UX / Lowest Risk): **Extend `ADWAV.AL` + `MUSIC.AP`/`BROWSER.AP` Extension Filter for Native `.TZX` & `.TAP` Playback**
- **How It Works**:
  1. Add `.TZX` and `.TAP` to the music file-scanner extension bitmask in `MUSIC.AP` / `BROWSER.AP` / `MENGINE.AP` (where `.WAV` is matched) and route `.TZX`/`.TAP` to `ADWAV.AL` (or a new `ADTZX.AL` slot in `MMM_MP.AL`).
  2. Inside `ADWAV.AL` (`0x303e00`), detect `"ZXTape!\x1a"` (`.TZX` header) or `.TAP` block headers during codec `OPEN`, report **48,000 Hz Mono 16-bit PCM** to `MMM_MP.AL`, and synthesize exact Z80 T-state pulses ($3.5\text{ MHz}$ clock with 32-bit fractional sample accumulator, $< 0.5\text{ T-state}$ jitter) on the fly into the PCM output buffer!
  3. Support `.TZX` blocks `0x10` (Standard Speed), `0x11` (Turbo Speed), `0x12` (Pure Tone), `0x13` (Pulse Sequence), `0x14` (Pure Data), `0x15` (Direct Recording), `0x20` (Pause / Silence — or optional auto-pause hook for multi-load games!), `0x21`/`0x22` (Groups), `0x24`/`0x25` (Loops), and `0x2B` (Set Signal Level).
- **Why It Shines**:
  - Zero custom LCD UI drawing code needed — **100% of your existing LCD folder browser, track list, elapsed/total time bar, Play/Pause (`II`), Next/Prev track skip, and side volume buttons work automatically!**
  - User partition stays untouched (`loader`/`MBREC`/`BREC`/`KERNEL.DRV` remain 100% factory stock; only userland `LFI` codec/app entries change).

### Option 2: **Dedicated `TZX` Mode Inside `TOOLS.AP` (or Standalone `TZX.AP`)**
- Add a `"TZX Player"` menu entry inside `TOOLS.AP` (or replace an unused app slot in `MAINMENU_AP_ID_ARRAY = 6[0, 6, 7, 5, 10]`) with custom block-by-block LCD readout (`Block 03/12: Turbo 19200`, manual/auto `Stop the Tape` `0x20` prompt, and Turbo 2x/4x Spectrum Next `EAR` mode).

---

## Mandatory Flash Safety & Verification Checklist (Before Any Future Write)

Per embedded flash safety rules, **before** writing any modified `LFI` back to SPI NOR (`0x00b400..0x3de9ff`, leaving `MBREC` `0x000000` and `BREC` `0x000200` untouched!):
1. **Offline Container & Decompiler Verification**:
   - Recompute `YQHX` header, file 32-bit word-sum, `LFI` directory checksum, and `LFI` header checksum (`fwhelper scan_lfi` must report `0` errors), and inspect every patched ARM Thumb-2 procedure in **REA MCP + Ghidra 12.1.4**.
2. **Hardware Scrambler Round-Trip Test**:
   - Test writing + reading back a single unused scratch block **`0x3df000..0x3dffff`** (all `0xFF`, between `LFI` end `0x3dea00` and `VRAM` `0x3e0000`; never `0x3ff000`, which is inside `VRAM`) via `python3 tools/adfu_flash_lfi.py --self-test-scratch --yes` to prove raw and hardware-scrambled (`SPI0_CTL |= 0x2000`, bit 15 released for the data phase only, as in `BREC` `FUN_00114118`) program paths before touching `LFI`.
3. **3-Tier Verified Rollback Plan Ready**:
   - Tier 1: Software ADFU (`10d6:1101` $\rightarrow$ `0xCB 0x21`).
   - Tier 2: Cold-boot `BREC` `LRADC9` button-hold USB recovery (`10d6:10d6` — untouched because `MBREC`/`BREC` at `0x000000..0x00b3ff` are never modified!).
   - Tier 3: External 3.3V SOIC-8 clip + `firmware-stock/spi_nor_raw_4m.bin` (`SHA256 = a9197b56...`).
    - Restore command: `python3 tools/adfu_flash_lfi.py --restore-stock --yes` (raw, bit-exact rewrite of only the blocks that differ from `spi_nor_raw_4m.bin` over `0x00b000..0x3defff`, full raw read-back verify).

---

## Implementation Status — Option 2 (Standalone `TZX Mode`, 6th Main Menu entry)

**State (22/11/2026): built and verified offline. Nothing has been written to the device yet.**

### What changes (5 of 75 `LFI` files, all same length, zero offset shifts)
| File | Change |
|---|---|
| `TOOLS.AP` (unused `APP_ID 9`) | Replaced by the `TZX Mode` app (`.text 0x3368`, `.bss 0x232c`, SRAM `0x108800..0x10de9c`, UDISK.AP-style single resident segment), zero-padded to the stock 110,592 B |
| `CONFIG.BIN` / `CONFIG.TXT` | `MAINMENU_AP_ID_ARRAY = 6[0,6,7,5,9,10]` |
| `MAINMENU.AP` | `VM_AP_MAINMENU` magic `0x55AA` → `0x55AB` (forces VRAM menu cache refresh on both upgrade and rollback) |
| `MAINMENU.STY` | Pic 6 → cassette icon (factory `<` `>` arrows kept); StrID 24 → `"TZX Mode"` in all 7 languages |

Flash footprint: **219 scrambled sectors + 93 raw-restored sectors in 39 × 4 KB blocks** (`0x00b000..0x3dcfff`). `MBREC`, `BREC` (incl. raw-restored tail `0x00b000..0x00b3ff`) and `VRAM` are never modified.

### Controls
| Button | Browser | Playback |
|---|---|---|
| Play/Pause | Open folder / load tape | Play / pause (resumes after auto-stop) |
| Stop | Up a folder / exit to menu | Back to browser |
| FWD / RWD | Move selection | Next / previous block |
| + / - | Volume | Volume |
| M | `48K STOP` on/off | `48K STOP` on/off |
| Record | – | Polarity NORM / INV |
| Power | Power off | Power off |

### Build & offline pre-flight (one command)
`tools/tzx_app/build.sh` → host `.TZX` engine tests, Thumb-2 build, `LFI` packaging, then `tools/verify_lfi_tzx.py` (size, `fwhelper` 0 checksum errors, round-trip unpack 70/75 identical + 5 intended, `TOOLS.AP` ≡ ELF, SRAM bounds, erase-block guard) and `tools/adfu_flash_lfi.py --plan`.

### Device tooling (`tools/adfu_flash_lfi.py` + `tools/spiflash_atj2157.c` SRAM payload)
Address guard enforced in both host and payload (erase `0x00b000..0x3df000`, program `< 0x3e0000`, scrambled program `>= 0x00b400`). Every block is backed up raw to `build/flash_sessions/<ts>/` before erase and verified after write; `--flash` ends with a full descrambled read-back of `LFI` and a raw check of the `BREC` tail.

Order of operations: `--verify-device` (read-only) → `--self-test-scratch --yes` → `--flash --yes`.
