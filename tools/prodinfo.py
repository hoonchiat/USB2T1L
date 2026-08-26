#!/usr/bin/env python3
"""
Production-info tool for the STM32F407 ADIN2111 USB-ECM bridge.

The firmware reads a 128-byte production record from a reserved flash sector
(sector 11 @ 0x080E0000): MAC address, serial number, batch, firmware version,
hardware revision, production date, protected by a CRC-32 (byte-identical to
the firmware and to Python's zlib.crc32).

Read / write the adapter's MAC, serial and batch:

  write   inject a record straight to the device over SWD (build + flash + verify,
          in one step) -- the production-line command
  dump    read the record from device flash over SWD and decode it
  read    read the record from a live device over USB (needs pyusb)

Lower-level helpers:

  build   assemble a 128-byte record (with CRC) into a .bin file
  flash   write an already-built .bin to device flash over SWD
  decode  print / verify a record from a .bin file

Examples:
  # Provision a unit in one shot (over an ST-Link):
  ./prodinfo.py write --mac 02:11:22:33:44:55 --serial SN0001 --batch B2026-08

  # Update just the serial, keep the MAC/batch/etc. already on the device:
  ./prodinfo.py write --keep --serial SN0002

  # Read what is currently on the device:
  ./prodinfo.py dump                 # over SWD (ST-Link), no firmware needed
  ./prodinfo.py read                 # over USB, from the running firmware

  # Offline file workflow:
  ./prodinfo.py build --mac 02:11:22:33:44:55 --serial SN0001 --out unit0001.bin
  ./prodinfo.py flash unit0001.bin
  ./prodinfo.py decode unit0001.bin
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
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


# ---------------------------------------------------------------------------
# Record encode / decode
# ---------------------------------------------------------------------------
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


def record_valid(info):
    return info["magic_ok"] and info["crc_ok"] and info["version"] == VERSION


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
    if not record_valid(info):
        print("  !! record is INVALID — the firmware will fall back to the UID MAC")


def resolve_fields(args, base):
    """Compute the final record fields for `write`.

    Any field not given on the command line is inherited from `base` (a decoded
    record dict, e.g. read back from the device with --keep) when present, else
    left empty. The MAC must resolve to something: it comes from --mac or, with
    --keep, from the existing on-device record.
    """
    def pick(val, key):
        if val is not None:
            return val
        if base is not None:
            return base[key]
        return ""

    mac = args.mac if args.mac is not None else (base["mac"] if base else None)
    if not mac:
        sys.exit("no MAC given and none found on the device — pass --mac")
    return {
        "mac": mac,
        "serial": pick(args.serial, "serial"),
        "batch": pick(args.batch, "batch"),
        "fwver": pick(args.fwver, "fw_version"),
        "hwrev": pick(args.hwrev, "hw_rev"),
        "date": pick(args.date, "prod_date"),
    }


# ---------------------------------------------------------------------------
# SWD access (st-flash / OpenOCD)
# ---------------------------------------------------------------------------
def _run(cmd):
    print("running:", " ".join(cmd))
    try:
        subprocess.run(cmd, check=True)
    except FileNotFoundError:
        sys.exit(f"{cmd[0]} not found — install it, or select the other "
                 f"programmer with --programmer.")
    except subprocess.CalledProcessError as e:
        sys.exit(f"{cmd[0]} failed (exit {e.returncode}).")


def swd_write(path, a):
    addr = f"0x{FLASH_ADDR:08X}"
    if a.programmer == "openocd":
        cmd = ["openocd", "-f", a.ocd_interface, "-f", a.ocd_target,
               "-c", f"program {path} {addr} verify reset exit"]
    else:
        cmd = [a.st_cmd, "--reset", "write", path, addr]
    _run(cmd)


def swd_read(a):
    """Read the 128-byte record from device flash over SWD; return raw bytes."""
    addr = f"0x{FLASH_ADDR:08X}"
    fd, tmp = tempfile.mkstemp(suffix=".bin")
    os.close(fd)
    try:
        if a.programmer == "openocd":
            cmd = ["openocd", "-f", a.ocd_interface, "-f", a.ocd_target,
                   "-c", "init", "-c", "reset halt",
                   "-c", f"dump_image {tmp} {addr} {SIZE}",
                   "-c", "reset run", "-c", "exit"]
        else:
            cmd = [a.st_cmd, "read", tmp, addr, str(SIZE)]
        _run(cmd)
        with open(tmp, "rb") as f:
            data = f.read()
        if len(data) < SIZE:
            sys.exit(f"read back only {len(data)} bytes, expected {SIZE}")
        return data[:SIZE]
    finally:
        os.unlink(tmp)


# ---------------------------------------------------------------------------
# Subcommands
# ---------------------------------------------------------------------------
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
    swd_write(a.file, a)


def cmd_dump(a):
    _print(decode_record(swd_read(a)))


def cmd_write(a):
    base = None
    if a.keep:
        print("reading current record over SWD (to keep unspecified fields)...")
        try:
            existing = decode_record(swd_read(a))
        except Exception as e:                       # noqa: BLE001
            sys.exit(f"could not read the existing record: {e}")
        if record_valid(existing):
            base = existing
        else:
            print("  (no valid existing record — nothing to inherit)")

    fields = resolve_fields(a, base)
    rec = build_record(parse_mac(fields["mac"]), fields["serial"],
                       fields["batch"], fields["fwver"], fields["hwrev"],
                       fields["date"])
    print("record to write:")
    _print(decode_record(rec))
    if a.dry_run:
        print("dry-run: not flashing.")
        return

    fd, tmp = tempfile.mkstemp(suffix=".bin")
    try:
        os.write(fd, rec)
        os.close(fd)
        swd_write(tmp, a)
    finally:
        os.unlink(tmp)

    if a.no_verify:
        return
    print("verifying (reading the record back over SWD)...")
    got = swd_read(a)
    _print(decode_record(got))
    if got != rec:
        sys.exit("VERIFY FAILED — flash content does not match what was written.")
    print("verify OK — device record matches.")


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


# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
def add_swd_args(sp):
    sp.add_argument("--programmer", choices=["st", "openocd"], default="st",
                    help="SWD tool to use (default: st = st-flash / st-info)")
    sp.add_argument("--st-cmd", default="st-flash",
                    help="st-flash binary name/path (default: st-flash)")
    sp.add_argument("--ocd-interface", default="interface/stlink.cfg",
                    help="OpenOCD interface cfg (default: interface/stlink.cfg)")
    sp.add_argument("--ocd-target", default="target/stm32f4x.cfg",
                    help="OpenOCD target cfg (default: target/stm32f4x.cfg)")


def _add_record_args(sp, required_mac):
    sp.add_argument("--mac", required=required_mac, default=None,
                    help="unicast MAC, e.g. 02:11:22:33:44:55")
    sp.add_argument("--serial", default=None, help="serial number (<=24 chars)")
    sp.add_argument("--batch", default=None, help="production batch (<=16 chars)")
    sp.add_argument("--fwver", default=None, help="firmware version (<=16 chars)")
    sp.add_argument("--hwrev", default=None, help="hardware revision (<=8 chars)")
    sp.add_argument("--date", default=None, help="production date (<=12 chars)")


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    # write: build + flash + verify over SWD (the production command)
    w = sub.add_parser("write",
                       help="inject MAC/serial/batch to the device over SWD")
    _add_record_args(w, required_mac=False)
    w.add_argument("--keep", action="store_true",
                   help="read the current record first and keep any field you "
                        "do not pass (read-modify-write)")
    w.add_argument("--no-verify", action="store_true",
                   help="do not read the record back to verify after writing")
    w.add_argument("--dry-run", action="store_true",
                   help="build and show the record, but do not flash")
    add_swd_args(w)
    w.set_defaults(func=cmd_write)

    # dump: read the record from flash over SWD
    du = sub.add_parser("dump", help="read + decode the record from flash over SWD")
    add_swd_args(du)
    du.set_defaults(func=cmd_dump)

    # read: read the record over USB from the running firmware
    r = sub.add_parser("read", help="read + decode from a live USB device")
    r.add_argument("--vid", type=lambda x: int(x, 0), default=DEFAULT_VID)
    r.add_argument("--pid", type=lambda x: int(x, 0), default=DEFAULT_PID)
    r.set_defaults(func=cmd_read)

    # build: assemble a record into a .bin (offline)
    b = sub.add_parser("build", help="assemble a record into a .bin file")
    b.add_argument("--mac", required=True)
    b.add_argument("--serial", default="")
    b.add_argument("--batch", default="")
    b.add_argument("--fwver", default="")
    b.add_argument("--hwrev", default="")
    b.add_argument("--date", default="")
    b.add_argument("--out", required=True)
    b.set_defaults(func=cmd_build)

    # flash: write an already-built .bin over SWD
    f = sub.add_parser("flash", help="write a .bin record to device flash over SWD")
    f.add_argument("file")
    add_swd_args(f)
    f.set_defaults(func=cmd_flash)

    # decode: print/verify a .bin
    d = sub.add_parser("decode", help="print/verify a .bin record")
    d.add_argument("file")
    d.set_defaults(func=cmd_decode)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
