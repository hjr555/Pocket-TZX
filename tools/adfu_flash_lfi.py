#!/usr/bin/env python3
"""
Safe LFI-only SPI NOR flasher / restorer for the ATJ2157 voice recorder over ADFU.

Modes (exactly one):
  --plan                 Offline: show which 4 KB blocks/sectors would be written. No USB.
  --verify-device        Read-only: compare the live LFI region (raw) against firmware-stock.
  --self-test-scratch    Writes ONLY the unused all-0xFF block 0x3df000..0x3dffff to prove erase,
                         raw program and hardware-scrambled program, then erases it back to 0xFF.
  --flash                Writes build/lfi_tzx.bin into LFI (only the changed 4 KB blocks).
  --restore-stock        Restores firmware-stock/spi_nor_raw_4m.bin bit-for-bit over
                         0x00b000..0x3defff (only blocks that differ), raw writes only.

Write modes require --yes. Every touched block is backed up (raw) to build/flash_sessions/<ts>/
before erase, and verified after write (raw and/or descrambled read-back).

Address guard (enforced here AND inside the SRAM payload):
  erase   : 0x00b000 <= addr <= 0x3df000, 4 KB aligned   (MBREC/BREC < 0x00b000, VRAM >= 0x3e0000)
  program : 0x00b000 <= addr, addr + 512 <= 0x3e0000; 0x00b000..0x00b3ff (BREC tail) raw-restore only
"""
import argparse
import hashlib
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from adfu_dump_all import AdfuClient, LibUSB, msc_adfu_reboot  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
STOCK_RAW = ROOT / "firmware-stock/spi_nor_raw_4m.bin"
STOCK_DEC = ROOT / "firmware-stock/spi_nor_decrypted_4m.bin"
STOCK_SUMS = ROOT / "firmware-stock/SHA256SUMS"
ADFUS = ROOT / "vendor/kilicdev_actions_flash/payload_arm/adfus.bin"
PAYLOAD = ROOT / "build/spiflash_atj2157.bin"
NEW_LFI = ROOT / "build/lfi_tzx.bin"
SESSIONS = ROOT / "build/flash_sessions"

LFI_BASE, LFI_END = 0x00B400, 0x3DEA00
ERASE_MIN, ERASE_MAX = 0x00B000, 0x3DF000
PROG_MIN, PROG_END = 0x00B000, 0x3E0000
SCRAMBLE_MIN = 0x00B400
SCRATCH = 0x3DF000
REGION_START, REGION_END = 0x00B000, 0x3DF000   # 4 KB-aligned span covering BREC tail + LFI + gap

PAYLOAD_ADDR = 0x0011E000
ARGS = 0x0011FFE0
DATA_BUF = 0x0011A000
FLAG_SCRAMBLE, FLAG_PIO = 1, 2
FF512 = b"\xFF" * 512


def guard_erase(addr: int) -> None:
    if addr & 0xFFF or not (ERASE_MIN <= addr <= ERASE_MAX):
        raise SystemExit(f"GUARD: refusing erase at 0x{addr:06x}")


def guard_program(addr: int, scramble: bool) -> None:
    if addr & 0x1FF or addr < PROG_MIN or addr + 512 > PROG_END or (scramble and addr < SCRAMBLE_MIN):
        raise SystemExit(f"GUARD: refusing program at 0x{addr:06x} (scramble={scramble})")


def check_stock_hashes() -> None:
    sums = dict(line.split()[::-1] for line in STOCK_SUMS.read_text().splitlines() if line.strip())
    for p in (STOCK_RAW, STOCK_DEC):
        if hashlib.sha256(p.read_bytes()).hexdigest() != sums[p.name]:
            raise SystemExit(f"{p.name} does not match SHA256SUMS - refusing to continue")


def build_flash_plan() -> list[dict]:
    """Per 4 KB block: list of (data, scramble, expected_raw|None, expected_plain)."""
    raw = STOCK_RAW.read_bytes()
    dec = STOCK_DEC.read_bytes()
    new = NEW_LFI.read_bytes()
    if len(new) != LFI_END - LFI_BASE:
        raise SystemExit("lfi_tzx.bin has wrong size")
    target = bytearray(dec)
    target[LFI_BASE:LFI_END] = new
    plan = []
    for b in range(REGION_START, REGION_END, 0x1000):
        if target[b : b + 0x1000] == dec[b : b + 0x1000]:
            continue
        secs = []
        for s in range(b, b + 0x1000, 512):
            if target[s : s + 512] == dec[s : s + 512]:
                secs.append(dict(addr=s, data=raw[s : s + 512], scramble=False,
                                 exp_raw=raw[s : s + 512], exp_plain=dec[s : s + 512]))
            else:
                secs.append(dict(addr=s, data=bytes(target[s : s + 512]), scramble=True,
                                 exp_raw=None, exp_plain=bytes(target[s : s + 512])))
        plan.append(dict(addr=b, sectors=secs))
    for blk in plan:
        guard_erase(blk["addr"])
        for s in blk["sectors"]:
            guard_program(s["addr"], s["scramble"])
    return plan


class Flasher:
    def __init__(self, session_dir: Path | None) -> None:
        self.usb = LibUSB()
        self.h = None
        self.c = None
        self.inited = False
        self.session = session_dir
        self.payload = PAYLOAD.read_bytes()
        if len(self.payload) > ARGS - PAYLOAD_ADDR:
            raise SystemExit("payload too large")

    def log(self, msg: str) -> None:
        print(msg, flush=True)
        if self.session:
            with open(self.session / "log.txt", "a") as fh:
                fh.write(f"{time.strftime('%H:%M:%S')} {msg}\n")

    def connect(self, already_adfus: bool = False) -> None:
        self.h = self.usb.open_vid_pid(0x10D6, 0x10D6, wait_s=0.1)
        if not self.h:
            if already_adfus:
                raise SystemExit("Device not found in 10d6:10d6 ADFU mode.")
            h_msc = self.usb.open_vid_pid(0x10D6, 0x1101, wait_s=0.5)
            if not h_msc:
                raise SystemExit("Device not found on USB (neither 10d6:1101 nor 10d6:10d6).")
            tmp = ROOT / "build"
            try:
                msc_adfu_reboot(self.usb, h_msc, tmp)
            finally:
                self.usb.close(h_msc)
            self.h = self.usb.open_vid_pid(0x10D6, 0x10D6, wait_s=15.0)
            if not self.h:
                raise SystemExit("Timed out waiting for ADFU 10d6:10d6.")
        self.c = AdfuClient(self.usb, self.h)
        if already_adfus:
            adfus_hdr = ADFUS.read_bytes()[:16]
            live_hdr = self.c.read_ram_native(0x00118000, 16, step=16)
            if live_hdr != adfus_hdr:
                raise SystemExit("adfus.bin not active at 0x00118000; retry without --already-adfus")
        else:
            _, chip = self.c.adfu_info()
            if chip != "ATJ2157":
                raise SystemExit(f"Unexpected chip {chip}")
            self.c.switch_adfus_arm(ADFUS.read_bytes())
        self.c.write_ram(PAYLOAD_ADDR, self.payload, step=0x400)
        r = self.cmd(1)
        if r[2] != 0x48534C46:
            raise SystemExit(f"payload init failed: {r}")
        self.inited = True
        self.log(f"[ADFU] ATJ2157 connected, payload up, SR1=0x{r[3]:02x}")
        if r[3] & 0x9C:
            raise SystemExit(f"SR1 block-protect/SRWD bits set (0x{r[3]:02x}) - aborting")

    def cmd(self, cmd: int, a: int = 0, b: int = 0, f: int = 0, timeout_ms: int = 8000) -> tuple:
        self.c.write_ram(ARGS, struct.pack("<8I", 0, 0, 0, 0, cmd, a, b, f), step=32)
        self.c.exec_ram(PAYLOAD_ADDR | 1, timeout_ms=timeout_ms)
        return struct.unpack("<8I", self.c.read_ram_native(ARGS, 32, step=32))

    def read(self, addr: int, length: int, descramble: bool) -> bytes:
        out = bytearray()
        for off in range(0, length, 0x4000):
            n = min(0x4000, length - off)
            r = self.cmd(2, addr + off, n, 1 if descramble else 0)
            if r[2] != 0 or r[3] != n:
                raise RuntimeError(f"read 0x{addr + off:06x} failed: {r}")
            out += self.c.read_ram_native(DATA_BUF, n, step=n)
        return bytes(out)

    def erase(self, addr: int) -> None:
        guard_erase(addr)
        r = self.cmd(5, addr, timeout_ms=10000)
        if r[2] >= 0x80000000 or (r[3] & 1):
            raise RuntimeError(f"erase 0x{addr:06x} failed: rc={r[2]:#x} sr={r[3]:#x}")

    def program(self, addr: int, data: bytes, scramble: bool, pio: bool = False) -> None:
        guard_program(addr, scramble)
        self.c.write_ram(DATA_BUF, data, step=0x200)
        flags = (FLAG_SCRAMBLE if scramble else 0) | (FLAG_PIO if pio else 0)
        r = self.cmd(6, addr, 0, flags)
        if r[2] >= 0x80000000 or (r[3] & 1):
            raise RuntimeError(f"program 0x{addr:06x} failed: rc={r[2]:#x} sr={r[3]:#x}")

    def write_block(self, blk: dict) -> None:
        b = blk["addr"]
        pre = self.read(b, 0x1000, False)
        if self.session:
            (self.session / f"pre_raw_{b:06x}.bin").write_bytes(pre)
        self.erase(b)
        if self.read(b, 0x1000, False) != b"\xFF" * 0x1000:
            raise RuntimeError(f"block 0x{b:06x} not blank after erase")
        for s in blk["sectors"]:
            if not s["scramble"] and s["data"] == FF512:
                continue
            self.program(s["addr"], s["data"], s["scramble"], s.get("pio", False))
        got_raw = self.read(b, 0x1000, False)
        got_dec = self.read(b, 0x1000, True)
        for i, s in enumerate(blk["sectors"]):
            o = i * 512
            if s["exp_raw"] is not None and got_raw[o : o + 512] != s["exp_raw"]:
                raise RuntimeError(f"raw verify failed @ 0x{s['addr']:06x}")
            if s["exp_plain"] is not None and got_dec[o : o + 512] != s["exp_plain"]:
                raise RuntimeError(f"descrambled verify failed @ 0x{s['addr']:06x}")
        nsc = sum(1 for s in blk["sectors"] if s["scramble"])
        self.log(f"  block 0x{b:06x} OK ({nsc} scrambled + {8 - nsc} raw-restored sectors)")

    def close(self, reboot: bool) -> None:
        try:
            if self.c and self.inited:
                self.cmd(3)
                if reboot:
                    self.log("[ADFU] Clearing ADFU flag and rebooting to normal mode...")
                    self.c.reboot_atj2157_clean(self.payload)
        finally:
            if self.h:
                self.usb.close(self.h)


def new_session(tag: str) -> Path:
    d = SESSIONS / f"{time.strftime('%Y%m%d_%H%M%S')}_{tag}"
    d.mkdir(parents=True, exist_ok=True)
    return d


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--plan", action="store_true")
    g.add_argument("--verify-device", action="store_true")
    g.add_argument("--self-test-scratch", action="store_true")
    g.add_argument("--flash", action="store_true")
    g.add_argument("--restore-stock", action="store_true")
    ap.add_argument("--yes", action="store_true", help="confirm a write mode")
    ap.add_argument("--stay-in-adfu", action="store_true")
    ap.add_argument("--already-adfus", action="store_true", help="reuse active adfus.bin session")
    ap.add_argument("--update", action="store_true", help="allow updating over a previously flashed LFI")
    args = ap.parse_args()

    check_stock_hashes()
    raw_stock = STOCK_RAW.read_bytes()
    dec_stock = STOCK_DEC.read_bytes()

    if args.plan:
        plan = build_flash_plan()
        nscr = sum(s["scramble"] for b in plan for s in b["sectors"])
        print(f"[PLAN] {len(plan)} x 4 KB blocks, {nscr} scrambled sectors, "
              f"{len(plan) * 8 - nscr} raw-restored sectors")
        print("[PLAN] blocks:", " ".join(f"{b['addr']:06x}" for b in plan))
        print(f"[PLAN] guard OK: all erases in 0x{ERASE_MIN:06x}..0x{ERASE_MAX:06x}; "
              "BREC tail 0x00b000..0x00b3ff raw-restored only; VRAM never touched")
        return

    if (args.self_test_scratch or args.flash or args.restore_stock) and not args.yes:
        raise SystemExit("Write mode requires --yes")

    tag = ("verify" if args.verify_device else "selftest" if args.self_test_scratch
           else "flash" if args.flash else "restore")
    fl = Flasher(new_session(tag))
    ok = False
    try:
        fl.connect(already_adfus=args.already_adfus)
        if args.verify_device:
            live = fl.read(REGION_START, REGION_END - REGION_START, False)
            bad = [b for b in range(0, len(live), 0x1000)
                   if live[b : b + 0x1000] != raw_stock[REGION_START + b : REGION_START + b + 0x1000]]
            vram = fl.read(0x3E0000, 0x20000, False)
            fl.log(f"[VERIFY] LFI region: {len(bad)} block(s) differ from stock"
                   + (f" (first 0x{REGION_START + bad[0]:06x})" if bad else ""))
            fl.log(f"[VERIFY] VRAM 0x3e0000.. sha256={hashlib.sha256(vram).hexdigest()[:16]} "
                   f"(stock {hashlib.sha256(raw_stock[0x3E0000:]).hexdigest()[:16]}, changes at runtime are normal)")
            ok = True

        elif args.self_test_scratch:
            if fl.read(SCRATCH, 0x1000, False) != b"\xFF" * 0x1000:
                raise SystemExit("Scratch block 0x3df000 is not blank - aborting self-test")
            secs = [dict(addr=SCRATCH + i * 512, data=FF512, scramble=False, exp_raw=FF512, exp_plain=None)
                    for i in range(8)]
            # S0: hardware-scrambled DMA write of a real LFI sector plaintext
            secs[0].update(data=dec_stock[LFI_BASE:LFI_BASE + 512], scramble=True, exp_raw=None,
                           exp_plain=dec_stock[LFI_BASE:LFI_BASE + 512])
            # S1: raw write of stock raw bytes (bit-exact path used by --restore-stock)
            secs[1].update(data=raw_stock[0xB600:0xB800], exp_raw=raw_stock[0xB600:0xB800])
            fl.write_block(dict(addr=SCRATCH, sectors=secs))
            got_raw = fl.read(SCRATCH, 0x400, False)
            got_dec = fl.read(SCRATCH, 0x400, True)
            fl.log(f"[INFO] scrambler address-independent (S0 raw == stock raw@0xb400): "
                   f"{got_raw[:512] == raw_stock[LFI_BASE:LFI_BASE + 512]}")
            fl.log(f"[INFO] S1 descrambles to stock plain@0xb600: {got_dec[512:1024] == dec_stock[0xB600:0xB800]}")
            fl.erase(SCRATCH)
            if fl.read(SCRATCH, 0x1000, False) != b"\xFF" * 0x1000:
                raise RuntimeError("scratch block not blank after final erase")
            fl.log("[SELFTEST] PASS - erase, raw program, scrambled program all verified; scratch back to 0xFF")
            ok = True

        elif args.flash:
            plan = build_flash_plan()
            fl.log(f"[FLASH] {len(plan)} blocks to write")
            for blk in plan:
                pre = fl.read(blk["addr"], 0x1000, False)
                if pre != raw_stock[blk["addr"]:blk["addr"] + 0x1000]:
                    dec_now = fl.read(blk["addr"], 0x1000, True)
                    if dec_now == b"".join(s["exp_plain"] for s in blk["sectors"]):
                        fl.log(f"  block 0x{blk['addr']:06x} already up to date - skip")
                        continue
                    if not args.update:
                        raise SystemExit(f"block 0x{blk['addr']:06x} matches neither stock nor target - "
                                         "pass --update or run --restore-stock first")
                fl.write_block(blk)
            fl.log("[FLASH] Full descrambled read-back of LFI...")
            live = fl.read(LFI_BASE & ~0x1FF, LFI_END - LFI_BASE, True)
            if live != NEW_LFI.read_bytes():
                raise RuntimeError("full LFI read-back mismatch")
            tail = fl.read(REGION_START, LFI_BASE - REGION_START, False)
            if tail != raw_stock[REGION_START:LFI_BASE]:
                raise RuntimeError("BREC tail changed!")
            fl.log("[FLASH] PASS - LFI == build/lfi_tzx.bin, BREC tail bit-identical")
            ok = True

        elif args.restore_stock:
            live = fl.read(REGION_START, REGION_END - REGION_START, False)
            bad = [REGION_START + b for b in range(0, len(live), 0x1000)
                   if live[b : b + 0x1000] != raw_stock[REGION_START + b : REGION_START + b + 0x1000]]
            fl.log(f"[RESTORE] {len(bad)} block(s) differ from stock raw")
            for b in bad:
                secs = [dict(addr=s, data=raw_stock[s:s + 512], scramble=False,
                             exp_raw=raw_stock[s:s + 512], exp_plain=None) for s in range(b, b + 0x1000, 512)]
                fl.write_block(dict(addr=b, sectors=secs))
            live = fl.read(REGION_START, REGION_END - REGION_START, False)
            if live != raw_stock[REGION_START:REGION_END]:
                raise RuntimeError("restore verify mismatch")
            fl.log("[RESTORE] PASS - 0x00b000..0x3defff bit-identical to spi_nor_raw_4m.bin")
            ok = True
    finally:
        fl.close(reboot=ok and not args.stay_in_adfu)
        if not ok:
            print("!!! Operation did not complete. Device left in ADFU; run --restore-stock --yes.")


if __name__ == "__main__":
    main()
