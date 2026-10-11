#!/usr/bin/env python3
"""Write a DScheevos session file (session.bin) from saved server replies.

The DScheevos launcher writes this file after going online; this tool makes
the same file offline for tests (mock server replies, test sets).

  mksession.py --rom-path sd:/games/kirby.nds --hash <md5> --set set.json \
      [--login login.json] [--session start.json] [--softcore] -o session.bin

Layout: dscm_boot_t (see dscheevos/module/include/dscheevos_module.h), then
the login2, achievementsets and startsession replies, 4-byte aligned.
"""
import argparse
import struct
import sys
import time

MAGIC = 0x53435344          # "DSCS"
BOOT_SIZE = 496             # sizeof(dscm_boot_t); checked by layout.c in CI
SESSION_MAX = 0x60000

DEFAULT_LOGIN = b'{"Success":true,"User":"tester","Token":"tok","Score":0,"SoftcoreScore":0,"Messages":0}'
DEFAULT_SESSION = b'{"Success":true,"Unlocks":[],"HardcoreUnlocks":[],"ServerNow":%d}'


def fixed(text, size):
    data = text.encode()
    if len(data) >= size:
        sys.exit(f"value too long for a {size}-byte field: {text!r}")
    return data + b"\0" * (size - len(data))


def build(args):
    replies = [
        open(args.login, "rb").read() if args.login else DEFAULT_LOGIN,
        open(args.set, "rb").read(),
        open(args.session, "rb").read() if args.session else DEFAULT_SESSION % int(time.time()),
    ]
    offsets, lengths, body = [], [], b""
    offset = BOOT_SIZE
    for reply in replies:
        offsets.append(offset)
        lengths.append(len(reply))
        pad = (-len(reply)) % 4
        body += reply + b"\0" * pad
        offset += len(reply) + pad
    file_size = offset
    if file_size > SESSION_MAX:
        sys.exit(f"session file too big: {file_size} > {SESSION_MAX}")

    head = struct.pack("<III", MAGIC, BOOT_SIZE, file_size)
    head += fixed(args.rom_path, 256)
    head += fixed(args.host, 64) + fixed(args.user, 32) + fixed(args.token, 64)
    head += fixed(args.hash.lower(), 33)
    head += struct.pack("<B2x", 0 if args.softcore else 1)  # hardcore, reserved[2]
    head += struct.pack("<II", 0, int(args.unix_time or time.time()))
    head += struct.pack("<3I", *offsets) + struct.pack("<3I", *lengths)
    assert len(head) == BOOT_SIZE, len(head)
    return head + body


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom-path", required=True, help="NDS_PATH as nds-bootstrap.ini has it")
    ap.add_argument("--hash", required=True, help="RetroAchievements hash of the ROM")
    ap.add_argument("--set", required=True, help="achievementsets reply (JSON)")
    ap.add_argument("--login", help="login2 reply (JSON)")
    ap.add_argument("--session", help="startsession reply (JSON)")
    ap.add_argument("--host", default="http://10.64.0.1:8080")
    ap.add_argument("--user", default="tester")
    ap.add_argument("--token", default="tok")
    ap.add_argument("--softcore", action="store_true")
    ap.add_argument("--unix-time", type=int)
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()
    data = build(args)
    with open(args.output, "wb") as f:
        f.write(data)
    print(f"wrote {args.output}: {len(data)} bytes")


if __name__ == "__main__":
    main()
