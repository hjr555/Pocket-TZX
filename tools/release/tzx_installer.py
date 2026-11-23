#!/usr/bin/env python3
"""
Standalone Cross-Platform (Linux + Windows) Installer / Backup / Restorer
for Actions ATJ2157 ZX Spectrum Next TZX Mode.

Zero external Python packages or third-party DLLs required:
  - Linux   : system libusb-1.0.so.0 via ctypes (MSC 10d6:1101 + ADFU 10d6:10d6)
  - Windows : inbox kernel32.dll IOCTL_SCSI_PASS_THROUGH_DIRECT (10d6:1101)
              + inbox setupapi.dll / winusb.dll (10d6:10d6 via signed WinUSB INF)

Commands:
  install [--update] [--backup-dir DIR] [--yes]   Full backup -> patch -> verify -> flash
  backup  [--out DIR]                             Read-only 4 MiB SPI NOR backup
  restore <backup_dir> [--yes]                    Bit-exact raw restore to factory stock
  verify  <backup_dir>                            Read-only compare device vs backup
"""
import argparse
import ctypes
import hashlib
import os
from pathlib import Path
import platform
import struct
import sys
import time

IS_WINDOWS = platform.system() == "Windows"

VID = 0x10D6
PID_MSC = 0x1101
PID_ADFU = 0x10D6

USBC_SIG = 0x43425355
USBS_SIG = 0x53425355
CMD_ADFU_WRITERAM = 0x13
CMD_ADFU_READRAM = 0x93
CMD_ADFU_SWITCH = 0x20
CMD_ADFU_EXEC = 0x21
CMD_ADFU_READRET = 0x23

# Must match drivers/atj2157_adfu.inf DeviceInterfaceGUIDs
WINUSB_GUID_STR = "{7c6b9a84-4e21-4d8a-9b2d-10d610d60001}"

LFI_BASE, LFI_END = 0x00B400, 0x3DEA00
ERASE_MIN, ERASE_MAX = 0x00B000, 0x3DF000
PROG_MIN, PROG_END = 0x00B000, 0x3E0000
SCRAMBLE_MIN = 0x00B400
SCRATCH = 0x3DF000
REGION_START, REGION_END = 0x00B000, 0x3DF000
FLASH_SIZE = 4 * 1024 * 1024

PAYLOAD_ADDR = 0x0011E000
ARGS = 0x0011FFE0
DATA_BUF = 0x0011A000
FLAG_SCRAMBLE = 1
FF512 = b"\xFF" * 512

SCRIPT_DIR = Path(__file__).resolve().parent
PAYLOADS_DIR = SCRIPT_DIR / "payloads"
BACKUPS_ROOT = SCRIPT_DIR / "backups"


# =============================================================================
# 1. Cross-Platform USB Transport (Linux libusb-1.0 / Windows SCSI + WinUSB)
# =============================================================================

class LinuxUsbBackend:
    def __init__(self) -> None:
        self.lib = ctypes.CDLL("libusb-1.0.so.0")
        self.lib.libusb_init.argtypes = [ctypes.c_void_p]
        self.lib.libusb_open_device_with_vid_pid.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint16]
        self.lib.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p
        self.lib.libusb_kernel_driver_active.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.libusb_detach_kernel_driver.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.libusb_claim_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.libusb_release_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.libusb_reset_device.argtypes = [ctypes.c_void_p]
        self.lib.libusb_clear_halt.argtypes = [ctypes.c_void_p, ctypes.c_uint8]
        self.lib.libusb_close.argtypes = [ctypes.c_void_p]
        self.lib.libusb_bulk_transfer.argtypes = [
            ctypes.c_void_p, ctypes.c_uint8, ctypes.c_void_p,
            ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.c_uint,
        ]
        if self.lib.libusb_init(None) < 0:
            raise RuntimeError("libusb_init failed")
        self.h = None

    def _open_raw(self, pid: int, wait_s: float) -> int | None:
        deadline = time.monotonic() + wait_s
        while True:
            h = self.lib.libusb_open_device_with_vid_pid(None, VID, pid)
            if h:
                if self.lib.libusb_kernel_driver_active(h, 0) == 1:
                    self.lib.libusb_detach_kernel_driver(h, 0)
                rc = self.lib.libusb_claim_interface(h, 0)
                if rc < 0:
                    self.lib.libusb_close(h)
                    raise RuntimeError(f"libusb_claim_interface failed ({rc}). Run install.sh to install udev rules.")
                return h
            if time.monotonic() >= deadline:
                return None
            time.sleep(0.05)

    def _bulk_out_h(self, h: int, ep: int, data: bytes, timeout_ms: int = 4000) -> None:
        xfer = ctypes.c_int(0)
        rc = self.lib.libusb_bulk_transfer(h, ep, data, len(data), ctypes.byref(xfer), timeout_ms)
        if rc != 0 or xfer.value != len(data):
            raise RuntimeError(f"bulk_out(ep=0x{ep:02x}, len={len(data)}) failed: rc={rc}, xfer={xfer.value}")

    def _bulk_in_h(self, h: int, ep: int, length: int, timeout_ms: int = 4000) -> bytes:
        buf = ctypes.create_string_buffer(max(length, 512))
        out = bytearray()
        while len(out) < length:
            rem = length - len(out)
            xfer = ctypes.c_int(0)
            rc = self.lib.libusb_bulk_transfer(h, ep, buf, rem, ctypes.byref(xfer), timeout_ms)
            if rc != 0:
                self.lib.libusb_clear_halt(h, ep)
                raise RuntimeError(f"bulk_in(ep=0x{ep:02x}, rem={rem}) failed: rc={rc}")
            chunk = buf.raw[: xfer.value]
            if not chunk:
                break
            out.extend(chunk)
        return bytes(out)

    def _msc_reboot_to_adfu(self) -> bool:
        h = self._open_raw(PID_MSC, wait_s=0.5)
        if not h:
            return False
        try:
            # Reset endpoint data toggle left over from usb-storage probe
            self.lib.libusb_reset_device(h)
            ep_out, ep_in = 0x02, 0x81
            cbw20 = struct.pack("<IIIBBB16s", USBC_SIG, 1, 20, 0x80, 0, 16, bytes([0xCC, 0, 0, 0, 0, 0, 0, 20]).ljust(16, b"\x00"))
            self._bulk_out_h(h, ep_out, cbw20)
            self._bulk_in_h(h, ep_in, 20)
            self._bulk_in_h(h, ep_in, 13)

            cbw11 = struct.pack("<IIIBBB16s", USBC_SIG, 2, 11, 0x80, 0, 16, bytes([0xCC, 0, 0, 0, 0, 0, 0, 11]).ljust(16, b"\x00"))
            self._bulk_out_h(h, ep_out, cbw11)
            sig11 = self._bulk_in_h(h, ep_in, 11)
            self._bulk_in_h(h, ep_in, 13)
            if sig11 != b"ACTIONSUSBD":
                raise RuntimeError(f"Unexpected Actions MSC signature: {sig11!r}")

            cbw_sw = struct.pack("<IIIBBB16s", USBC_SIG, 3, 2, 0x80, 0, 16, bytes([0xCB, 0x21, 0, 0, 0, 0, 0, 2]).ljust(16, b"\x00"))
            self._bulk_out_h(h, ep_out, cbw_sw)
            ack = self._bulk_in_h(h, ep_in, 2)
            self._bulk_in_h(h, ep_in, 13)
            print(f"[MSC] SwitchToAdfu ack={ack.hex()} -> waiting for 10d6:10d6...", flush=True)
            return True
        finally:
            self.lib.libusb_release_interface(h, 0)
            self.lib.libusb_close(h)

    def open_adfu(self) -> None:
        self.h = self._open_raw(PID_ADFU, wait_s=0.1)
        if not self.h:
            if not self._msc_reboot_to_adfu():
                raise SystemExit("Recorder not found on USB (connect it until the 'connected' screen shows).")
            self.h = self._open_raw(PID_ADFU, wait_s=15.0)
            if not self.h:
                raise SystemExit("Timed out waiting for ADFU (10d6:10d6) enumeration.")

    def bulk_out(self, ep: int, data: bytes, timeout_ms: int = 4000) -> None:
        self._bulk_out_h(self.h, ep, data, timeout_ms)

    def bulk_in(self, ep: int, length: int, timeout_ms: int = 4000) -> bytes:
        return self._bulk_in_h(self.h, ep, length, timeout_ms)

    def close(self) -> None:
        if self.h:
            self.lib.libusb_release_interface(self.h, 0)
            self.lib.libusb_close(self.h)
            self.h = None


class WindowsUsbBackend:
    """Zero-dependency Windows USB backend using inbox SCSI pass-through + WinUSB."""

    def __init__(self) -> None:
        self.k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
        self.winusb = ctypes.WinDLL("winusb", use_last_error=True)
        self.dev_handle = None
        self.usb_handle = None

    @staticmethod
    def _parse_guid(s: str) -> bytes:
        s = s.strip("{}").replace("-", "")
        d1 = int(s[0:8], 16)
        d2 = int(s[8:12], 16)
        d3 = int(s[12:16], 16)
        d4 = bytes.fromhex(s[16:32])
        return struct.pack("<IHH8s", d1, d2, d3, d4)

    def _scsi_pass_through(self, handle: int, cdb: bytes, rx_len: int) -> bytes:
        """Send IOCTL_SCSI_PASS_THROUGH_DIRECT (0x0004D014) to an Actions MSC disk."""
        IOCTL_SCSI_PASS_THROUGH_DIRECT = 0x0004D014
        is_64 = ctypes.sizeof(ctypes.c_void_p) == 8
        rx_buf = ctypes.create_string_buffer(max(rx_len, 32))

        class SPTD_64(ctypes.Structure):
            _fields_ = [
                ("Length", ctypes.c_ushort), ("ScsiStatus", ctypes.c_ubyte),
                ("PathId", ctypes.c_ubyte), ("TargetId", ctypes.c_ubyte),
                ("Lun", ctypes.c_ubyte), ("CdbLength", ctypes.c_ubyte),
                ("SenseInfoLength", ctypes.c_ubyte), ("DataIn", ctypes.c_ubyte),
                ("Pad0", ctypes.c_ubyte * 3),
                ("DataTransferLength", ctypes.c_uint32),
                ("TimeOutValue", ctypes.c_uint32),
                ("Pad1", ctypes.c_uint32),
                ("DataBuffer", ctypes.c_void_p),
                ("SenseInfoOffset", ctypes.c_uint32),
                ("Cdb", ctypes.c_ubyte * 16),
                ("Pad2", ctypes.c_uint32),
            ]

        class SPTD_32(ctypes.Structure):
            _fields_ = [
                ("Length", ctypes.c_ushort), ("ScsiStatus", ctypes.c_ubyte),
                ("PathId", ctypes.c_ubyte), ("TargetId", ctypes.c_ubyte),
                ("Lun", ctypes.c_ubyte), ("CdbLength", ctypes.c_ubyte),
                ("SenseInfoLength", ctypes.c_ubyte), ("DataIn", ctypes.c_ubyte),
                ("Pad0", ctypes.c_ubyte * 3),
                ("DataTransferLength", ctypes.c_uint32),
                ("TimeOutValue", ctypes.c_uint32),
                ("DataBuffer", ctypes.c_void_p),
                ("SenseInfoOffset", ctypes.c_uint32),
                ("Cdb", ctypes.c_ubyte * 16),
            ]

        Base = SPTD_64 if is_64 else SPTD_32

        class SPTDWithSense(ctypes.Structure):
            _fields_ = [("sptd", Base), ("sense", ctypes.c_ubyte * 32)]

        req = SPTDWithSense()
        req.sptd.Length = ctypes.sizeof(Base)
        req.sptd.CdbLength = len(cdb)
        req.sptd.SenseInfoLength = 32
        req.sptd.DataIn = 1  # SCSI_IOCTL_DATA_IN
        req.sptd.DataTransferLength = rx_len
        req.sptd.TimeOutValue = 5
        req.sptd.DataBuffer = ctypes.addressof(rx_buf)
        req.sptd.SenseInfoOffset = ctypes.sizeof(Base)
        for i, b in enumerate(cdb):
            req.sptd.Cdb[i] = b

        returned = ctypes.c_uint32(0)
        ok = self.k32.DeviceIoControl(
            ctypes.c_void_p(handle), IOCTL_SCSI_PASS_THROUGH_DIRECT,
            ctypes.byref(req), ctypes.sizeof(req),
            ctypes.byref(req), ctypes.sizeof(req),
            ctypes.byref(returned), None,
        )
        if not ok:
            raise OSError(ctypes.get_last_error())
        return rx_buf.raw[:rx_len]

    def _msc_reboot_to_adfu(self) -> bool:
        """Probe removable PhysicalDrive0..15 for ACTIONSUSBD and trigger 0xCB 0x21 ADFU reboot."""
        GENERIC_RW = 0xC0000000
        FILE_SHARE_RW = 3
        OPEN_EXISTING = 3
        INVALID_HANDLE = ctypes.c_void_p(-1).value
        self.k32.CreateFileW.restype = ctypes.c_void_p

        for idx in range(16):
            path = f"\\\\.\\PhysicalDrive{idx}"
            h = self.k32.CreateFileW(path, GENERIC_RW, FILE_SHARE_RW, None, OPEN_EXISTING, 0, None)
            if h in (None, 0, INVALID_HANDLE):
                continue
            try:
                sig = self._scsi_pass_through(h, bytes([0xCC, 0, 0, 0, 0, 0, 0, 11]).ljust(16, b"\x00"), 11)
                if sig != b"ACTIONSUSBD":
                    continue
                self._scsi_pass_through(h, bytes([0xCC, 0, 0, 0, 0, 0, 0, 20]).ljust(16, b"\x00"), 20)
                ack = self._scsi_pass_through(h, bytes([0xCB, 0x21, 0, 0, 0, 0, 0, 2]).ljust(16, b"\x00"), 2)
                print(f"[MSC] Found Actions recorder on {path}, SwitchToAdfu ack={ack.hex()} -> waiting for ADFU...", flush=True)
                return True
            except OSError:
                continue
            finally:
                self.k32.CloseHandle(ctypes.c_void_p(h))
        return False

    def _find_winusb_path(self) -> str | None:
        DIGCF_PRESENT = 0x02
        DIGCF_DEVICEINTERFACE = 0x10
        guid = ctypes.create_string_buffer(self._parse_guid(WINUSB_GUID_STR))
        self.setupapi.SetupDiGetClassDevsW.restype = ctypes.c_void_p
        hdev = self.setupapi.SetupDiGetClassDevsW(guid, None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
        if hdev in (None, 0, ctypes.c_void_p(-1).value):
            return None
        try:
            class SP_DEV_IF(ctypes.Structure):
                _fields_ = [
                    ("cbSize", ctypes.c_uint32),
                    ("InterfaceClassGuid", ctypes.c_ubyte * 16),
                    ("Flags", ctypes.c_uint32),
                    ("Reserved", ctypes.c_void_p),
                ]
            dif = SP_DEV_IF()
            dif.cbSize = ctypes.sizeof(dif)
            if not self.setupapi.SetupDiEnumDeviceInterfaces(ctypes.c_void_p(hdev), None, guid, 0, ctypes.byref(dif)):
                return None
            req_sz = ctypes.c_uint32(0)
            self.setupapi.SetupDiGetDeviceInterfaceDetailW(ctypes.c_void_p(hdev), ctypes.byref(dif), None, 0, ctypes.byref(req_sz), None)
            if req_sz.value == 0:
                return None
            buf = ctypes.create_string_buffer(req_sz.value)
            cb = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
            struct.pack_into("<I", buf, 0, cb)
            if not self.setupapi.SetupDiGetDeviceInterfaceDetailW(ctypes.c_void_p(hdev), ctypes.byref(dif), buf, req_sz.value, None, None):
                return None
            return ctypes.wstring_at(ctypes.addressof(buf) + 4)
        finally:
            self.setupapi.SetupDiDestroyDeviceInfoList(ctypes.c_void_p(hdev))

    def _open_winusb(self, wait_s: float) -> bool:
        GENERIC_RW = 0xC0000000
        FILE_SHARE_RW = 3
        OPEN_EXISTING = 3
        FILE_FLAG_OVERLAPPED = 0x40000000
        INVALID_HANDLE = ctypes.c_void_p(-1).value
        self.k32.CreateFileW.restype = ctypes.c_void_p
        deadline = time.monotonic() + wait_s
        while True:
            path = self._find_winusb_path()
            if path:
                dh = self.k32.CreateFileW(path, GENERIC_RW, FILE_SHARE_RW, None, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, None)
                if dh not in (None, 0, INVALID_HANDLE):
                    uh = ctypes.c_void_p()
                    if self.winusb.WinUsb_Initialize(ctypes.c_void_p(dh), ctypes.byref(uh)):
                        self.dev_handle = dh
                        self.usb_handle = uh.value
                        return True
                    self.k32.CloseHandle(ctypes.c_void_p(dh))
            if time.monotonic() >= deadline:
                return False
            time.sleep(0.1)

    def open_adfu(self) -> None:
        if not self._open_winusb(wait_s=0.2):
            if not self._msc_reboot_to_adfu():
                raise SystemExit("Recorder not found on USB (connect it until the 'connected' screen shows, and run install.ps1 as Administrator).")
            if not self._open_winusb(wait_s=15.0):
                raise SystemExit("Timed out waiting for ADFU (10d6:10d6) WinUSB interface. Re-run install.ps1 to install the WinUSB driver.")

    def _set_timeout(self, ep: int, timeout_ms: int) -> None:
        PIPE_TRANSFER_TIMEOUT = 0x03
        val = ctypes.c_uint32(timeout_ms)
        self.winusb.WinUsb_SetPipePolicy(ctypes.c_void_p(self.usb_handle), ctypes.c_ubyte(ep), PIPE_TRANSFER_TIMEOUT, 4, ctypes.byref(val))

    def bulk_out(self, ep: int, data: bytes, timeout_ms: int = 4000) -> None:
        self._set_timeout(ep, timeout_ms)
        xfer = ctypes.c_uint32(0)
        buf = ctypes.create_string_buffer(data, len(data))
        ok = self.winusb.WinUsb_WritePipe(ctypes.c_void_p(self.usb_handle), ctypes.c_ubyte(ep), buf, len(data), ctypes.byref(xfer), None)
        if not ok or xfer.value != len(data):
            raise RuntimeError(f"WinUsb_WritePipe(ep=0x{ep:02x}, len={len(data)}) failed: err={ctypes.get_last_error()}")

    def bulk_in(self, ep: int, length: int, timeout_ms: int = 4000) -> bytes:
        self._set_timeout(ep, timeout_ms)
        buf = ctypes.create_string_buffer(max(length, 512))
        out = bytearray()
        while len(out) < length:
            rem = length - len(out)
            xfer = ctypes.c_uint32(0)
            ok = self.winusb.WinUsb_ReadPipe(ctypes.c_void_p(self.usb_handle), ctypes.c_ubyte(ep), buf, rem, ctypes.byref(xfer), None)
            if not ok:
                self.winusb.WinUsb_ResetPipe(ctypes.c_void_p(self.usb_handle), ctypes.c_ubyte(ep))
                raise RuntimeError(f"WinUsb_ReadPipe(ep=0x{ep:02x}, rem={rem}) failed: err={ctypes.get_last_error()}")
            if xfer.value == 0:
                break
            out.extend(buf.raw[: xfer.value])
        return bytes(out)

    def close(self) -> None:
        if self.usb_handle:
            self.winusb.WinUsb_Free(ctypes.c_void_p(self.usb_handle))
            self.usb_handle = None
        if self.dev_handle:
            self.k32.CloseHandle(ctypes.c_void_p(self.dev_handle))
            self.dev_handle = None


# =============================================================================
# 2. ADFU Protocol & SPI NOR Flasher (with Hard Address Guards)
# =============================================================================

def guard_erase(addr: int) -> None:
    if addr & 0xFFF or not (ERASE_MIN <= addr <= ERASE_MAX):
        raise SystemExit(f"GUARD: refusing erase at 0x{addr:06x}")


def guard_program(addr: int, scramble: bool) -> None:
    if addr & 0x1FF or addr < PROG_MIN or addr + 512 > PROG_END or (scramble and addr < SCRAMBLE_MIN):
        raise SystemExit(f"GUARD: refusing program at 0x{addr:06x} (scramble={scramble})")


class AdfuFlasher:
    def __init__(self) -> None:
        self.usb = WindowsUsbBackend() if IS_WINDOWS else LinuxUsbBackend()
        self.ep_in = 0x81
        self.ep_out = 0x02
        self.inited = False
        self.adfus = (PAYLOADS_DIR / "adfus.bin").read_bytes()
        self.payload = (PAYLOADS_DIR / "spiflash_atj2157.bin").read_bytes()

    def _check_csw(self, csw: bytes) -> None:
        if len(csw) != 13:
            raise RuntimeError(f"Bad CSW length: {len(csw)}")
        sig, _, _, st = struct.unpack("<IIIB", csw)
        if sig != USBS_SIG or st != 0:
            raise RuntimeError(f"Bad CSW: sig=0x{sig:08x} status={st}")

    def _cmd_0xcd(self, subcmd: int, length: int, addr: int, recv: bool, data_len: int) -> None:
        cdb = struct.pack("<BIII3s", 0xCD, subcmd, length, addr, b"\x00\x00\x00")
        cbw = struct.pack("<IIIBBB16s", USBC_SIG, 0, data_len, 0x80 if recv else 0x00, 0, 16, cdb)
        self.usb.bulk_out(self.ep_out, cbw)

    def adfu_info(self) -> tuple[bytes, str]:
        cdb = bytearray(16)
        cdb[0] = 0xCC
        cdb[7] = 0x12
        cbw = struct.pack("<IIIBBB16s", USBC_SIG, 0, 0x12, 0x80, 0, 16, bytes(cdb))
        for cand_out in (0x02, 0x01):
            try:
                self.usb.bulk_out(cand_out, cbw)
                self.ep_out = cand_out
                break
            except RuntimeError:
                continue
        resp = b""
        for cand_in in (0x81, 0x82):
            try:
                resp = self.usb.bulk_in(cand_in, 0x12)
                self.ep_in = cand_in
                break
            except RuntimeError:
                continue
        self._check_csw(self.usb.bulk_in(self.ep_in, 13))
        chip = "UNKNOWN"
        if resp[:7] == b"\x00CADFUD":
            cid = (resp[7] << 8) | resp[8]
            chip = {0x10D6: "ATJ2127", 0x3051: "ATJ2157"}.get(cid, f"ID_0x{cid:04x}")
        return resp, chip

    def read_ram(self, addr: int, size: int, step: int = 0x200) -> bytes:
        out = bytearray()
        off = 0
        while off < size:
            n = min(step, size - off)
            self._cmd_0xcd(CMD_ADFU_READRAM, n, (addr + off) & 0xFFFFFFFF, True, n)
            out.extend(self.usb.bulk_in(self.ep_in, n))
            self._check_csw(self.usb.bulk_in(self.ep_in, 13))
            off += n
        return bytes(out)

    def write_ram(self, addr: int, data: bytes, step: int = 0x200) -> None:
        off = 0
        while off < len(data):
            chunk = data[off : off + step]
            self._cmd_0xcd(CMD_ADFU_WRITERAM, len(chunk), (addr + off) & 0xFFFFFFFF, False, len(chunk))
            self.usb.bulk_out(self.ep_out, chunk)
            self._check_csw(self.usb.bulk_in(self.ep_in, 13))
            off += len(chunk)

    def exec_ram(self, addr: int, timeout_ms: int = 8000) -> None:
        self._cmd_0xcd(CMD_ADFU_EXEC, 0, addr & 0xFFFFFFFF, False, 0)
        self._check_csw(self.usb.bulk_in(self.ep_in, 13, timeout_ms=timeout_ms))

    def connect(self) -> None:
        self.usb.open_adfu()
        _, chip = self.adfu_info()
        if chip != "ATJ2157":
            raise SystemExit(f"Unsupported chip '{chip}' — this installer only supports ATJ2157 (0x3051).")
        self.write_ram(0x00118500, b"\x00" * 0x200, step=0x200)
        self.write_ram(0x00118000, self.adfus, step=0x200)
        self._cmd_0xcd(CMD_ADFU_SWITCH, 0, 0x00118000, False, 0)
        self._check_csw(self.usb.bulk_in(self.ep_in, 13))
        time.sleep(0.05)

        self.write_ram(PAYLOAD_ADDR, self.payload, step=0x400)
        r = self.cmd(1)
        if r[2] != 0x48534C46:
            raise SystemExit(f"SPI flash payload init failed: {r}")
        self.inited = True
        print(f"[ADFU] Connected to ATJ2157 (SR1=0x{r[3]:02x})", flush=True)
        if r[3] & 0x9C:
            raise SystemExit(f"Flash write-protect bits set (SR1=0x{r[3]:02x}) — aborting.")

    def cmd(self, cmd: int, a: int = 0, b: int = 0, f: int = 0, timeout_ms: int = 8000) -> tuple:
        self.write_ram(ARGS, struct.pack("<8I", 0, 0, 0, 0, cmd, a, b, f), step=32)
        self.exec_ram(PAYLOAD_ADDR | 1, timeout_ms=timeout_ms)
        return struct.unpack("<8I", self.read_ram(ARGS, 32, step=32))

    def read_flash(self, addr: int, length: int, descramble: bool) -> bytes:
        out = bytearray()
        for off in range(0, length, 0x4000):
            n = min(0x4000, length - off)
            r = self.cmd(2, addr + off, n, 1 if descramble else 0)
            if r[2] != 0 or r[3] != n:
                raise RuntimeError(f"read 0x{addr + off:06x} failed: {r}")
            out.extend(self.read_ram(DATA_BUF, n, step=n))
        return bytes(out)

    def erase_4k(self, addr: int) -> None:
        guard_erase(addr)
        r = self.cmd(5, addr, timeout_ms=10000)
        if r[2] >= 0x80000000 or (r[3] & 1):
            raise RuntimeError(f"erase 0x{addr:06x} failed: rc={r[2]:#x} sr={r[3]:#x}")

    def program_512(self, addr: int, data: bytes, scramble: bool) -> None:
        guard_program(addr, scramble)
        self.write_ram(DATA_BUF, data, step=0x200)
        r = self.cmd(6, addr, 0, FLAG_SCRAMBLE if scramble else 0)
        if r[2] >= 0x80000000 or (r[3] & 1):
            raise RuntimeError(f"program 0x{addr:06x} failed: rc={r[2]:#x} sr={r[3]:#x}")

    def write_block(self, b: int, sectors: list[dict]) -> None:
        self.erase_4k(b)
        if self.read_flash(b, 0x1000, False) != b"\xFF" * 0x1000:
            raise RuntimeError(f"block 0x{b:06x} not blank after erase")
        for s in sectors:
            if not s["scramble"] and s["data"] == FF512:
                continue
            self.program_512(s["addr"], s["data"], s["scramble"])
        got_raw = self.read_flash(b, 0x1000, False)
        got_dec = self.read_flash(b, 0x1000, True)
        for i, s in enumerate(sectors):
            o = i * 512
            if s["exp_raw"] is not None and got_raw[o : o + 512] != s["exp_raw"]:
                raise RuntimeError(f"raw verify failed @ 0x{s['addr']:06x}")
            if s["exp_plain"] is not None and got_dec[o : o + 512] != s["exp_plain"]:
                raise RuntimeError(f"descrambled verify failed @ 0x{s['addr']:06x}")

    def close(self, reboot: bool) -> None:
        try:
            if self.inited:
                self.cmd(3)
                if reboot:
                    print("[ADFU] Clearing ADFU flag & rebooting to normal mode...", flush=True)
                    self.write_ram(0x00130000, b"\x00" * 16, step=16)
                    self.write_ram(ARGS, struct.pack("<8I", 0, 0, 0, 0, 4, 0, 0, 0), step=32)
                    self.exec_ram(PAYLOAD_ADDR | 1, timeout_ms=4000)
                    self._cmd_0xcd(CMD_ADFU_READRET, 16, 0, True, 16)
                    self.usb.bulk_in(self.ep_in, 16)
                    self._check_csw(self.usb.bulk_in(self.ep_in, 13))
                    cbw_rst = struct.pack("<IIIBBB16s", USBC_SIG, 0, 0, 0x00, 0, 16, bytes([0xB0]) + b"\x00" * 15)
                    self.usb.bulk_out(self.ep_out, cbw_rst)
                    self._check_csw(self.usb.bulk_in(self.ep_in, 13, timeout_ms=2000))
        finally:
            self.usb.close()


# =============================================================================
# 3. Live LFI Patcher (Patches Connected Device's Own LFI In-Place)
# =============================================================================

def csum32(data: bytes) -> int:
    if len(data) % 4:
        data += b"\x00" * (4 - len(data) % 4)
    return sum(struct.unpack(f"<{len(data)//4}I", data)) & 0xFFFFFFFF


def csum16(data: bytes) -> int:
    return sum(struct.unpack(f"<{len(data)//2}H", data)) & 0xFFFF


def patch_config_bin(src: bytes) -> bytes:
    if src[:4] != b"cfg\x00":
        raise ValueError("Invalid CONFIG.BIN magic")
    first_off, _ = struct.unpack_from("<II", src, 0x10)
    entries = [list(struct.unpack_from("<II", src, 0x10 + i * 8)) for i in range((first_off - 0x10) // 8)]
    off1, len1 = entries[1]
    cur = src[off1 : off1 + len1]
    new_item1 = struct.pack("<8H", 6, 6, 0, 6, 7, 5, 9, 10)
    if cur == new_item1:
        return src  # Already patched
    if cur != struct.pack("<7H", 6, 5, 0, 6, 7, 5, 10):
        raise ValueError(f"Incompatible CONFIG.BIN Main Menu array: {cur.hex()}")
    delta = len(new_item1) - len1
    if src[-delta:] != b"\x00" * delta:
        raise ValueError("CONFIG.BIN lacks trailing zero padding")
    out = bytearray(src[:off1] + new_item1 + src[off1 + len1 : -delta])
    entries[1][1] = len(new_item1)
    for i in range(2, len(entries)):
        if entries[i][0] > off1:
            entries[i][0] += delta
    for i, (o, l) in enumerate(entries):
        struct.pack_into("<II", out, 0x10 + i * 8, o, l)
    return bytes(out)


def patch_mainmenu_ap(src: bytes) -> bytes:
    out = bytearray(src)
    for foff in (0x2A02, 0x2A3A):
        cur = bytes(out[foff : foff + 4])
        if cur == bytes.fromhex("45f2ab51"):
            continue  # Already patched
        if cur != bytes.fromhex("45f2aa51"):
            raise ValueError(f"Incompatible MAINMENU.AP @ 0x{foff:x}: {cur.hex()}")
        out[foff : foff + 4] = bytes.fromhex("45f2ab51")
    return bytes(out)


def render_cassette_icon(orig_pic6: bytes) -> bytes:
    px = list(struct.unpack("<8192H", orig_pic6))
    for y in range(64):
        for x in range(24, 104):
            px[y * 128 + x] = 0xFFFF

    def sp(x: int, y: int) -> None:
        if 24 <= x < 104 and 0 <= y < 64:
            px[y * 128 + x] = 0x0000

    def hl(x0: int, x1: int, y: int) -> None:
        for x in range(x0, x1 + 1):
            sp(x, y)

    def vl(x: int, y0: int, y1: int) -> None:
        for y in range(y0, y1 + 1):
            sp(x, y)

    hl(34, 93, 6); hl(33, 94, 7); hl(33, 94, 41); hl(34, 93, 42)
    vl(32, 8, 40); vl(33, 7, 41); vl(95, 8, 40); vl(94, 7, 41)
    for sx, sy in ((36, 10), (91, 10), (36, 38), (91, 38)):
        sp(sx, sy); sp(sx + 1, sy); sp(sx, sy + 1)
    hl(38, 89, 11)
    for s in range(4):
        for dy in range(5):
            sp(80 + s * 2 - dy, 12 + dy); sp(81 + s * 2 - dy, 12 + dy)
    hl(41, 86, 16); hl(41, 86, 30); vl(40, 17, 29); vl(87, 17, 29)
    for cx in (49, 78):
        for dy in range(-5, 6):
            for dx in range(-5, 6):
                if 10 <= dx * dx + dy * dy <= 27:
                    sp(cx + dx, 23 + dy)
        sp(cx, 20); sp(cx, 26); sp(cx - 3, 23); sp(cx + 3, 23)
    hl(57, 70, 19); hl(57, 70, 27); vl(57, 19, 27); vl(70, 19, 27); vl(61, 20, 26); vl(66, 20, 26)
    for i, y in enumerate(range(34, 42)):
        xl, xr = 46 - (i // 2), 81 + (i // 2)
        sp(xl, y); sp(xl + 1, y); sp(xr, y); sp(xr - 1, y)
    hl(46, 81, 34)
    for hx in (52, 60, 67, 75):
        sp(hx, 38); sp(hx + 1, 38); sp(hx, 39); sp(hx + 1, 39)
    return struct.pack("<8192H", *px)


def patch_mainmenu_sty(src: bytes) -> bytes:
    out = bytearray(src)
    out[0x0F770 : 0x0F770 + 0x4000] = render_cassette_icon(bytes(out[0x0F770 : 0x0F770 + 0x4000]))
    tzx_utf16 = "TZX Mode\x00".encode("utf-16le")
    cur = bytes(out[0x1DB40 : 0x1DB40 + len(tzx_utf16)])
    if cur != tzx_utf16 and bytes(out[0x1DB40 : 0x1DB60]) != b"\xFF" * 32:
        raise ValueError("Incompatible MAINMENU.STY string padding @ 0x1db40")
    out[0x1DB40 : 0x1DB40 + len(tzx_utf16)] = tzx_utf16
    for tbl in (0x1DB70, 0x1DE68, 0x1E168, 0x1E4A0, 0x1E7D4, 0x1EB08, 0x1EE08):
        struct.pack_into("<II", out, tbl + 24 * 8, 0x1DB40, len(tzx_utf16))
    return bytes(out)


def build_patched_lfi(stock_lfi: bytes, tools_ap: bytes) -> bytes:
    if struct.unpack_from("<I", stock_lfi, 0)[0] != 0x0FF0AA55:
        raise ValueError("Invalid LFI magic — incompatible recorder firmware.")
    if csum16(stock_lfi[:0x1FE]) != struct.unpack_from("<H", stock_lfi, 0x1FE)[0]:
        raise ValueError("LFI header checksum mismatch on device.")
    if csum32(stock_lfi[0x200:0x2000]) != struct.unpack_from("<I", stock_lfi, 0x10)[0]:
        raise ValueError("LFI directory checksum mismatch on device.")

    out = bytearray(stock_lfi)
    files = {}
    for idx in range((0x2000 - 0x200) // 32):
        eo = 0x200 + idx * 32
        ent = out[eo : eo + 32]
        if ent[0] in (0x00, 0xFF):
            break
        base = bytes(ent[:8]).rstrip(b" ").decode("ascii", errors="replace")
        ext = bytes(ent[8:11]).rstrip(b" ").decode("ascii", errors="replace")
        nm = f"{base}.{ext}" if ext else base
        off_s, blen, rsv, old_cs = struct.unpack_from("<IIII", ent, 0x10)
        blob = bytes(out[off_s * 512 : off_s * 512 + blen])
        if csum32(blob) != old_cs:
            raise ValueError(f"Corrupt file checksum in device LFI: {nm}")
        files[nm] = (eo, off_s, blen, rsv, blob)

    needed = ("CONFIG.BIN", "CONFIG.TXT", "MAINMENU.AP", "MAINMENU.STY", "TOOLS.AP")
    for k in needed:
        if k not in files:
            raise ValueError(f"Device LFI missing {k} — incompatible firmware.")

    orig_txt = files["CONFIG.TXT"][4]
    cfg_txt = orig_txt.replace(b"MAINMENU_AP_ID_ARRAY = 6[0,6, 7,5,10];", b"MAINMENU_AP_ID_ARRAY = 6[0,6,7,5,9,10];")[: len(orig_txt)]
    replacements = {
        "CONFIG.BIN": patch_config_bin(files["CONFIG.BIN"][4]),
        "CONFIG.TXT": cfg_txt,
        "MAINMENU.AP": patch_mainmenu_ap(files["MAINMENU.AP"][4]),
        "MAINMENU.STY": patch_mainmenu_sty(files["MAINMENU.STY"][4]),
        "TOOLS.AP": tools_ap,
    }
    for nm, blob in replacements.items():
        eo, off_s, blen, rsv, _ = files[nm]
        if len(blob) != blen:
            raise ValueError(f"{nm}: length {len(blob)} != {blen}")
        out[off_s * 512 : off_s * 512 + blen] = blob
        struct.pack_into("<IIII", out, eo + 0x10, off_s, blen, rsv, csum32(blob))

    struct.pack_into("<I", out, 0x10, csum32(bytes(out[0x200:0x2000])))
    struct.pack_into("<H", out, 0x1FE, csum16(bytes(out[:0x1FE])))
    return bytes(out)


def save_backup(out_dir: Path, raw_4m: bytes, dec_4m: bytes) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    files = {
        "spi_nor_raw_4m.bin": raw_4m,
        "spi_nor_decrypted_4m.bin": dec_4m,
        "lfi_stock.bin": dec_4m[LFI_BASE:LFI_END],
    }
    lines = []
    for nm, data in files.items():
        (out_dir / nm).write_bytes(data)
        lines.append(f"{hashlib.sha256(data).hexdigest()}  {nm}")
    (out_dir / "SHA256SUMS").write_text("\n".join(lines) + "\n")
    print(f"[BACKUP] Saved verified 4 MiB backup to {out_dir}", flush=True)


def verify_backup_dir(bdir: Path) -> tuple[bytes, bytes]:
    sums_path = bdir / "SHA256SUMS"
    if not sums_path.exists():
        raise SystemExit(f"Missing SHA256SUMS in {bdir}")
    sums = dict(l.split()[::-1] for l in sums_path.read_text().splitlines() if l.strip())
    raw = (bdir / "spi_nor_raw_4m.bin").read_bytes()
    dec = (bdir / "spi_nor_decrypted_4m.bin").read_bytes()
    if hashlib.sha256(raw).hexdigest() != sums["spi_nor_raw_4m.bin"]:
        raise SystemExit("Backup spi_nor_raw_4m.bin SHA-256 mismatch!")
    if hashlib.sha256(dec).hexdigest() != sums["spi_nor_decrypted_4m.bin"]:
        raise SystemExit("Backup spi_nor_decrypted_4m.bin SHA-256 mismatch!")
    return raw, dec


# =============================================================================
# 4. Main CLI
# =============================================================================

def main() -> None:
    ap = argparse.ArgumentParser(description="ATJ2157 ZX Spectrum Next TZX Mode Installer")
    sub = ap.add_subparsers(dest="action", required=True)

    p_inst = sub.add_parser("install", help="Backup stock flash, patch LFI, self-test scratch, and flash TZX Mode")
    p_inst.add_argument("--backup-dir", type=Path, default=None, help="Existing stock backup dir (for updates)")
    p_inst.add_argument("--yes", action="store_true", help="Skip interactive confirmation")

    p_bak = sub.add_parser("backup", help="Read-only full 4 MiB flash backup")
    p_bak.add_argument("--out", type=Path, default=None)

    sub.add_parser("check", help="Read-only compatibility check (writes nothing)")

    p_res = sub.add_parser("restore", help="Bit-exact raw restore from a backup directory")
    p_res.add_argument("backup_dir", type=Path)
    p_res.add_argument("--yes", action="store_true")

    args = ap.parse_args()
    fl = AdfuFlasher()
    ok = False
    try:
        fl.connect()

        if args.action == "check":
            print("[CHECK] Reading firmware info (read-only, nothing will be written)...", flush=True)
            lfi = fl.read_flash(LFI_BASE, LFI_END - LFI_BASE, True)
            ver = lfi[4:12].split(b"\x00", 1)[0].decode("ascii", errors="replace")
            tools_ap = (PAYLOADS_DIR / "TOOLS.AP").read_bytes()
            try:
                patched = build_patched_lfi(lfi, tools_ap)
            except Exception as exc:
                print(f"\n[RESULT] NOT COMPATIBLE: {exc}")
                print(f"         Chip: ATJ2157 | Firmware version: {ver!r}")
                ok = True
                return
            if patched == lfi:
                print(f"\n[RESULT] COMPATIBLE — TZX Mode is already installed and up to date! (Firmware {ver})")
            else:
                changed = sum(
                    1 for off in range(0, len(lfi), 0x1000)
                    if patched[off : off + 0x1000] != lfi[off : off + 0x1000]
                )
                print(f"\n[RESULT] COMPATIBLE! (Chip: ATJ2157, Firmware: {ver}, {changed} blocks to update)")
                print("         Safe to run install.sh / install.ps1.")
            ok = True

        elif args.action == "backup":
            out_dir = args.out or (BACKUPS_ROOT / f"backup_{time.strftime('%Y%m%d_%H%M%S')}")
            print("[BACKUP] Reading 4 MiB raw + descrambled SPI NOR...", flush=True)
            raw = fl.read_flash(0, FLASH_SIZE, False)
            dec = fl.read_flash(0, FLASH_SIZE, True)
            save_backup(out_dir, raw, dec)
            ok = True

        elif args.action == "install":
            tools_ap = (PAYLOADS_DIR / "TOOLS.AP").read_bytes()
            if args.backup_dir:
                raw_base, dec_base = verify_backup_dir(args.backup_dir)
                print(f"[INSTALL] Using verified stock backup: {args.backup_dir}", flush=True)
            else:
                out_dir = BACKUPS_ROOT / f"backup_{time.strftime('%Y%m%d_%H%M%S')}"
                print("[INSTALL] Step 1/4: Backing up full 4 MiB flash before any writes...", flush=True)
                raw_base = fl.read_flash(0, FLASH_SIZE, False)
                dec_base = fl.read_flash(0, FLASH_SIZE, True)
                save_backup(out_dir, raw_base, dec_base)

            print("[INSTALL] Step 2/4: Patching LFI & verifying checksums...", flush=True)
            new_lfi = build_patched_lfi(dec_base[LFI_BASE:LFI_END], tools_ap)
            target_dec = bytearray(dec_base)
            target_dec[LFI_BASE:LFI_END] = new_lfi

            # Compare against live flash so only blocks that actually differ are erased/written
            live_dec = fl.read_flash(REGION_START, REGION_END - REGION_START, True)
            blocks = []
            for b in range(REGION_START, REGION_END, 0x1000):
                off = b - REGION_START
                if live_dec[off : off + 0x1000] == target_dec[b : b + 0x1000]:
                    continue
                secs = []
                for s in range(b, b + 0x1000, 512):
                    if target_dec[s : s + 512] == dec_base[s : s + 512]:
                        secs.append(dict(addr=s, data=raw_base[s : s + 512], scramble=False,
                                         exp_raw=raw_base[s : s + 512], exp_plain=dec_base[s : s + 512]))
                    else:
                        secs.append(dict(addr=s, data=bytes(target_dec[s : s + 512]), scramble=True,
                                         exp_raw=None, exp_plain=bytes(target_dec[s : s + 512])))
                blocks.append((b, secs))

            if not blocks:
                print("[INSTALL] Device is already running this exact TZX Mode build — nothing to write!", flush=True)
                ok = True
                return

            if not args.yes:
                ans = input(f"Ready to flash {len(blocks)} x 4 KB LFI blocks (MBREC/BREC/VRAM untouched). Proceed? [y/N] ").strip().lower()
                if ans not in ("y", "yes"):
                    print("Cancelled — nothing written to flash.")
                    ok = True
                    return

            # Quick scratch-block verification on unused 0x3df000 (if blank) before touching LFI
            if fl.read_flash(SCRATCH, 0x1000, False) == b"\xFF" * 0x1000:
                print("[INSTALL] Step 3/4: Verifying erase & hardware scrambler on scratch block 0x3df000...", flush=True)
                test_secs = [dict(addr=SCRATCH + i * 512, data=FF512, scramble=False, exp_raw=FF512, exp_plain=None) for i in range(8)]
                test_secs[0].update(data=dec_base[LFI_BASE : LFI_BASE + 512], scramble=True, exp_raw=None, exp_plain=dec_base[LFI_BASE : LFI_BASE + 512])
                fl.write_block(SCRATCH, test_secs)
                fl.erase_4k(SCRATCH)
                if fl.read_flash(SCRATCH, 0x1000, False) != b"\xFF" * 0x1000:
                    raise RuntimeError("Scratch block did not erase back to 0xFF")

            print(f"[INSTALL] Step 4/4: Flashing {len(blocks)} changed 4 KB blocks...", flush=True)
            for idx, (b, secs) in enumerate(blocks, 1):
                fl.write_block(b, secs)
                print(f"  [{idx:02d}/{len(blocks):02d}] block 0x{b:06x} OK", flush=True)

            print("[INSTALL] Running full LFI read-back + BREC tail verification...", flush=True)
            if fl.read_flash(LFI_BASE, LFI_END - LFI_BASE, True) != new_lfi:
                raise RuntimeError("Full LFI read-back mismatch!")
            if fl.read_flash(REGION_START, LFI_BASE - REGION_START, False) != raw_base[REGION_START:LFI_BASE]:
                raise RuntimeError("BREC tail mismatch!")
            print("[INSTALL] SUCCESS — TZX Mode installed and verified!", flush=True)
            ok = True

        elif args.action == "restore":
            raw_stock, _ = verify_backup_dir(args.backup_dir)
            live = fl.read_flash(REGION_START, REGION_END - REGION_START, False)
            bad = [REGION_START + b for b in range(0, len(live), 0x1000)
                   if live[b : b + 0x1000] != raw_stock[REGION_START + b : REGION_START + b + 0x1000]]
            print(f"[RESTORE] {len(bad)} block(s) differ from factory backup {args.backup_dir}", flush=True)
            if bad and not args.yes:
                ans = input(f"Restore {len(bad)} x 4 KB blocks back to stock? [y/N] ").strip().lower()
                if ans not in ("y", "yes"):
                    print("Cancelled.")
                    ok = True
                    return
            for idx, b in enumerate(bad, 1):
                secs = [dict(addr=s, data=raw_stock[s : s + 512], scramble=False,
                             exp_raw=raw_stock[s : s + 512], exp_plain=None) for s in range(b, b + 0x1000, 512)]
                fl.write_block(b, secs)
                print(f"  [{idx:02d}/{len(bad):02d}] block 0x{b:06x} restored", flush=True)
            if fl.read_flash(REGION_START, REGION_END - REGION_START, False) != raw_stock[REGION_START:REGION_END]:
                raise RuntimeError("Restore verification mismatch!")
            print("[RESTORE] SUCCESS — flash is 100% bit-identical to backup!", flush=True)
            ok = True
    finally:
        fl.close(reboot=ok)
        if not ok:
            print("!!! Operation did not complete cleanly. Re-run restore with your backup folder.", file=sys.stderr)


if __name__ == "__main__":
    main()
