#!/usr/bin/env python3
"""
Pre-flight verification for build/lfi_tzx.bin (read-only, offline).

Checks:
  1. Exact size == lfi_stock.bin (4,011,520 B / 0x1e9b sectors, as hardcoded in BREC).
  2. fwhelper scan_lfi reports zero '!!!' errors (header csum16, dir csum32, all 75 file csum32).
  3. Independent round-trip: fwhelper unpack_lfi -> 70/75 files SHA-256 identical to the
     stock unpack, and exactly the 5 intended files differ and match build/ byte-for-byte.
  4. TOOLS.AP YQHX header + code/data bytes match the .text/.data sections of tzx_ap.elf,
     and the AP's SRAM footprint stays inside 0x00108800..0x0010E800.
  5. Every changed byte lies inside LFI (it is by construction) and the flash-aligned
     4 KB erase blocks touched are listed for the flasher plan.
"""
import hashlib
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FWHELPER = ROOT / "vendor/kilicdev_actions_flash/fwhelper/fwhelper"
STOCK_LFI = ROOT / "firmware-stock/lfi_stock.bin"
STOCK_DIR = ROOT / "dumps/fw_unpacked"
BUILD = ROOT / "build"
NEW_LFI = BUILD / "lfi_tzx.bin"
ELF = BUILD / "tzx_ap.elf"
MODIFIED = {"CONFIG.BIN", "CONFIG.TXT", "MAINMENU.AP", "MAINMENU.STY", "TOOLS.AP"}
LFI_FLASH_BASE = 0x00B400


def fail(msg: str) -> None:
    print(f"[FAIL] {msg}")
    sys.exit(1)


def elf_sections(path: Path) -> dict:
    raw = path.read_bytes()
    e_shoff, = struct.unpack_from("<I", raw, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", raw, 0x2E)
    so = struct.unpack_from("<10I", raw, e_shoff + shstrndx * shentsize)
    strtab = raw[so[4] : so[4] + so[5]]
    out = {}
    for i in range(shnum):
        s = struct.unpack_from("<10I", raw, e_shoff + i * shentsize)
        name = strtab[s[0] : strtab.find(b"\0", s[0])].decode()
        out[name] = (s[3], raw[s[4] : s[4] + s[5]] if s[1] != 8 else b"", s[5])
    return out


def main() -> None:
    stock = STOCK_LFI.read_bytes()
    new = NEW_LFI.read_bytes()

    # 1. Size
    if len(new) != len(stock) or len(new) != 0x1E9B * 512:
        fail(f"size {len(new)} != {len(stock)}")
    print(f"[PASS] Size {len(new):,} B == stock (0x1e9b sectors)")

    # 2. fwhelper scan
    scan = subprocess.run([str(FWHELPER), str(NEW_LFI), "scan_lfi"], capture_output=True, text=True)
    errs = [l for l in (scan.stdout + scan.stderr).splitlines() if "!!!" in l]
    if scan.returncode != 0 or errs:
        fail(f"fwhelper scan_lfi errors: {errs[:5]} rc={scan.returncode}")
    nfiles = sum(1 for l in scan.stdout.splitlines() if l.startswith("0x") and "offset" in l)
    print(f"[PASS] fwhelper scan_lfi: {nfiles} files, 0 checksum errors")

    # 3. Round-trip extraction
    with tempfile.TemporaryDirectory(prefix="lfi_verify_") as td:
        subprocess.run([str(FWHELPER), str(NEW_LFI), "unpack_lfi"], cwd=td, check=True, capture_output=True)
        got = {p.name: p.read_bytes() for p in Path(td).iterdir()}
    ref = {p.name: p.read_bytes() for p in STOCK_DIR.iterdir() if p.is_file()}
    if set(got) != set(ref):
        fail(f"file set mismatch: {set(got) ^ set(ref)}")
    changed = {k for k in ref if ref[k] != got[k]}
    if changed != MODIFIED:
        fail(f"unexpected changed set: {sorted(changed)}")
    for k in MODIFIED:
        if got[k] != (BUILD / k).read_bytes():
            fail(f"{k} differs from build/{k}")
        if len(got[k]) != len(ref[k]):
            fail(f"{k} length changed")
    print(f"[PASS] Round-trip: {len(ref) - len(changed)}/{len(ref)} SHA-256 identical; "
          f"{len(changed)} intended files match build/ byte-for-byte")

    # 4. TOOLS.AP vs ELF
    ap = got["TOOLS.AP"]
    if ap[:8] != b"P\x01\x04\x00yqhx":
        fail("TOOLS.AP bad YQHX magic")
    coff, csz, cva, doff, dsz, dva, bsz, bva, entry = struct.unpack_from("<9I", ap, 8)
    secs = elf_sections(ELF)
    t_va, t_bytes, _ = secs[".text"]
    d_va, d_bytes, _ = secs[".data"]
    b_va, _, b_sz = secs[".bss"]
    if (cva, ap[coff : coff + csz]) != (t_va, t_bytes):
        fail("TOOLS.AP code != ELF .text")
    if (dva, ap[doff : doff + dsz]) != (d_va, d_bytes):
        fail("TOOLS.AP data != ELF .data")
    if (bva, bsz) != (b_va, b_sz):
        fail("TOOLS.AP bss header != ELF .bss")
    lo, hi = cva, bva + bsz
    if lo < 0x00108800 or hi > 0x0010E800:
        fail(f"SRAM footprint 0x{lo:x}..0x{hi:x} outside 0x108800..0x10e800")
    if entry != 0x00108801:
        fail(f"entry 0x{entry:x} != 0x108801")
    print(f"[PASS] TOOLS.AP == tzx_ap.elf (.text {csz:#x} B, .data {dsz:#x} B, .bss {bsz:#x} B); "
          f"SRAM 0x{lo:06x}..0x{hi:06x} (headroom {0x10E800 - hi:,} B)")

    # 5. Flash-aligned erase blocks
    blocks = sorted({(LFI_FLASH_BASE + off) & ~0xFFF for off in range(0, len(new), 512)
                     if new[off : off + 512] != stock[off : off + 512]})
    if not blocks or blocks[0] < 0x00B000 or blocks[-1] >= 0x003DF000:
        fail(f"erase blocks outside guard: {blocks[:1]}..{blocks[-1:]}")
    nsec = sum(1 for off in range(0, len(new), 512) if new[off : off + 512] != stock[off : off + 512])
    print(f"[PASS] {nsec} changed 512 B sectors -> {len(blocks)} flash 4 KB erase blocks "
          f"(0x{blocks[0]:06x}..0x{blocks[-1] + 0xFFF:06x}); MBREC/BREC/VRAM untouched")
    print(f"       sha256(lfi_tzx.bin) = {hashlib.sha256(new).hexdigest()}")
    print("ALL PRE-FLIGHT CONTAINER CHECKS PASSED")


if __name__ == "__main__":
    main()
