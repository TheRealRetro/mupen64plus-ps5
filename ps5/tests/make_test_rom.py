#!/usr/bin/env python3
# Mupen64Plus PS5: builds a tiny N64 test ROM (no Nintendo code in it) for the host build.
#
# mupen64plus-core's HLE boot stands in for the PIF: it copies the cartridge's boot code (IPL3, ROM 0x40) to
# the RSP's memory and jumps to it. Instead of Nintendo's IPL3 this ROM carries a stub of its own that copies
# the program (ROM 0x1000) to RDRAM 0x400 and jumps there. The program drives every part of the port:
#   - the CPU (cached interpreter) draws eight colour bars into a 320x240 RGBA5551 frame buffer and programs
#     the VI (angrylion's VI emulation -> the frontend's scaler -> VideoOut);
#   - it sends the RDP a command list from RDRAM (set colour image, scissor, fill mode, fill colour, a red
#     100x100 rectangle at (40,40), sync full): angrylion's rasteriser;
#   - it keeps the AI fed with a 440 Hz square wave at 32 kHz: the audio plugin and AudioOut.
#
#   make_test_rom.py out.z64
#
# SPDX-License-Identifier: MIT
import struct
import sys

T0, T1, T2, T3, T4, T5, T6, T7, T8 = 8, 9, 10, 11, 12, 13, 14, 15, 24
ZERO = 0

CODE_ROM = 0x1000  # what the HLE boot copies to RDRAM 0x400
ENTRY = 0x80000400


def phys(rom_off):
    return rom_off - CODE_ROM + 0x400


class Asm:
    def __init__(self, base):
        self.base = base
        self.words = []
        self.labels = {}
        self.fixups = []  # (index, kind, label)

    def pc(self):
        return self.base + 4 * len(self.words)

    def label(self, name):
        self.labels[name] = self.pc()

    def emit(self, w):
        self.words.append(w & 0xFFFFFFFF)

    def i(self, op, rs, rt, imm):
        self.emit((op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF))

    def r(self, rs, rt, rd, sa, funct):
        self.emit((rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct)

    def nop(self):
        self.emit(0)

    def lui(self, rt, imm):
        self.i(0x0F, 0, rt, imm)

    def ori(self, rt, rs, imm):
        self.i(0x0D, rs, rt, imm)

    def andi(self, rt, rs, imm):
        self.i(0x0C, rs, rt, imm)

    def addiu(self, rt, rs, imm):
        self.i(0x09, rs, rt, imm)

    def slti(self, rt, rs, imm):
        self.i(0x0A, rs, rt, imm)

    def lw(self, rt, off, base):
        self.i(0x23, base, rt, off)

    def lhu(self, rt, off, base):
        self.i(0x25, base, rt, off)

    def sw(self, rt, off, base):
        self.i(0x2B, base, rt, off)

    def sh(self, rt, off, base):
        self.i(0x29, base, rt, off)

    def sll(self, rd, rt, sa):
        self.r(0, rt, rd, sa, 0x00)

    def srl(self, rd, rt, sa):
        self.r(0, rt, rd, sa, 0x02)

    def addu(self, rd, rs, rt):
        self.r(rs, rt, rd, 0, 0x21)

    def and_(self, rd, rs, rt):
        self.r(rs, rt, rd, 0, 0x24)

    def li(self, rt, value):
        value &= 0xFFFFFFFF
        self.lui(rt, value >> 16)
        self.ori(rt, rt, value & 0xFFFF)

    def branch(self, op, rs, rt, label):
        self.fixups.append((len(self.words), "b", label))
        self.i(op, rs, rt, 0)

    def bne(self, rs, rt, label):
        self.branch(0x05, rs, rt, label)

    def jr(self, rs):
        self.r(rs, 0, 0, 0, 0x08)

    def j(self, label):
        self.fixups.append((len(self.words), "j", label))
        self.emit(0x02 << 26)

    def assemble(self):
        for idx, kind, label in self.fixups:
            target = self.labels[label]
            pc = self.base + 4 * idx
            if kind == "b":
                off = (target - (pc + 4)) >> 2
                self.words[idx] |= off & 0xFFFF
            else:
                self.words[idx] |= (target >> 2) & 0x3FFFFFF
        return b"".join(struct.pack(">I", w) for w in self.words)


def main():
    out = sys.argv[1]
    opts = set(sys.argv[2:])  # --no-rdp, --no-ai: leave that part out (to narrow a problem down)
    rom = bytearray(2 * 1024 * 1024)

    # header (big-endian, .z64 order)
    struct.pack_into(">IIII", rom, 0, 0x80371240, 0x0000000F, ENTRY, 0x00001444)
    struct.pack_into(">II", rom, 0x10, 0x4E363450, 0x53355445)  # "N64P" "S5TE": a CRC no catalog entry has
    rom[0x20:0x34] = b"N64PS5 TEST ROM".ljust(20, b" ")
    rom[0x3B] = ord("N")
    rom[0x3C:0x3E] = b"ZT"
    rom[0x3E] = ord("E")  # USA: NTSC
    rom[0x3F] = 0

    COLORS = 0x1800
    CMDS = 0x1900
    SOUND = 0x2000
    SND_SAMPLES = 3200  # 0.1 s at 32 kHz
    SND_LEN = SND_SAMPLES * 4
    FB = 0x00100000

    # colour bars, RGBA5551: white, yellow, cyan, green, magenta, red, blue, grey
    colors = [0xFFFF, 0xFFC1, 0x07FF, 0x07C1, 0xF83F, 0xF801, 0x003F, 0x8421]
    for k, c in enumerate(colors):
        struct.pack_into(">H", rom, COLORS + 2 * k, c)

    # RDP command list (64-bit commands, big-endian)
    cmds = [
        (0x3F10013F, FB),  # set colour image: RGBA, 16 bpp, width 320, at FB
        (0x2D000000, 0x005003C0),  # scissor (0,0)-(320,240), 10.2 fixed point
        (0x2F300000, 0x00000000),  # other modes: fill cycle
        (0x37000000, 0xF801F801),  # fill colour: red, two pixels
        (0x36230230, 0x000A00A0),  # fill rectangle (40,40)-(140,140)
        (0x29000000, 0x00000000),  # sync full
    ]
    for k, (w0, w1) in enumerate(cmds):
        struct.pack_into(">II", rom, CMDS + 8 * k, w0, w1)
    cmds_end = CMDS + 8 * len(cmds)

    # 440 Hz square wave, stereo: each 32-bit word is left << 16 | right
    period = 32000 / 440.0
    for n in range(SND_SAMPLES):
        v = 6000 if (n % period) < period / 2 else -6000
        struct.pack_into(">hh", rom, SOUND + 4 * n, v, v)

    a = Asm(ENTRY)
    # VI: 320x240, 16 bpp, NTSC (libdragon's values), no gamma, no anti-aliasing
    vi = [0x0000320E, FB, 320, 2, 0, 0x03E52239, 0x0000020D, 0x00000C15, 0x0C150C15, 0x006C02EC,
          0x002501FF, 0x000E0204, 0x00000200, 0x00000400]
    a.lui(T0, 0xA440)
    for k, v in enumerate(vi):
        if v == 0:
            a.sw(ZERO, 4 * k, T0)
        else:
            a.li(T1, v)
            a.sw(T1, 4 * k, T0)

    # frame buffer: bar = (x >> 5) & 7
    if "--no-fill" in opts:
        a.j("after_fill")
        a.nop()
    a.li(T2, 0xA0000000 | FB)
    a.li(T6, 0x80000000 | phys(COLORS))
    a.li(T3, 240)
    a.label("row")
    a.li(T4, 0)
    a.label("col")
    a.srl(T5, T4, 5)
    a.andi(T5, T5, 7)
    a.sll(T5, T5, 1)
    a.addu(T5, T5, T6)
    a.lhu(T7, 0, T5)
    a.sh(T7, 0, T2)
    a.addiu(T2, T2, 2)
    a.addiu(T4, T4, 1)
    a.slti(T8, T4, 320)
    a.bne(T8, ZERO, "col")
    a.nop()
    a.addiu(T3, T3, -1)
    a.bne(T3, ZERO, "row")
    a.nop()
    a.label("after_fill")

    # RDP: commands from RDRAM (clear XBUS), start, end
    if "--no-rdp" not in opts:
        a.lui(T0, 0xA410)
        a.li(T1, 0x00000001)
        a.sw(T1, 0x0C, T0)
        a.li(T1, phys(CMDS))
        a.sw(T1, 0x00, T0)
        a.li(T1, phys(cmds_end))
        a.sw(T1, 0x04, T0)

    if "--no-ai" in opts:
        a.label("idle")
        a.j("idle")
        a.nop()
    else:
        # AI: 32 kHz, DMA on, then keep two buffers queued
        a.lui(T0, 0xA450)
        a.li(T1, 48681812 // 32000 - 1)
        a.sw(T1, 0x10, T0)
        a.li(T1, 15)
        a.sw(T1, 0x14, T0)
        a.li(T1, 1)
        a.sw(T1, 0x08, T0)
        a.lui(T3, 0x8000)
        a.label("loop")
        a.lw(T1, 0x0C, T0)
        a.and_(T1, T1, T3)
        a.bne(T1, ZERO, "loop")
        a.nop()
        a.li(T1, phys(SOUND))
        a.sw(T1, 0x00, T0)
        a.li(T1, SND_LEN)
        a.sw(T1, 0x04, T0)
        a.j("loop")
        a.nop()

    # the boot stub, run from DMEM (0xA4000040): copy 32 KiB of the program, then jump to it
    b = Asm(0xA4000040)
    b.li(T0, 0xB0000000 | CODE_ROM)
    b.li(T1, 0xA0000000 | phys(CODE_ROM))
    b.li(T2, 0x8000)
    b.label("copy")
    b.lw(T3, 0, T0)
    b.sw(T3, 0, T1)
    b.addiu(T0, T0, 4)
    b.addiu(T1, T1, 4)
    b.addiu(T2, T2, -4)
    b.bne(T2, ZERO, "copy")
    b.nop()
    b.li(T0, ENTRY)
    b.jr(T0)
    b.nop()
    boot = b.assemble()
    rom[0x40:0x40 + len(boot)] = boot
    assert SOUND + SND_LEN <= CODE_ROM + 0x8000, "data past what the stub copies"

    code = a.assemble()
    assert CODE_ROM + len(code) <= COLORS, "code overlaps the data"
    rom[CODE_ROM:CODE_ROM + len(code)] = code
    with open(out, "wb") as f:
        f.write(rom)
    print("%s: %d bytes of code, entry %08x" % (out, len(code), ENTRY))


if __name__ == "__main__":
    main()
