#!/usr/bin/env python3
"""
Safe Read-Only ADFU Dumper for Actions Semiconductor Voice Recorders (10d6:1101 -> 10d6:10d6).

Supports ATJ2157 (ARM Cortex-M, ID 0x3051) and ATJ2127 (MIPS32/MIPS16, ID 0x10d6):
  1. Captures USB descriptors & MSC vendor signatures if starting from 10d6:1101,
     then triggers adfu_reboot (0xCC + 0xCB 0x21) to enter Boot ROM ADFU (10d6:10d6).
  2. Queries Boot ROM adfu_info (0xCC, 18 bytes) -> identifies ATJ2157 vs ATJ2127.
  3. Dumps raw untouched 256 KB SRAM via native Boot ROM CMD_ADFU_READRAM (0x93).
  4. On ATJ2157:
     - Switches to adfus.bin @ 0x00118000 and dumps 64 KB ARM Mask Boot ROM @ 0x00000000.
     - Executes read-only spiread_atj2157.bin @ 0x0011e000 to capture JEDEC/SFDP flash metadata,
       clocked MMIO snapshot, full 4 MiB raw physical SPI NOR (spi_nor_raw_4m.bin), and
       full 4 MiB hardware-descrambled SPI NOR (spi_nor_decrypted_4m.bin), carving
       mbrec_512b.bin, brec_45k.bin, lfi_stock.bin, and vram_nvram_tail_133k.bin.
     - Clears the persistent SRAM ADFU flag (0x00130000 = 0xadf0adf0 -> 0) and triggers
       a clean FWSC=1 + 0xB0 hardware Watchdog reboot out of ADFU back to normal mode.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import struct
import sys
import time

USBC_SIG = 0x43425355
USBS_SIG = 0x53425355

CMD_ADFU_WRITERAM = 0x13
CMD_ADFU_READRAM = 0x93
CMD_ADFU_SWITCH = 0x20
CMD_ADFU_EXEC = 0x21
CMD_ADFU_READRET = 0x23


class LibUSB:
    def __init__(self) -> None:
        self.lib = ctypes.CDLL("libusb-1.0.so.0")
        self.lib.libusb_init.argtypes = [ctypes.c_void_p]
        self.lib.libusb_exit.argtypes = [ctypes.c_void_p]
        self.lib.libusb_open_device_with_vid_pid.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint16]
        self.lib.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p
        self.lib.libusb_kernel_driver_active.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.libusb_detach_kernel_driver.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.libusb_claim_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.libusb_release_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.libusb_bulk_transfer.argtypes = [
            ctypes.c_void_p, ctypes.c_uint8, ctypes.c_void_p,
            ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.c_uint,
        ]
        self.lib.libusb_clear_halt.argtypes = [ctypes.c_void_p, ctypes.c_uint8]
        self.lib.libusb_close.argtypes = [ctypes.c_void_p]
        if self.lib.libusb_init(None) < 0:
            raise RuntimeError("libusb_init failed")

    def open_vid_pid(self, vid: int, pid: int, wait_s: float = 0.0) -> int | None:
        deadline = time.monotonic() + wait_s
        while True:
            h = self.lib.libusb_open_device_with_vid_pid(None, vid, pid)
            if h:
                if self.lib.libusb_kernel_driver_active(h, 0) == 1:
                    self.lib.libusb_detach_kernel_driver(h, 0)
                rc = self.lib.libusb_claim_interface(h, 0)
                if rc < 0:
                    self.lib.libusb_close(h)
                    raise RuntimeError(f"libusb_claim_interface failed: {rc}")
                return h
            if time.monotonic() >= deadline:
                return None
            time.sleep(0.05)

    def close(self, h: int) -> None:
        if h:
            self.lib.libusb_release_interface(h, 0)
            self.lib.libusb_close(h)

    def bulk_out(self, h: int, ep: int, data: bytes, timeout_ms: int = 4000) -> None:
        transferred = ctypes.c_int(0)
        rc = self.lib.libusb_bulk_transfer(h, ep, data, len(data), ctypes.byref(transferred), timeout_ms)
        if rc != 0 or transferred.value != len(data):
            raise RuntimeError(f"bulk_out(ep=0x{ep:02x}, len={len(data)}) failed: rc={rc}, xfer={transferred.value}")

    def bulk_in(self, h: int, ep: int, length: int, timeout_ms: int = 4000) -> bytes:
        buf = ctypes.create_string_buffer(max(length, 512))
        out = bytearray()
        while len(out) < length:
            rem = length - len(out)
            transferred = ctypes.c_int(0)
            rc = self.lib.libusb_bulk_transfer(h, ep, buf, rem, ctypes.byref(transferred), timeout_ms)
            if rc != 0:
                self.lib.libusb_clear_halt(h, ep)
                raise RuntimeError(f"bulk_in(ep=0x{ep:02x}, rem={rem}) failed: rc={rc}")
            chunk = buf.raw[: transferred.value]
            if not chunk:
                break
            out.extend(chunk)
        return bytes(out)


def msc_adfu_reboot(usb: LibUSB, h: int, out_dir: Path) -> None:
    """Capture MSC vendor info and trigger 0xCC + 0xCB 0x21 reboot to ADFU (10d6:10d6)."""
    ep_out, ep_in = 0x02, 0x81
    cbw20 = struct.pack("<IIIBBB16s", USBC_SIG, 1, 20, 0x80, 0, 16, bytes([0xCC, 0, 0, 0, 0, 0, 0, 20]).ljust(16, b"\x00"))
    usb.bulk_out(h, ep_out, cbw20)
    sol20 = usb.bulk_in(h, ep_in, 20)
    usb.bulk_in(h, ep_in, 13)
    (out_dir / "msc_isactions_20b.bin").write_bytes(sol20)

    cbw11 = struct.pack("<IIIBBB16s", USBC_SIG, 2, 11, 0x80, 0, 16, bytes([0xCC, 0, 0, 0, 0, 0, 0, 11]).ljust(16, b"\x00"))
    usb.bulk_out(h, ep_out, cbw11)
    sig11 = usb.bulk_in(h, ep_in, 11)
    usb.bulk_in(h, ep_in, 13)
    if sig11 != b"ACTIONSUSBD":
        raise RuntimeError(f"Unexpected 0xCC(11) signature: {sig11!r}")

    cbw_sw = struct.pack("<IIIBBB16s", USBC_SIG, 3, 2, 0x80, 0, 16, bytes([0xCB, 0x21, 0, 0, 0, 0, 0, 2]).ljust(16, b"\x00"))
    usb.bulk_out(h, ep_out, cbw_sw)
    ack = usb.bulk_in(h, ep_in, 2)
    usb.bulk_in(h, ep_in, 13)
    print(f"[MSC] SwitchToAdfu ack={ack.hex()} -> waiting for 10d6:10d6...")


class AdfuClient:
    def __init__(self, usb: LibUSB, h: int) -> None:
        self.usb = usb
        self.h = h
        self.ep_in = 0x81
        self.ep_out = 0x02

    def check_csw(self, csw: bytes, expected_tag: int = 0) -> None:
        if len(csw) != 13:
            raise RuntimeError(f"Bad CSW length: {len(csw)}")
        sig, rtag, res, st = struct.unpack("<IIIB", csw)
        if sig != USBS_SIG or st != 0:
            raise RuntimeError(f"Bad CSW: sig=0x{sig:08x} tag=0x{rtag:08x} res={res} status={st}")

    def adfu_info(self) -> tuple[bytes, str]:
        cdb = bytearray(16)
        cdb[0] = 0xCC
        cdb[7] = 0x12
        cbw = struct.pack("<IIIBBB16s", USBC_SIG, 0, 0x12, 0x80, 0, 16, bytes(cdb))
        for cand_out in (0x02, 0x01):
            try:
                self.usb.bulk_out(self.h, cand_out, cbw)
                self.ep_out = cand_out
                break
            except RuntimeError:
                continue
        for cand_in in (0x81, 0x82):
            try:
                resp = self.usb.bulk_in(self.h, cand_in, 0x12)
                self.ep_in = cand_in
                break
            except RuntimeError:
                continue
        csw = self.usb.bulk_in(self.h, self.ep_in, 13)
        self.check_csw(csw, 0)
        chip = "UNKNOWN"
        if resp[:7] == b"\x00CADFUD":
            cid = (resp[7] << 8) | resp[8]
            chip = {0x10D6: "ATJ2127", 0x3051: "ATJ2157"}.get(cid, f"ID_0x{cid:04x}")
        return resp, chip

    def cmd_0xcd(self, subcmd: int, length: int, addr: int, recv: bool, data_len: int) -> None:
        cdb = struct.pack("<BIII3s", 0xCD, subcmd, length, addr, b"\x00\x00\x00")
        cbw = struct.pack("<IIIBBB16s", USBC_SIG, 0, data_len, 0x80 if recv else 0x00, 0, 16, cdb)
        self.usb.bulk_out(self.h, self.ep_out, cbw)

    def read_ram_native(self, addr: int, size: int, step: int = 0x200) -> bytes:
        out = bytearray()
        off = 0
        while off < size:
            n = min(step, size - off)
            self.cmd_0xcd(CMD_ADFU_READRAM, n, (addr + off) & 0xFFFFFFFF, True, n)
            pkt = self.usb.bulk_in(self.h, self.ep_in, n)
            csw = self.usb.bulk_in(self.h, self.ep_in, 13)
            self.check_csw(csw, 0)
            out.extend(pkt)
            off += n
        return bytes(out)

    def write_ram(self, addr: int, data: bytes, step: int = 0x200) -> None:
        off = 0
        while off < len(data):
            chunk = data[off : off + step]
            n = len(chunk)
            self.cmd_0xcd(CMD_ADFU_WRITERAM, n, (addr + off) & 0xFFFFFFFF, False, n)
            self.usb.bulk_out(self.h, self.ep_out, chunk)
            csw = self.usb.bulk_in(self.h, self.ep_in, 13)
            self.check_csw(csw, 0)
            off += n

    def exec_ram(self, addr: int, timeout_ms: int = 8000) -> None:
        self.cmd_0xcd(CMD_ADFU_EXEC, 0, addr & 0xFFFFFFFF, False, 0)
        csw = self.usb.bulk_in(self.h, self.ep_in, 13, timeout_ms=timeout_ms)
        self.check_csw(csw, 0)

    def switch_adfus_arm(self, adfus_bin: bytes) -> None:
        self.write_ram(0x00118500, b"\x00" * 0x200, step=0x200)
        self.write_ram(0x00118000, adfus_bin, step=0x200)
        self.cmd_0xcd(CMD_ADFU_SWITCH, 0, 0x00118000, False, 0)
        csw = self.usb.bulk_in(self.h, self.ep_in, 13)
        self.check_csw(csw, 0)
        time.sleep(0.05)

    def dump_mem_unmasked(self, chip: str, addr: int, size: int, word_aligned: bool = False) -> bytes:
        if chip == "ATJ2157":
            code_addr = 0x0011E000 | 1
            buf_addr = 0x0011E020
            stub = struct.pack(
                "<12H",
                0x4805, 0x0003, 0xC806, 0x6018,
                0xF811, 0x3B01, 0x3A01, 0xF800, 0x3B01, 0xD1F9,
                0x4800, 0x4770,
            ) + struct.pack("<I", buf_addr)
        else:
            if word_aligned:
                code_addr = 0xBFC1E000
                buf_addr = 0xBFC1E040
                stub = struct.pack(
                    "<13I",
                    0x3C08BFC1, 0x3508E040, 0x8D040000, 0x8D050004,
                    0x25060008, 0x00852821, 0x8C830000, 0x24840004,
                    0xACC30000, 0x1485FFFC, 0x24C60004, 0x03E00008,
                    0x35020000,
                )
            else:
                code_addr = 0xBFC1E000 | 1
                buf_addr = 0xBFC1E020
                stub = struct.pack(
                    "<14H",
                    0xB207, 0x9A80, 0x9AA1, 0xF000, 0x42C8, 0xE595, 0xDAC0,
                    0xA460, 0x4C01, 0xC660, 0x4E01, 0xECAA, 0x61FA, 0xE8A0,
                ) + struct.pack("<I", buf_addr)

        self.write_ram(code_addr & ~1, stub)
        step = 0x1D0
        out = bytearray()
        off = 0
        while off < size:
            n = min(step, size - off)
            self.write_ram(buf_addr, struct.pack("<II", (addr + off) & 0xFFFFFFFF, n))
            self.exec_ram(code_addr)
            chunk = self.read_ram_native(buf_addr + 8, n)
            out.extend(chunk)
            off += n
        return bytes(out)

    def reboot_atj2157_clean(self, spiread_bin: bytes) -> None:
        """
        Clear SRAM[0x00130000] == 0xadf0adf0 persistent ADFU magic (checked by BREC @ 0x10f900),
        arm FWSC=1 in adfus.bin via CMD 4 + 0x23, and trigger 0xB0 hardware Watchdog reset.
        """
        self.write_ram(0x00130000, b"\x00" * 16, step=16)
        self.write_ram(0x0011E000, spiread_bin, step=0x400)
        self.write_ram(0x0011FFE0, struct.pack("<8I", 0, 0, 0, 0, 4, 0, 0, 0), step=32)
        self.exec_ram(0x0011E001, timeout_ms=4000)

        self.cmd_0xcd(CMD_ADFU_READRET, 16, 0, True, 16)
        self.usb.bulk_in(self.h, self.ep_in, 16)
        self.check_csw(self.usb.bulk_in(self.h, self.ep_in, 13), 0)

        cbw_rst = struct.pack("<IIIBBB16s", USBC_SIG, 0, 0, 0x00, 0, 16, bytes([0xB0]) + b"\x00" * 15)
        self.usb.bulk_out(self.h, self.ep_out, cbw_rst)
        self.check_csw(self.usb.bulk_in(self.h, self.ep_in, 13, timeout_ms=2000), 0)


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=root / "firmware-stock")
    ap.add_argument("--adfus-arm", type=Path, default=root / "vendor/kilicdev_actions_flash/payload_arm/adfus.bin")
    ap.add_argument("--spiread-arm", type=Path, default=root / "dumps/spiread_atj2157.bin")
    ap.add_argument("--stay-in-adfu", action="store_true", help="Stay in 10d6:10d6 after dumping")
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    usb = LibUSB()

    h_adfu = usb.open_vid_pid(0x10D6, 0x10D6, wait_s=0.1)
    if not h_adfu:
        h_msc = usb.open_vid_pid(0x10D6, 0x1101, wait_s=0.5)
        if not h_msc:
            sys.exit("Neither 10d6:1101 (MSC) nor 10d6:10d6 (ADFU) is currently connected on USB.")
        try:
            msc_adfu_reboot(usb, h_msc, args.out)
        finally:
            usb.close(h_msc)
        h_adfu = usb.open_vid_pid(0x10D6, 0x10D6, wait_s=15.0)
        if not h_adfu:
            sys.exit("Timed out waiting for 10d6:10d6 ADFU enumeration.")

    try:
        client = AdfuClient(usb, h_adfu)
        info_bytes, chip = client.adfu_info()
        (args.out / "adfu_info.bin").write_bytes(info_bytes)
        print(f"[ADFU] Connected! adfu_info={info_bytes!r} ({info_bytes.hex()}) -> chip={chip}")

        if chip == "ATJ2157":
            print("[ADFU] Stage 1: Dumping untouched 256 KB SRAM (0x00100000) via native Boot ROM...")
            sram_raw = client.read_ram_native(0x00100000, 256 * 1024)
            (args.out / "sram_untouched_256k.bin").write_bytes(sram_raw)

            print("[ADFU] Stage 2: Switching to adfus.bin @ 0x00118000 & dumping 64 KB ARM Mask Boot ROM...")
            client.switch_adfus_arm(args.adfus_arm.read_bytes())
            rom64 = client.dump_mem_unmasked("ATJ2157", 0x00000000, 64 * 1024)
            (args.out / "bootrom_64k.bin").write_bytes(rom64)

            print("[ADFU] Stage 3: Probing SPI NOR JEDEC/SFDP & dumping full 4 MiB raw + decrypted images...")
            spiread_bin = args.spiread_arm.read_bytes()
            client.write_ram(0x0011E000, spiread_bin, step=0x400)
            client.write_ram(0x0011FFE0, struct.pack("<8I", 0, 0, 0, 0, 1, 0, 0, 0), step=32)
            client.exec_ram(0x0011E001, timeout_ms=8000)
            info_buf = client.read_ram_native(0x0011A000, 0x200, step=0x200)
            (args.out / "spi_nor_info_512b.bin").write_bytes(info_buf)

            bfpt_density = struct.unpack_from("<I", info_buf, 0x134)[0]
            flash_size = (bfpt_density + 1) // 8 if (bfpt_density & 0x80000000) == 0 else 4 * 1024 * 1024
            chunk = 0x4000

            for enc, fname in ((0, "spi_nor_raw_4m.bin"), (1, "spi_nor_decrypted_4m.bin")):
                img = bytearray()
                for off in range(0, flash_size, chunk):
                    client.write_ram(0x0011FFE0, struct.pack("<8I", 0, 0, 0, 0, 2, off, chunk, enc), step=32)
                    client.exec_ram(0x0011E001, timeout_ms=8000)
                    img.extend(client.read_ram_native(0x0011A000, chunk, step=chunk))
                (args.out / fname).write_bytes(img)

            dec_b = (args.out / "spi_nor_decrypted_4m.bin").read_bytes()
            (args.out / "mbrec_512b.bin").write_bytes(dec_b[0x000000:0x000200])
            (args.out / "brec_45k.bin").write_bytes(dec_b[0x000200:0x00B400])
            (args.out / "lfi_stock.bin").write_bytes(dec_b[0x00B400:0x3DEA00])
            (args.out / "vram_nvram_tail_133k.bin").write_bytes(dec_b[0x3DEA00:0x400000])

            client.write_ram(0x0011FFE0, struct.pack("<8I", 0, 0, 0, 0, 3, 0, 0, 0), step=32)
            client.exec_ram(0x0011E001, timeout_ms=4000)

            if not args.stay_in_adfu:
                print("[ADFU] Stage 4: Clearing 0x00130000 ADFU flag & triggering clean Watchdog reboot...")
                client.reboot_atj2157_clean(spiread_bin)
        else:
            print(f"[ADFU] Chip '{chip}' handled via legacy flow; saved adfu_info.bin.")

        sums = []
        for f in sorted(args.out.iterdir()):
            if f.is_file() and f.name not in ("SHA256SUMS", "RECOVERY.md"):
                hsh = hashlib.sha256(f.read_bytes()).hexdigest()
                sums.append(f"{hsh}  {f.name}")
        (args.out / "SHA256SUMS").write_text("\n".join(sums) + "\n")
        print("[ADFU] Completed read-only dump and wrote SHA256SUMS.")
    finally:
        usb.close(h_adfu)


if __name__ == "__main__":
    main()
