#!/usr/bin/env python3
"""
Production info tool for the STM32F407 ADIN2111 USB-ECM bridge.

The firmware reads a 128-byte production record from a reserved flash sector
(sector 11 @ 0x080E0000): MAC address, serial number, batch, firmware version,
hardware revision, production date, protected by a CRC-32 (matches the firmware
and Python's zlib.crc32).

Subcommands:
  build   assemble the 128-byte record (with CRC) into a .bin file
  flash   write a record to the device flash over SWD (st-flash / OpenOCD)
  decode  print/verify a record from a .bin file
  read    read + decode the record from a live device over USB (needs pyusb)

Examples:
  ./prodinfo.py build --mac 02:11:22:33:44:55 --serial SN0001 \\
                --batch B2026-08 --fwver v0.1 --hwrev A1 --out unit0001.bin
  ./prodinfo.py flash unit0001.bin           # st-flash write ... 0x080E0000
  ./prodinfo.py decode unit0001.bin
  ./prodinfo.py read                          # from the plugged-in device
"""
import argparse
import struct
import subprocess
import sys
import zlib

FLASH_ADDR = 0x080E0000
MAGIC = 0x444F5250              # 'PROD'
VERSION = 1
SIZE = 128
VENDOR_GET_PRODINFO = 0x50     # vendor control request (must match firmware)
DEFAULT_VID = 0x0483
DEFAULT_PID = 0xA111

# <  little-endian, packed (matches the __attribute__((packed)) C struct)
# I magic, B version, B flags, H size, 6s mac, 2s rsv0, 24s serial, 16s batch,
# 16s fw_version, 8s hw_rev, 12s prod_date, 32s rsv1  == 124 bytes, then I crc
_PREFIX_FMT = "<IBBH6s2s24s16s16s8s12s32s"
assert struct.calcsize(_PREFIX_FMT) == SIZE - 4


def parse_mac(s):
    parts = s.replace("-", ":").split(":")
    if len(parts) != 6:
        raise ValueError("MAC must be 6 octets, e.g. 02:11:22:33:44:55")
    b = bytes(int(p, 16) for p in parts)
    if b[0] & 0x01:
        raise ValueError("first octet has the multicast bit set; not a unicast MAC")
    return b


def _field(s, n):
    data = s.encode("ascii")
    if len(data) > n:
        raise ValueError(f"'{s}' is longer than {n} bytes")
    return data.ljust(n, b"\x00")


def build_record(mac, serial, batch, fwver, hwrev, date):
    prefix = struct.pack(
        _PREFIX_FMT, MAGIC, VERSION, 0, SIZE, mac, b"\x00\x00",
        _field(serial, 24), _field(batch, 16), _field(fwver, 16),
        _field(hwrev, 8), _field(date, 12), b"\x00" * 32)
    crc = zlib.crc32(prefix) & 0xFFFFFFFF
    return prefix + struct.pack("<I", crc)


def decode_record(raw):
    if len(raw) < SIZE:
        raise ValueError(f"record is {len(raw)} bytes, expected {SIZE}")
    raw = raw[:SIZE]
    (magic, version, flags, size, mac, _rsv0, serial, batch, fwver, hwrev,
     date, _rsv1) = struct.unpack(_PREFIX_FMT, raw[:SIZE - 4])
    (crc,) = struct.unpack("<I", raw[SIZE - 4:])
    calc = zlib.crc32(raw[:SIZE - 4]) & 0xFFFFFFFF
    txt = lambda b: b.split(b"\x00", 1)[0].decode("ascii", "replace")
    return {
        "magic_ok": magic == MAGIC,
        "version": version,
        "size": size,
        "mac": ":".join(f"{x:02x}" for x in mac),
        "serial": txt(serial),
        "batch": txt(batch),
        "fw_version": txt(fwver),
        "hw_rev": txt(hwrev),
        "prod_date": txt(date),
        "crc": crc,
        "crc_ok": crc == calc,
    }


def _print(info):
    print(f"  magic     : {'OK' if info['magic_ok'] else 'BAD'}")
    print(f"  version   : {info['version']}")
    print(f"  MAC       : {info['mac']}")
    print(f"  serial    : {info['serial']}")
    print(f"  batch     : {info['batch']}")
    print(f"  fw_version: {info['fw_version']}")
    print(f"  hw_rev    : {info['hw_rev']}")
    print(f"  prod_date : {info['prod_date']}")
    print(f"  crc       : {info['crc']:08X} {'(OK)' if info['crc_ok'] else '(BAD)'}")
    if not (info["magic_ok"] and info["crc_ok"] and info["version"] == VERSION):
        print("  !! record is INVALID — the firmware will fall back to the UID MAC")


def cmd_build(a):
    rec = build_record(parse_mac(a.mac), a.serial, a.batch, a.fwver,
                       a.hwrev, a.date)
    with open(a.out, "wb") as f:
        f.write(rec)
    print(f"wrote {a.out} ({len(rec)} bytes)")
    _print(decode_record(rec))


def cmd_decode(a):
    with open(a.file, "rb") as f:
        _print(decode_record(f.read()))


def cmd_flash(a):
    addr = f"0x{FLASH_ADDR:08X}"
    print(f"OpenOCD alternative:\n"
          f"  openocd -f interface/stlink.cfg -f target/stm32f4x.cfg "
          f"-c \"program {a.file} {addr} verify reset exit\"\n")
    cmd = ["st-flash", "--reset", "write", a.file, addr]
    print("running:", " ".join(cmd))
    try:
        subprocess.run(cmd, check=True)
    except FileNotFoundError:
        sys.exit("st-flash not found — install stlink tools or use the OpenOCD "
                 "command above.")


def cmd_read(a):
    try:
        import usb.core
    except ImportError:
        sys.exit("pyusb not installed (pip install pyusb)")
    dev = usb.core.find(idVendor=a.vid, idProduct=a.pid)
    if dev is None:
        sys.exit(f"device {a.vid:04x}:{a.pid:04x} not found")
    # bmRequestType: IN | Vendor | Device = 0xC0
    raw = dev.ctrl_transfer(0xC0, VENDOR_GET_PRODINFO, 0, 0, SIZE)
    _print(decode_record(bytes(raw)))


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build", help="assemble a record into a .bin")
    b.add_argument("--mac", required=True)
    b.add_argument("--serial", default="")
    b.add_argument("--batch", default="")
    b.add_argument("--fwver", default="")
    b.add_argument("--hwrev", default="")
    b.add_argument("--date", default="")
    b.add_argument("--out", required=True)
    b.set_defaults(func=cmd_build)

    f = sub.add_parser("flash", help="write a record to device flash over SWD")
    f.add_argument("file")
    f.set_defaults(func=cmd_flash)

    d = sub.add_parser("decode", help="print/verify a .bin record")
    d.add_argument("file")
    d.set_defaults(func=cmd_decode)

    r = sub.add_parser("read", help="read + decode from a live USB device")
    r.add_argument("--vid", type=lambda x: int(x, 0), default=DEFAULT_VID)
    r.add_argument("--pid", type=lambda x: int(x, 0), default=DEFAULT_PID)
    r.set_defaults(func=cmd_read)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
