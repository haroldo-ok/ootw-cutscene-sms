#!/usr/bin/env python3
"""Disassembler for Another World bytecode (per rawgl script.cpp)."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from aw_data import AwData, TYPE_NAMES, MEMLIST_PARTS

OPS = {
    0x00: ("movConst", "vB cW"), 0x01: ("mov", "vB vB"), 0x02: ("add", "vB vB"),
    0x03: ("addConst", "vB cW"), 0x04: ("call", "oW"), 0x05: ("ret", ""),
    0x06: ("yield", ""), 0x07: ("jmp", "oW"), 0x08: ("installTask", "tB oW"),
    0x09: ("jmpIfVar", "vB oW"), 0x0A: ("condJmp", "opB vB a? oW"),
    0x0B: ("setPalette", "pW"), 0x0C: ("changeTasksState", "sB eB stB"),
    0x0D: ("selectPage", "pB"), 0x0E: ("fillPage", "pB cB"),
    0x0F: ("copyPage", "pB pB"), 0x10: ("updateDisplay", "pB"),
    0x11: ("removeTask", ""), 0x12: ("drawString", "sW xB yB cB"),
    0x13: ("sub", "vB vB"), 0x14: ("and", "vB cW"), 0x15: ("or", "vB cW"),
    0x16: ("shl", "vB cW"), 0x17: ("shr", "vB cW"),
    0x18: ("playSound", "rW fB vB chB"),
    0x19: ("updateResources", "rW"), 0x1A: ("playMusic", "rW delayW posB"),
}


class Dis:
    def __init__(self, code):
        self.code = code
        self.pc = 0

    def u8(self):
        v = self.code[self.pc]
        self.pc += 1
        return v

    def s16(self):
        v = struct.unpack(">h", self.code[self.pc:self.pc + 2])[0]
        self.pc += 2
        return v

    def u16(self):
        v = struct.unpack(">H", self.code[self.pc:self.pc + 2])[0]
        self.pc += 2
        return v

    def one(self):
        start = self.pc
        op = self.u8()
        txt = ""
        if op & 0x80:
            off = ((op << 8) | self.u8()) << 1
            x = self.u8()
            y = self.u8()
            txt = f"drawShape1(off=0x{off:04X} x={x} y={y})"
        elif op & 0x40:
            offset_hi = self.u8()
            off = ((offset_hi << 8) | self.u8()) << 1
            x = self.u8()
            xs = ""
            if not (op & 0x20):
                if not (op & 0x10):
                    x = (x << 8) | self.u8()
                else:
                    xs = f"VAR(0x{x:02X})"
            else:
                if op & 0x10:
                    x += 0x100
            y = self.u8()
            ys = ""
            if not (op & 8):
                if not (op & 4):
                    y = (y << 8) | self.u8()
                else:
                    ys = f"VAR(0x{y:02X})"
            zoom = "64"
            if not (op & 2):
                if op & 1:
                    zoom = f"VAR(0x{self.u8():02X})"
            else:
                if op & 1:
                    zoom = "useSegVideo2"
                else:
                    zoom = str(self.u8())
            xdesc = xs if xs else str(x)
            ydesc = ys if ys else str(y)
            txt = f"drawShape2(op=0x{op:02X} off=0x{off:04X} x={xdesc} y={ydesc} zoom={zoom})"
        elif op <= 0x1A:
            name, args = OPS[op]
            parts = []
            for a in args.split():
                if a == "vB":
                    parts.append(f"VAR(0x{self.u8():02X})")
                elif a == "cB":
                    parts.append(str(self.u8()))
                elif a == "pB":
                    parts.append(f"page=0x{self.u8():02X}")
                elif a == "tB":
                    parts.append(f"task={self.u8()}")
                elif a == "sB":
                    parts.append(f"start={self.u8()}")
                elif a == "eB":
                    parts.append(f"end={self.u8()}")
                elif a == "stB":
                    parts.append(f"state={self.u8()}")
                elif a == "xB":
                    parts.append(f"x={self.u8()}")
                elif a == "yB":
                    parts.append(f"y={self.u8()}")
                elif a == "fB":
                    parts.append(f"freq={self.u8()}")
                elif a == "chB":
                    parts.append(f"ch={self.u8()}")
                elif a == "posB":
                    parts.append(f"pos={self.u8()}")
                elif a == "cW":
                    parts.append(str(self.s16()))
                elif a == "oW":
                    parts.append(f"@0x{self.u16():04X}")
                elif a == "rW":
                    parts.append(f"res=0x{self.u16():04X}")
                elif a == "delayW":
                    parts.append(f"delay={self.u16()}")
                elif a == "pW":
                    parts.append(f"pal={self.u16()}")
                elif a == "sW":
                    parts.append(f"str=0x{self.u16():03X}")
                elif a == "opB":
                    parts.append(f"op=0x{self.u8():02X}")
                elif a == "a?":
                    oplen = self.code[start + 2] if self.pc == start + 2 else None
                else:
                    raise ValueError(a)
            # handle condJmp arg
            if name == "condJmp":
                # re-parse: opcode byte at start+2
                # already consumed op and var; figure arg size from op bits
                # we need the op value: recompute
                pass
            txt = f"{name}({', '.join(parts)})"
        else:
            txt = f"INVALID(0x{op:02X})"
        return start, txt

    def one_cond(self):
        """condJmp needs custom handling for the 'a' operand size."""
        start = self.pc
        op = self.u8()
        assert op == 0x0A
        cond = self.u8()
        var = self.u8()
        if cond & 0x80:
            a = f"VAR(0x{self.u8():02X})"
        elif cond & 0x40:
            a = str(self.s16())
        else:
            a = str(self.u8())
        dest = f"@0x{self.u16():04X}"
        cname = {0: "==", 1: "!=", 2: ">", 3: ">=", 4: "<", 5: "<="}.get(cond & 7, "?")
        return start, f"condJmp(VAR(0x{var:02X}) {cname} {a}, {dest})"

    def disasm(self):
        lines = []
        self.pc = 0
        while self.pc < len(self.code):
            if self.code[self.pc] == 0x0A:
                s, t = self.one_cond()
            else:
                try:
                    s, t = self.one()
                except Exception as ex:
                    s, t = self.pc, f"ERROR {ex}"
                    break
            lines.append((s, t))
        return lines


def main():
    data = AwData(sys.argv[1] if len(sys.argv) > 1 else "../work/game_files")
    outdir = "../work/disasm"
    os.makedirs(outdir, exist_ok=True)
    for i, (ipal, icod, ivd1, ivd2) in enumerate(MEMLIST_PARTS):
        part = 16000 + i
        code = data.load(icod)
        d = Dis(code)
        lines = d.disasm()
        fn = os.path.join(outdir, f"part{part}.asm")
        with open(fn, "w") as f:
            f.write(f"; part {part} code res #{icod} ({len(code)} bytes), "
                    f"pal #{ipal}, video #{ivd1}{'/'+str(ivd2) if ivd2 else ''}\n")
            for s, t in lines:
                f.write(f"{s:04X}: {t}\n")
        print(f"part {part}: {len(code)} bytes -> {len(lines)} instructions -> {fn}")


if __name__ == "__main__":
    main()
