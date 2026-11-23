#!/usr/bin/env python3
"""
Read-only USB Mass Storage + Actions Vendor SCSI probe for 10d6:1101 devices.
Performs strictly passive IN queries (GET_MAX_LUN, INQUIRY, 0xCC IsActions, 0xDD GetPhilipsInfo).
"""
import ctypes
import struct
import sys
from pathlib import Path


def hexdump(data: bytes, max_bytes: int = 512) -> str:
    lines = []
    for off in range(0, min(len(data), max_bytes), 16):
        chunk = data[off : off + 16]
        hex_part = " ".join(f"{b:02x}" for b in chunk)
        asc_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"  {off:04x}: {hex_part:<47s} |{asc_part}|")
    return "\n".join(lines)


def main() -> None:
    lib = ctypes.CDLL("libusb-1.0.so.0")
    lib.libusb_init.argtypes = [ctypes.c_void_p]
    lib.libusb_open_device_with_vid_pid.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint16]
    lib.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p
    lib.libusb_kernel_driver_active.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.libusb_detach_kernel_driver.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.libusb_attach_kernel_driver.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.libusb_claim_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.libusb_release_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.libusb_control_transfer.argtypes = [
        ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint8,
        ctypes.c_uint16, ctypes.c_uint16, ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint,
    ]
    lib.libusb_bulk_transfer.argtypes = [
        ctypes.c_void_p, ctypes.c_uint8, ctypes.c_void_p,
        ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.c_uint,
    ]
    lib.libusb_clear_halt.argtypes = [ctypes.c_void_p, ctypes.c_uint8]
    lib.libusb_close.argtypes = [ctypes.c_void_p]

    lib.libusb_init(None)
    h = lib.libusb_open_device_with_vid_pid(None, 0x10D6, 0x1101)
    if not h:
        sys.exit("Device 10d6:1101 not found on USB bus.")

    was_active = lib.libusb_kernel_driver_active(h, 0)
    if was_active == 1:
        lib.libusb_detach_kernel_driver(h, 0)
    lib.libusb_claim_interface(h, 0)

    try:
        cbuf = ctypes.create_string_buffer(8)
        r = lib.libusb_control_transfer(h, 0xA1, 0xFE, 0, 0, cbuf, 1, 1000)
        print(f"GET_MAX_LUN: ret={r}, max_lun={cbuf.raw[0] if r == 1 else 'N/A'}")

        tag = 100

        def query_in(cdb: bytes, dlen: int, lun: int = 0):
            nonlocal tag
            tag += 1
            transferred = ctypes.c_int(0)
            cbw = struct.pack(
                "<IIIBBB16s",
                0x43425355,
                tag,
                dlen,
                0x80,
                lun,
                len(cdb),
                cdb.ljust(16, b"\x00"),
            )
            lib.libusb_bulk_transfer(h, 0x02, cbw, 31, ctypes.byref(transferred), 1000)
            dbuf = ctypes.create_string_buffer(dlen)
            rc_d = lib.libusb_bulk_transfer(h, 0x81, dbuf, dlen, ctypes.byref(transferred), 1500)
            got = dbuf.raw[: transferred.value] if rc_d == 0 else b""
            if rc_d != 0:
                lib.libusb_clear_halt(h, 0x81)
            csw = ctypes.create_string_buffer(13)
            rc_s = lib.libusb_bulk_transfer(h, 0x81, csw, 13, ctypes.byref(transferred), 1000)
            if rc_s != 0:
                lib.libusb_clear_halt(h, 0x81)
                lib.libusb_bulk_transfer(h, 0x81, csw, 13, ctypes.byref(transferred), 1000)
            return rc_d, got, csw.raw.hex()

        rc, inq, csw = query_in(bytes([0x12, 0, 0, 0, 36, 0]), 36)
        print(f"SCSI INQUIRY (0x12): rc={rc} csw={csw}\n{hexdump(inq)}")

        rc, act11, csw = query_in(bytes([0xCC, 0, 0, 0, 0, 0, 0, 11, 0])[:16], 11)
        print(f"Vendor IsActions 0xCC (11B): rc={rc} ascii={act11!r} csw={csw}")

        rc, act20, csw = query_in(bytes([0xCC, 0, 0, 0, 0, 0, 0, 20, 0])[:16], 20)
        print(f"Vendor IsActions 0xCC (20B): rc={rc} csw={csw}\n{hexdump(act20)}")

        rc, phil, csw = query_in(bytes([0xDD, 0, 0, 0, 0, 0, 0, 0x08, 0x00])[:16], 2048)
        print(f"Vendor GetPhilipsInfo 0xDD (2048B): rc={rc} len={len(phil)} csw={csw}")
        if phil:
            out_file = Path(sys.argv[1]) if len(sys.argv) > 1 else None
            if out_file:
                out_file.parent.mkdir(parents=True, exist_ok=True)
                out_file.write_bytes(phil)
                print(f"Saved {len(phil)} bytes to {out_file}")
            print(hexdump(phil, 512))
    finally:
        lib.libusb_release_interface(h, 0)
        if was_active == 1:
            lib.libusb_attach_kernel_driver(h, 0)
        lib.libusb_close(h)


if __name__ == "__main__":
    main()
