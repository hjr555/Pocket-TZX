#!/usr/bin/env python3
"""
Synchronize the on-chip RTC of an Actions Semiconductor (10d6:1101) voice recorder
using the reverse-engineered vendor SCSI command 0xB0 (set_time_and_date).

REA Ghidra Evidence: ev_8e32a99851f578ab781ac8f05014c3f80c2d3ee83b1a67a14dfaf938ad299069
Payload (7 bytes, DATA-OUT): <uint16_le year><u8 month><u8 day><u8 hour><u8 min><u8 sec>
"""
import ctypes
from datetime import datetime
import struct
import sys


def sync_rtc(dt: datetime) -> None:
    lib = ctypes.CDLL("libusb-1.0.so.0")
    lib.libusb_init.argtypes = [ctypes.c_void_p]
    lib.libusb_open_device_with_vid_pid.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint16]
    lib.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p
    lib.libusb_kernel_driver_active.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.libusb_detach_kernel_driver.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.libusb_attach_kernel_driver.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.libusb_claim_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.libusb_release_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.libusb_bulk_transfer.argtypes = [
        ctypes.c_void_p, ctypes.c_uint8, ctypes.c_void_p,
        ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.c_uint,
    ]
    lib.libusb_close.argtypes = [ctypes.c_void_p]

    lib.libusb_init(None)
    h = lib.libusb_open_device_with_vid_pid(None, 0x10D6, 0x1101)
    if not h:
        sys.exit("Device 10d6:1101 not found.")

    was_active = lib.libusb_kernel_driver_active(h, 0)
    if was_active == 1:
        lib.libusb_detach_kernel_driver(h, 0)
    lib.libusb_claim_interface(h, 0)

    try:
        payload = struct.pack("<HBBBBB", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second)
        cdb = bytes([0xB0, 0, 0, 0, 0, 0, 0, 0, len(payload)]) + b"\x00" * 7
        cbw = struct.pack("<IIIBBB16s", 0x43425355, 1, len(payload), 0x00, 0, 16, cdb)

        transferred = ctypes.c_int(0)
        rc1 = lib.libusb_bulk_transfer(h, 0x02, cbw, 31, ctypes.byref(transferred), 1000)
        rc2 = lib.libusb_bulk_transfer(h, 0x02, payload, len(payload), ctypes.byref(transferred), 1000)
        csw = ctypes.create_string_buffer(13)
        rc3 = lib.libusb_bulk_transfer(h, 0x81, csw, 13, ctypes.byref(transferred), 1000)
        sig, rtag, res, status = struct.unpack("<IIIB", csw.raw)
        print(f"RTC sync -> {dt.isoformat(sep=' ', timespec='seconds')} | rc=({rc1},{rc2},{rc3}) CSW status={status}")
    finally:
        lib.libusb_release_interface(h, 0)
        if was_active == 1:
            lib.libusb_attach_kernel_driver(h, 0)
        lib.libusb_close(h)


if __name__ == "__main__":
    sync_rtc(datetime.now())
