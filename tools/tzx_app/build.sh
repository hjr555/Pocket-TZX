#!/usr/bin/env bash
# Reproducible build + offline pre-flight for the TZX Mode LFI image.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ZIG="$ROOT/vendor/toolchain/zig"
SRC="$ROOT/tools/tzx_app"
mkdir -p "$ROOT/build"

echo "== Host .TZX engine unit tests =="
gcc -O2 -Wall -Wextra "$SRC/tzx_engine.c" "$SRC/test_tzx_engine.c" -o "$ROOT/build/test_tzx_engine"
"$ROOT/build/test_tzx_engine"

echo "== Cross-compile tzx_ap.elf (Thumb-2, Cortex-M3) =="
# -g keeps .symtab for REA/Ghidra verification; only .text/.data are packaged into TOOLS.AP.
"$ZIG" cc -target thumb-freestanding-eabi -mcpu=cortex_m3 -Oz -g \
  -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -Werror \
  -Wl,-T,"$SRC/tzx_ap.ld" "$SRC/tzx_ap_main.c" "$SRC/tzx_engine.c" \
  -o "$ROOT/build/tzx_ap.elf"

echo "== Cross-compile spiflash_atj2157.bin (ADFU SRAM payload @ 0x0011e000) =="
"$ZIG" cc -target thumb-freestanding-eabi -mcpu=cortex_m3 -Os -g \
  -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -Werror \
  -Wl,-T,"$ROOT/dumps/spiread.ld" "$ROOT/tools/spiflash_atj2157.c" \
  -o "$ROOT/build/spiflash_atj2157.elf"
"$ZIG" objcopy -O binary -j .text "$ROOT/build/spiflash_atj2157.elf" "$ROOT/build/spiflash_atj2157.bin"

echo "== Package LFI =="
python3 "$ROOT/tools/build_tzx_lfi.py" --elf "$ROOT/build/tzx_ap.elf"

echo "== Offline pre-flight verification =="
python3 "$ROOT/tools/verify_lfi_tzx.py"
python3 "$ROOT/tools/adfu_flash_lfi.py" --plan
