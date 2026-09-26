#!/usr/bin/env python3
"""Dump all Another World resources for inspection."""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from aw_data import AwData, TYPE_NAMES, MEMLIST_PARTS

def main():
    data = AwData(sys.argv[1] if len(sys.argv) > 1 else "../work/game_files")
    out = sys.argv[2] if len(sys.argv) > 2 else "../work/dumped"
    os.makedirs(out, exist_ok=True)
    print(f"{len(data.entries)} memlist entries")
    for e in data.entries:
        if e.status == 0xFF:
            print("terminator")
            break
        if e.unpacked == 0:
            continue
        blob = data.load(e.num)
        name = f"{e.num:03d}_{TYPE_NAMES.get(e.type, e.type)}_{e.unpacked}"
        open(os.path.join(out, name), "wb").write(blob)
        print(f"#{e.num:3d} {TYPE_NAMES.get(e.type,'?'):8s} bank={e.bank:02x} "
              f"pos={e.pos:06x} packed={e.packed:6d} unpacked={e.unpacked:6d}")
    print("\nGame parts:")
    for i, (ipal, icod, ivd1, ivd2) in enumerate(MEMLIST_PARTS):
        parts = [ipal, icod, ivd1] + ([ivd2] if ivd2 else [])
        desc = ",".join(f"#{n}:{TYPE_NAMES.get(data.entries[n].type,'?')}" for n in parts)
        print(f"  160{i}0  {desc}")

if __name__ == "__main__":
    main()
