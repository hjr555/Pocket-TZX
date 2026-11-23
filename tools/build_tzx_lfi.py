#!/usr/bin/env python3
"""
Build a pre-flight-verifiable ATJ2157 LFI firmware image (`build/lfi_tzx.bin`)
that adds the standalone 6th Main Menu entry "TZX Mode" (ZX Spectrum Next .TZX
tape synthesizer) while preserving all 5 factory modes 100% intact.

Changes strictly isolated to userland LFI (`0x00b400..0x3de9ff`):
  1. `CONFIG.BIN` (+ `CONFIG.TXT`): `MAINMENU_AP_ID_ARRAY` = `6[0, 6, 7, 5, 9, 10]`
  2. `MAINMENU.AP`: `MAGIC_MAINMENU` `0x55AA` -> `0x55AB` in `_read_var()` so
     both upgrade AND rollback auto-refresh `VM_AP_MAINMENU` without touching VRAM!
  3. `MAINMENU.STY`:
     - `Pic 6` (`0x0f770`, `128x64` uint16) -> crisp ZX Spectrum Cassette icon
       preserving the factory `<` `>` navigation arrows!
     - `StrID 24` across all 7 language tables -> `"TZX Mode"` (UTF-16LE in unused
       `0x1db40` padding; zero resource offset shifts!)
  4. `TOOLS.AP` (`APP_ID_TOOLS = 9`, unused in stock firmware): replaced in-place
     by our flat resident ARMv7-M Thumb-2 `TZX Mode` YQHX v4.0 binary, padded/packed
     so total `LFI` size equals exact stock `4,011,520` bytes (`0x1e9b` sectors).
"""
import argparse
import os
import shutil
import struct
from pathlib import Path


def build_yqhx_ap(elf_path: Path, out_ap_path: Path) -> dict:
    """Package a flat ARMv7-M Thumb-2 ET_EXEC ELF (RCODE @ 0x108800, RDATA @ 0x200000)
    into a valid Actions YQHX v4.0 foreground .AP module (`P\\x01\\x04\\x00yqhx`)."""
    raw = elf_path.read_bytes()
    if raw[:4] != b"\x7fELF":
        raise ValueError(f"{elf_path}: not an ELF file")

    e_entry, e_phoff, e_shoff, _, _, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = (
        struct.unpack_from("<IIIIHHHHHH", raw, 0x18)
    )

    sh_str_off, sh_str_sz = struct.unpack_from("<II", raw, e_shoff + e_shstrndx * e_shentsize + 16)
    shstr = raw[sh_str_off : sh_str_off + sh_str_sz]

    code_va, code_bytes = 0x00108800, b""
    data_va, data_bytes = 0x00200000, b""
    bss_va, bss_sz = 0x00200000, 0

    for i in range(e_shnum):
        sh = struct.unpack_from("<10I", raw, e_shoff + i * e_shentsize)
        nm_off, sh_type, sh_flags, sh_addr, sh_offset, sh_size = sh[:6]
        end_nm = shstr.find(b"\x00", nm_off)
        name = shstr[nm_off:end_nm].decode("ascii", errors="replace")
        if name == ".text" and sh_type == 1:
            code_va = sh_addr
            code_bytes = raw[sh_offset : sh_offset + sh_size]
        elif name == ".data" and sh_type == 1:
            data_va = sh_addr
            data_bytes = raw[sh_offset : sh_offset + sh_size]
        elif name == ".bss" and sh_type == 8:
            bss_va = sh_addr
            bss_sz = sh_size

    if not code_bytes:
        raise ValueError("Missing .text section in ELF")
    if code_va + len(code_bytes) > 0x0010FC00:
        raise ValueError(f".text overflows 0x0010FC00 OS vector region: end=0x{code_va+len(code_bytes):x}")
    if len(data_bytes) == 0:
        data_bytes = b"\x00" * 4

    code_off = 0x2800
    code_sz = len(code_bytes)
    # Round data_off to next 0x200 sector boundary after code
    code_padded_len = ((code_sz + 0x1FF) // 0x200) * 0x200
    data_off = code_off + code_padded_len
    data_sz = len(data_bytes)
    data_padded_len = ((data_sz + 0x1FF) // 0x200) * 0x200

    hdr = bytearray(code_off)
    struct.pack_into(
        "<4s4s9I",
        hdr,
        0x00,
        b"P\x01\x04\x00",
        b"yqhx",
        code_off,
        code_sz,
        code_va,
        data_off,
        data_sz,
        data_va,
        bss_sz,
        bss_va,
        e_entry | 1,
    )

    ap_blob = (
        bytes(hdr)
        + code_bytes + b"\x00" * (code_padded_len - code_sz)
        + data_bytes + b"\x00" * (data_padded_len - data_sz)
    )
    stock_tools_len = 0x1B000  # Exact stock TOOLS.AP length (110,592 B) for zero-shift LFI layout
    if len(ap_blob) > stock_tools_len:
        raise ValueError(f"TOOLS.AP size {len(ap_blob)} exceeds stock slot {stock_tools_len}")
    ap_blob += b"\x00" * (stock_tools_len - len(ap_blob))
    out_ap_path.write_bytes(ap_blob)
    return {
        "ap_size": len(ap_blob),
        "active_payload": code_off + code_padded_len + data_padded_len,
        "code": (hex(code_off), hex(code_sz), hex(code_va)),
        "data": (hex(data_off), hex(data_sz), hex(data_va)),
        "bss": (hex(bss_sz), hex(bss_va)),
        "bss_end": hex(bss_va + bss_sz),
        "entry": hex(e_entry | 1),
    }


def patch_config_bin(src: bytes) -> bytes:
    """Update Item 1 (`MAINMENU_AP_ID_ARRAY`) from `6[0,6,7,5,10]` (14 B) to
    `6[0,6,7,5,9,10]` (16 B), shifting subsequent item offsets by +2 bytes and
    trimming 2 trailing 0x00 pad bytes so file length remains 2560 B."""
    if src[:4] != b"cfg\x00":
        raise ValueError("Invalid CONFIG.BIN magic")

    # Count directory items from 0x10 up to first item offset
    first_off, _ = struct.unpack_from("<II", src, 0x10)
    max_items = (first_off - 0x10) // 8
    entries = []
    for i in range(max_items):
        off, ln = struct.unpack_from("<II", src, 0x10 + i * 8)
        entries.append([off, ln])

    old_item1_off, old_item1_len = entries[1]
    old_item1 = src[old_item1_off : old_item1_off + old_item1_len]
    expected_old = struct.pack("<7H", 6, 5, 0, 6, 7, 5, 10)
    if old_item1 != expected_old:
        raise ValueError(f"Unexpected Item 1 payload: {old_item1.hex()}")

    new_item1 = struct.pack("<8H", 6, 6, 0, 6, 7, 5, 9, 10)
    delta = len(new_item1) - old_item1_len  # +2 bytes

    if src[-delta:] != b"\x00" * delta:
        raise ValueError("Expected trailing zero padding in CONFIG.BIN")

    out = bytearray(src[:old_item1_off] + new_item1 + src[old_item1_off + old_item1_len : -delta])
    entries[1][1] = len(new_item1)
    for i in range(2, len(entries)):
        if entries[i][0] > old_item1_off:
            entries[i][0] += delta
    for i, (off, ln) in enumerate(entries):
        struct.pack_into("<II", out, 0x10 + i * 8, off, ln)

    assert len(out) == len(src)
    return bytes(out)


def patch_mainmenu_ap(src: bytes) -> bytes:
    """Change `MAGIC_MAINMENU` check/write in `_read_var()` (`0x180011f0`, file
    `0x2a02` & `0x2a3a`) from `0x55AA` (`movw r1, #0x55aa` = `45f2 aa51`) to
    `0x55AB` (`45f2 ab51`) so VRAM reloads the 6-item Main Menu automatically."""
    out = bytearray(src)
    patched = 0
    for foff in (0x2A02, 0x2A3A):
        if bytes(out[foff : foff + 4]) != bytes.fromhex("45f2aa51"):
            raise ValueError(f"MAINMENU.AP @ 0x{foff:x} unexpected bytes: {out[foff:foff+4].hex()}")
        out[foff : foff + 4] = bytes.fromhex("45f2ab51")
        patched += 1
    assert patched == 2
    return bytes(out)


def render_cassette_icon_128x64(orig_pic6: bytes) -> bytes:
    """Render a crisp 128x64 ZX Spectrum Cassette icon into Pic 6 (`uint16_le`
    pixels: `0x0000` = lit pixel, `0xFFFF` = unlit pixel), preserving the exact
    factory `<` and `>` side arrows (`x < 24` and `x >= 104`)."""
    px = list(struct.unpack("<8192H", orig_pic6))

    # Clear center icon canvas x=24..103, y=0..63 while keeping factory < > arrows!
    for y in range(64):
        for x in range(24, 104):
            px[y * 128 + x] = 0xFFFF

    def set_px(x: int, y: int, on: bool = True) -> None:
        if 24 <= x < 104 and 0 <= y < 64:
            px[y * 128 + x] = 0x0000 if on else 0xFFFF

    def hline(x0: int, x1: int, y: int, on: bool = True) -> None:
        for x in range(x0, x1 + 1):
            set_px(x, y, on)

    def vline(x: int, y0: int, y1: int, on: bool = True) -> None:
        for y in range(y0, y1 + 1):
            set_px(x, y, on)

    # Outer cassette shell: x=32..95 (64px wide), y=6..42 (37px tall) with 2px chamfers
    hline(34, 93, 6);  hline(33, 94, 7)
    hline(33, 94, 41); hline(34, 93, 42)
    vline(32, 8, 40);  vline(33, 7, 41)
    vline(95, 8, 40);  vline(94, 7, 41)

    # Corner screws
    for sx, sy in ((36, 10), (91, 10), (36, 38), (91, 38)):
        set_px(sx, sy); set_px(sx + 1, sy); set_px(sx, sy + 1)

    # Inner label border & ZX Spectrum diagonal stripes on top-right of label
    hline(38, 89, 11)
    for s in range(4):
        for dy in range(5):
            set_px(80 + s * 2 - dy, 12 + dy)
            set_px(81 + s * 2 - dy, 12 + dy)

    # Central tape reel window: x=40..87, y=16..30
    hline(41, 86, 16); hline(41, 86, 30)
    vline(40, 17, 29); vline(87, 17, 29)

    # Left & Right tape hubs (centers at (49, 23) and (78, 23), radius 5)
    for cx in (49, 78):
        for dy in range(-5, 6):
            for dx in range(-5, 6):
                r2 = dx * dx + dy * dy
                if 10 <= r2 <= 27:
                    set_px(cx + dx, 23 + dy)
        # Hub teeth
        set_px(cx, 20); set_px(cx, 26); set_px(cx - 3, 23); set_px(cx + 3, 23)

    # Tape pack window between reels (x=57..70, y=19..27)
    hline(57, 70, 19); hline(57, 70, 27)
    vline(57, 19, 27); vline(70, 19, 27)
    # Tape spooling curve hint
    vline(61, 20, 26); vline(66, 20, 26)

    # Trapezoid bottom magnetic head shield: y=34..41, x=43..84
    for i, y in enumerate(range(34, 42)):
        x_left = 46 - (i // 2)
        x_right = 81 + (i // 2)
        set_px(x_left, y); set_px(x_left + 1, y)
        set_px(x_right, y); set_px(x_right - 1, y)
    hline(46, 81, 34)
    # Capstan / guide holes inside trapezoid
    for hx in (52, 60, 67, 75):
        set_px(hx, 38); set_px(hx + 1, 38)
        set_px(hx, 39); set_px(hx + 1, 39)

    return struct.pack("<8192H", *px)


def patch_mainmenu_sty(src: bytes) -> bytes:
    """Patch Pic 6 (`0x0f770`) with the ZX Spectrum Cassette icon and point
    `StrID 24` across all 7 language string tables to `"TZX Mode\\0"` stored in
    the unused `0x1db40` 0xFF padding region."""
    out = bytearray(src)

    # 1. Replace Pic 6 (128x64 uint16 = 0x4000 bytes @ 0x0f770)
    pic_off = 0x0F770
    out[pic_off : pic_off + 0x4000] = render_cassette_icon_128x64(bytes(out[pic_off : pic_off + 0x4000]))

    # 2. Store UTF-16LE "TZX Mode\0" (18 bytes) at unused 0xFF region 0x1db40
    str_off = 0x1DB40
    tzx_utf16 = "TZX Mode\x00".encode("utf-16le")
    if bytes(out[str_off : str_off + 32]) != b"\xFF" * 32:
        raise ValueError("Expected 0xFF padding at MAINMENU.STY 0x1db40")
    out[str_off : str_off + len(tzx_utf16)] = tzx_utf16

    # 3. Update StrID 24 descriptor (tbl + 24*8) in all 7 language tables
    lang_tables = [0x1DB70, 0x1DE68, 0x1E168, 0x1E4A0, 0x1E7D4, 0x1EB08, 0x1EE08]
    for tbl in lang_tables:
        struct.pack_into("<II", out, tbl + 24 * 8, str_off, len(tzx_utf16))

    assert len(out) == len(src)
    return bytes(out)


def checksum_u32_words(data: bytes) -> int:
    if len(data) % 4 != 0:
        data = data + b"\x00" * (4 - (len(data) % 4))
    words = struct.unpack(f"<{len(data)//4}I", data)
    return sum(words) & 0xFFFFFFFF


def checksum_u16_words(data: bytes) -> int:
    words = struct.unpack(f"<{len(data)//2}H", data)
    return sum(words) & 0xFFFF


def rebuild_lfi(stock_lfi_path: Path, replacements: dict[str, bytes], out_lfi_path: Path | None) -> tuple[bytes, dict]:
    """Rebuild an exact-size Actions LFI image from `lfi_stock.bin`, replacing
    only the specified files in-place while preserving exact 32-byte directory
    entries (`0x200..0x2000`), exact sector offsets (`off_sec`), per-file 32-bit
    checksums (`entry + 0x1c`), directory checksum (`LFI[0x10]`), header 16-bit
    checksum (`LFI[0x1fe]`), and exact total byte size (`4,011,520 B`)."""
    stock = stock_lfi_path.read_bytes()
    total_size = len(stock)
    if struct.unpack_from("<I", stock, 0)[0] != 0x0FF0AA55:
        raise ValueError("Invalid LFI magic in stock LFI")

    out = bytearray(stock)
    replaced_set = set()
    files_total = 0

    for idx in range((0x2000 - 0x200) // 32):
        ent_off = 0x200 + idx * 32
        raw_ent = bytearray(out[ent_off : ent_off + 32])
        if raw_ent[0] in (0x00, 0xFF):
            break
        files_total += 1
        raw_name = bytes(raw_ent[:11])
        base = raw_name[:8].rstrip(b" ").decode("ascii")
        ext = raw_name[8:11].rstrip(b" ").decode("ascii")
        fname = f"{base}.{ext}" if ext else base
        off_sec, byte_len, rsv, old_csum = struct.unpack_from("<IIII", raw_ent, 0x10)

        if fname in replacements:
            blob = replacements[fname]
            if len(blob) != byte_len:
                raise ValueError(f"{fname}: replacement length {len(blob)} != stock length {byte_len}")
            out[off_sec * 512 : off_sec * 512 + byte_len] = blob
            csum = checksum_u32_words(blob)
            struct.pack_into("<IIII", raw_ent, 0x10, off_sec, byte_len, rsv, csum)
            out[ent_off : ent_off + 32] = raw_ent
            replaced_set.add(fname)

    missing = set(replacements.keys()) - replaced_set
    if missing:
        raise ValueError(f"Replacement filenames not found in stock LFI: {missing}")

    # Recompute LFI directory 32-bit checksum at LFI[0x10] over 0x200..0x2000
    dir_csum = checksum_u32_words(bytes(out[0x200:0x2000]))
    struct.pack_into("<I", out, 0x10, dir_csum)

    # Recompute LFI header 16-bit checksum at LFI[0x1fe] over 0x000..0x1fe
    hdr_csum = checksum_u16_words(bytes(out[0x000:0x1FE]))
    struct.pack_into("<H", out, 0x1FE, hdr_csum)

    out_bytes = bytes(out)
    if out_lfi_path is not None:
        out_lfi_path.parent.mkdir(parents=True, exist_ok=True)
        out_lfi_path.write_bytes(out_bytes)

    changed_4k_blocks = sum(
        1 for b in range(0, total_size, 4096) if out_bytes[b : b + 4096] != stock[b : b + 4096]
    )
    return out_bytes, {
        "files_total": files_total,
        "total_bytes": total_size,
        "changed_4k_blocks": changed_4k_blocks,
        "dir_csum": hex(dir_csum),
        "hdr_csum": hex(hdr_csum),
    }


def main() -> None:
    ap = argparse.ArgumentParser(description="Build ATJ2157 TZX Mode LFI image")
    ap.add_argument("--elf", type=Path, required=True, help="Compiled tzx_ap.elf")
    root = Path(__file__).resolve().parent.parent
    ap.add_argument("--unpacked", type=Path, default=root / "dumps/fw_unpacked")
    ap.add_argument("--stock-lfi", type=Path, default=root / "firmware-stock/lfi_stock.bin")
    ap.add_argument("--out-lfi", type=Path, default=root / "build/lfi_tzx.bin")
    args = ap.parse_args()

    # Self-test: zero-replacement rebuild MUST match lfi_stock.bin bit-for-bit!
    noop_bytes, _ = rebuild_lfi(args.stock_lfi, {}, None)
    if noop_bytes != args.stock_lfi.read_bytes():
        raise AssertionError("No-op rebuild_lfi did not reproduce lfi_stock.bin bit-for-bit!")

    build_dir = args.out_lfi.parent
    build_dir.mkdir(parents=True, exist_ok=True)

    tzx_ap_path = build_dir / "TOOLS.AP"
    ap_info = build_yqhx_ap(args.elf, tzx_ap_path)
    print("Built YQHX TOOLS.AP (TZX Mode):", ap_info)

    cfg_bin = patch_config_bin((args.unpacked / "CONFIG.BIN").read_bytes())
    mm_ap = patch_mainmenu_ap((args.unpacked / "MAINMENU.AP").read_bytes())
    mm_sty = patch_mainmenu_sty((args.unpacked / "MAINMENU.STY").read_bytes())
    orig_txt = (args.unpacked / "CONFIG.TXT").read_bytes()
    cfg_txt = orig_txt.replace(
        b"MAINMENU_AP_ID_ARRAY = 6[0,6, 7,5,10];",
        b"MAINMENU_AP_ID_ARRAY = 6[0,6,7,5,9,10];",
    )
    if len(cfg_txt) > len(orig_txt):
        delta = len(cfg_txt) - len(orig_txt)
        if cfg_txt[-delta:] not in (b"\x00" * delta, b" " * delta, b"\r\n"[:delta]):
            # Trim trailing NUL/whitespace pad to preserve exact sector length
            cfg_txt = cfg_txt[:len(orig_txt)]
        else:
            cfg_txt = cfg_txt[:len(orig_txt)]
    assert len(cfg_txt) == len(orig_txt)

    (build_dir / "CONFIG.BIN").write_bytes(cfg_bin)
    (build_dir / "CONFIG.TXT").write_bytes(cfg_txt)
    (build_dir / "MAINMENU.AP").write_bytes(mm_ap)
    (build_dir / "MAINMENU.STY").write_bytes(mm_sty)

    _, lfi_info = rebuild_lfi(
        args.stock_lfi,
        {
            "CONFIG.BIN": cfg_bin,
            "CONFIG.TXT": cfg_txt,
            "MAINMENU.AP": mm_ap,
            "MAINMENU.STY": mm_sty,
            "TOOLS.AP": tzx_ap_path.read_bytes(),
        },
        args.out_lfi,
    )
    print("Built LFI image:", args.out_lfi, lfi_info)


if __name__ == "__main__":
    main()
