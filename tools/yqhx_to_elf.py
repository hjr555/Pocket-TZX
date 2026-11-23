#!/usr/bin/env python3
"""
Convert Actions Semiconductor YQHX (.AP / .DRV / .AL) firmware modules into
ARM Thumb-2 (ATJ2157) or MIPS32/MIPS16 (ATJ2127) ET_EXEC ELF images at exact
runtime virtual addresses for REA MCP / Ghidra 12.1.4 decompilation.

Auto-detects architecture from YQHX runtime base address:
  - code_va <  0x80000000 -> ATJ2157 ARMv7-M / Cortex-M (EM_ARM = 40, Thumb entry | 1)
  - code_va >= 0x80000000 -> ATJ2127 MIPS32/MIPS16e     (EM_MIPS = 8, kseg0/kseg1)
"""
import argparse
import struct
from pathlib import Path


def yqhx_to_elf(src_path: Path, dst_path: Path, arch: str = "auto") -> dict:
    raw = src_path.read_bytes()
    if len(raw) < 0x40 or raw[4:8] != b"yqhx":
        raise ValueError(f"{src_path}: missing 'yqhx' magic at +0x04")

    mod_type = raw[9]
    group = raw[10] & 0x3F
    ver_maj, ver_min = raw[8] >> 4, raw[8] & 0x0F

    # YQHX v4.0 header layout (verified against fwhelper & live ATJ2157 LFI):
    # +0x0c: code_off, +0x10: code_sz, +0x14: code_va
    # +0x18: data_off, +0x1c: data_sz, +0x20: data_va
    # +0x24: bss_sz,   +0x28: bss_va
    # +0x2c: init_va,  +0x30: exit_va, +0x34: export_va
    # Note: older v3.0 MIPS modules start code_off at +0x08; detect cleanly:
    c_off_v4, c_sz_v4, c_va_v4 = struct.unpack_from("<III", raw, 0x0C)
    c_off_v3, c_sz_v3, c_va_v3 = struct.unpack_from("<III", raw, 0x08)
    if c_sz_v4 > 0 and c_off_v4 >= 0x200 and c_off_v4 + c_sz_v4 <= len(raw) and (c_va_v4 >= 0x00100000):
        code_off, code_sz, code_va = c_off_v4, c_sz_v4, c_va_v4
        data_off, data_sz, data_va = struct.unpack_from("<III", raw, 0x18)
        bss_sz, bss_va = struct.unpack_from("<II", raw, 0x24)
        init_va, exit_va, export_va = struct.unpack_from("<III", raw, 0x2C)
        hdr_variant = "v4"
    else:
        code_off, code_sz, code_va = c_off_v3, c_sz_v3, c_va_v3
        data_off, data_sz, data_va = struct.unpack_from("<III", raw, 0x14)
        bss_sz, bss_va = struct.unpack_from("<II", raw, 0x20)
        init_va, exit_va = struct.unpack_from("<II", raw, 0x28)
        _, _, export_va = struct.unpack_from("<III", raw, 0x30)
        hdr_variant = "v3"

    if arch == "auto":
        is_arm = code_va < 0x80000000
    else:
        is_arm = arch.lower() == "arm"

    segments = []
    if code_sz and code_off + code_sz <= len(raw):
        segments.append((".text", code_va, raw[code_off : code_off + code_sz], code_sz, 5))  # PF_R | PF_X
    if data_sz and data_off + data_sz <= len(raw):
        segments.append((".data", data_va, raw[data_off : data_off + data_sz], data_sz, 6))  # PF_R | PF_W
    if bss_sz and bss_va:
        segments.append((".bss", bss_va, b"\x00" * bss_sz, bss_sz, 6))

    ehdr_sz = 52
    phdr_sz = 32
    shent_sz = 40
    phnum = len(segments)

    shnames = [b""] + [s[0].encode() for s in segments] + [b".shstrtab"]
    shstrtab = b"\x00".join(shnames) + b"\x00"
    shstr_padded = shstrtab + b"\x00" * ((4 - len(shstrtab) % 4) % 4)

    cur_off = 0x1000
    seg_offs = []
    blob_parts = []
    for _, _, fbytes, _, _ in segments:
        seg_offs.append(cur_off)
        pad = (0x1000 - (len(fbytes) % 0x1000)) % 0x1000
        blob_parts.append(fbytes + b"\x00" * pad)
        cur_off += len(fbytes) + pad

    str_off = cur_off
    sh_off = str_off + len(shstr_padded)

    if is_arm:
        e_machine = 40          # EM_ARM
        e_flags = 0x05000000    # EF_ARM_EABI_VER5
        if code_va <= (init_va & ~1) < code_va + code_sz:
            entry = init_va | 1
        elif code_va <= (export_va & ~1) < code_va + code_sz:
            entry = export_va | 1
        else:
            entry = code_va | 1
    else:
        e_machine = 8           # EM_MIPS
        e_flags = 0x50000000    # EF_MIPS_ARCH_32
        entry = init_va if (init_va >> 28) in (0x8, 0x9, 0xB) else code_va

    e_ident = b"\x7fELF\x01\x01\x01\x00" + b"\x00" * 8
    ehdr = struct.pack(
        "<16sHHIIIIIHHHHHH",
        e_ident,
        2,             # ET_EXEC
        e_machine,
        1,             # EV_CURRENT
        entry,
        ehdr_sz,
        sh_off,
        e_flags,
        ehdr_sz,
        phdr_sz,
        phnum,
        shent_sz,
        len(shnames),
        len(shnames) - 1,
    )

    phdrs = b"".join(
        struct.pack(
            "<IIIIIIII",
            1,
            seg_offs[i],
            segments[i][1],
            segments[i][1],
            len(segments[i][2]),
            segments[i][3],
            segments[i][4],
            0x1000,
        )
        for i in range(phnum)
    )
    pad0 = b"\x00" * (0x1000 - len(ehdr) - len(phdrs))

    shdrs = [struct.pack("<IIIIIIIIII", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    name_pos = 1
    for i, (nm, vaddr, fbytes, _, pflags) in enumerate(segments):
        sflags = 2 | (4 if (pflags & 1) else 0) | (1 if (pflags & 2) else 0)
        shdrs.append(struct.pack("<IIIIIIIIII", name_pos, 1, sflags, vaddr, seg_offs[i], len(fbytes), 0, 0, 4, 0))
        name_pos += len(nm) + 1
    shdrs.append(struct.pack("<IIIIIIIIII", name_pos, 3, 0, 0, str_off, len(shstrtab), 0, 0, 1, 0))

    dst_path.write_bytes(ehdr + phdrs + pad0 + b"".join(blob_parts) + shstr_padded + b"".join(shdrs))
    return {
        "arch": "arm" if is_arm else "mips",
        "hdr_variant": hdr_variant,
        "type": chr(mod_type) if 32 <= mod_type < 127 else f"0x{mod_type:02x}",
        "version": f"{ver_maj}.{ver_min}",
        "group": group,
        "code": (hex(code_off), hex(code_sz), hex(code_va)),
        "data": (hex(data_off), hex(data_sz), hex(data_va)),
        "bss": (hex(bss_sz), hex(bss_va)),
        "init": hex(init_va),
        "export": hex(export_va),
        "entry": hex(entry),
    }


def main() -> None:
    ap = argparse.ArgumentParser(description="Convert Actions YQHX module to ARM/MIPS ELF")
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--arch", choices=["auto", "arm", "mips"], default="auto")
    args = ap.parse_args()
    print(yqhx_to_elf(args.input, args.output, args.arch))


if __name__ == "__main__":
    main()
