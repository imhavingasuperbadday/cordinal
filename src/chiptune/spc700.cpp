// The SPC700 core: an interpreter for every opcode that returns each instruction's cycle count,
// so the SPC snapshot player can interleave the CPU with the DSP.
#include "spc700.h"

namespace {

// Cycles per opcode; taken conditional branches add 2.
constexpr uint8_t kCycles[256] = {
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 6, 8,   // 0
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 4, 6,   // 1
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 5, 4,   // 2
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 3, 8,   // 3
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 6, 6,   // 4
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 4, 5, 2, 2, 4, 3,   // 5
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 5, 5,   // 6
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 6,   // 7
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 2, 4, 5,   // 8
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 12, 5,  // 9
    3, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 2, 4, 4,   // A
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 4,   // B
    3, 8, 4, 5, 4, 5, 4, 7, 2, 5, 6, 4, 5, 2, 4, 9,   // C
    2, 8, 4, 5, 5, 6, 6, 7, 4, 5, 5, 5, 2, 2, 6, 3,   // D
    2, 8, 4, 5, 3, 4, 3, 6, 2, 4, 5, 3, 4, 3, 4, 3,   // E
    2, 8, 4, 5, 4, 5, 5, 6, 3, 4, 5, 4, 2, 2, 4, 3,   // F
};

}  // namespace

// OR AND EOR CMP ADC SBC. SBC borrows by adding ~r; for CMP the carry means "no borrow".
uint8_t Spc700::Alu(int op, uint8_t l, uint8_t r) {
    uint8_t v;
    switch (op) {
        case 0: v = l | r; break;
        case 1: v = l & r; break;
        case 2: v = l ^ r; break;
        case 3:
            SetFlag(C, l >= r);
            SetNZ((uint8_t)(l - r));
            return l;
        case 4: {
            const int carry = psw & C;
            const int t = l + r + carry;
            SetFlag(H, ((l & 15) + (r & 15) + carry) > 15);
            SetFlag(V, (~(l ^ r) & (l ^ t) & 0x80) != 0);
            SetFlag(C, t > 0xFF);
            v = (uint8_t)t;
            break;
        }
        default: return Alu(4, l, (uint8_t)~r);  // SBC: H and C mean "no borrow"
    }
    SetNZ(v);
    return v;
}

// ASL ROL LSR ROR. Each pulls the carry in or out as the SPC700 defines it.
uint8_t Spc700::Shift(int op, uint8_t v) {
    const uint8_t carry = psw & C;
    switch (op) {
        case 0: SetFlag(C, v & 0x80); v = (uint8_t)(v << 1); break;
        case 1: SetFlag(C, v & 0x80); v = (uint8_t)((v << 1) | carry); break;
        case 2: SetFlag(C, v & 1); v >>= 1; break;
        default: SetFlag(C, v & 1); v = (uint8_t)((v >> 1) | (carry << 7)); break;
    }
    SetNZ(v);
    return v;
}

int Spc700::Branch(bool taken) {
    const int8_t rel = (int8_t)Fetch();
    if (!taken) return 0;
    pc = (uint16_t)(pc + rel);
    return 2;
}

int Spc700::Step() {
    if (halted) return 2;
    const uint8_t op = Fetch();
    int cycles = kCycles[op];
    const int lo = op & 15, row = op >> 4;

    if (lo == 1) {  // TCALL n
        Push16(pc);
        pc = ReadWord((uint16_t)(0xFFDE - row * 2));
        return cycles;
    }
    if (lo == 2) {  // SET1 / CLR1 dp.bit
        const uint16_t addr = Dp(Fetch());
        const uint8_t m = Read(addr), mask = (uint8_t)(1 << (row >> 1));
        Write(addr, (row & 1) ? (uint8_t)(m & ~mask) : (uint8_t)(m | mask));
        return cycles;
    }
    if (lo == 3) {  // BBS / BBC dp.bit, rel
        const uint8_t m = Read(Dp(Fetch()));
        const bool set = (m >> (row >> 1)) & 1;
        return cycles + Branch((row & 1) ? !set : set);
    }

    // OR AND EOR CMP ADC SBC, in rows 0-B, columns 4-9.
    if (row < 0xC && lo >= 4 && lo <= 9) {
        const int alu = row >> 1;
        if (!(row & 1)) {
            switch (lo) {
                case 4: a = Alu(alu, a, Read(Dp(Fetch()))); break;
                case 5: a = Alu(alu, a, Read(Fetch16())); break;
                case 6: a = Alu(alu, a, Read(Dp(x))); break;
                case 7: a = Alu(alu, a, Read(ReadWordDp((uint8_t)(Fetch() + x)))); break;
                case 8: a = Alu(alu, a, Fetch()); break;
                default: {  // dp, dp: source first
                    const uint8_t src = Read(Dp(Fetch()));
                    const uint16_t dst = Dp(Fetch());
                    const uint8_t r = Alu(alu, Read(dst), src);
                    if (alu != 3) Write(dst, r);
                    break;
                }
            }
        } else {
            switch (lo) {
                case 4: a = Alu(alu, a, Read(Dp((uint8_t)(Fetch() + x)))); break;
                case 5: a = Alu(alu, a, Read((uint16_t)(Fetch16() + x))); break;
                case 6: a = Alu(alu, a, Read((uint16_t)(Fetch16() + y))); break;
                case 7: a = Alu(alu, a, Read((uint16_t)(ReadWordDp(Fetch()) + y))); break;
                case 8: {  // dp, #imm: immediate first
                    const uint8_t imm = Fetch();
                    const uint16_t dst = Dp(Fetch());
                    const uint8_t r = Alu(alu, Read(dst), imm);
                    if (alu != 3) Write(dst, r);
                    break;
                }
                default: {  // (X), (Y)
                    const uint8_t src = Read(Dp(y));
                    const uint16_t dst = Dp(x);
                    const uint8_t r = Alu(alu, Read(dst), src);
                    if (alu != 3) Write(dst, r);
                    break;
                }
            }
        }
        return cycles;
    }

    // ASL ROL LSR ROR DEC INC, in rows 0-B, columns B and C.
    if (row < 0xC && (lo == 0xB || lo == 0xC)) {
        auto apply = [&](uint8_t v) -> uint8_t {
            if (row < 8) return Shift(row >> 1, v);
            const uint8_t r = (uint8_t)(row < 0xA ? v - 1 : v + 1);
            SetNZ(r);
            return r;
        };
        if (lo == 0xC && (row & 1)) {
            a = apply(a);
        } else {
            const uint16_t addr = lo == 0xB ? Dp((uint8_t)(Fetch() + ((row & 1) ? x : 0))) : Fetch16();
            Write(addr, apply(Read(addr)));
        }
        return cycles;
    }

    switch (op) {
        // Column 0: flags and branches
        case 0x00: break;
        case 0x20: psw &= (uint8_t)~P; break;
        case 0x40: psw |= P; break;
        case 0x60: psw &= (uint8_t)~C; break;
        case 0x80: psw |= C; break;
        case 0xA0: psw |= I; break;
        case 0xC0: psw &= (uint8_t)~I; break;
        case 0xE0: psw &= (uint8_t)~(V | H); break;
        case 0x10: cycles += Branch(!(psw & N)); break;
        case 0x30: cycles += Branch(psw & N); break;
        case 0x50: cycles += Branch(!(psw & V)); break;
        case 0x70: cycles += Branch(psw & V); break;
        case 0x90: cycles += Branch(!(psw & C)); break;
        case 0xB0: cycles += Branch(psw & C); break;
        case 0xD0: cycles += Branch(!(psw & Z)); break;
        case 0xF0: cycles += Branch(psw & Z); break;

        // Column A: bit operations on a 13-bit address, and 16-bit ops
        case 0x0A: case 0x2A: case 0x4A: case 0x6A: case 0x8A: case 0xAA: case 0xCA: case 0xEA: {
            const uint16_t w = Fetch16(), addr = w & 0x1FFF;
            const int bit = w >> 13;
            const uint8_t m = Read(addr);
            const bool b = (m >> bit) & 1, c = psw & C;
            switch (op) {
                case 0x0A: SetFlag(C, c || b); break;
                case 0x2A: SetFlag(C, c || !b); break;
                case 0x4A: SetFlag(C, c && b); break;
                case 0x6A: SetFlag(C, c && !b); break;
                case 0x8A: SetFlag(C, c != b); break;
                case 0xAA: SetFlag(C, b); break;
                case 0xCA: Write(addr, c ? (uint8_t)(m | 1 << bit) : (uint8_t)(m & ~(1 << bit))); break;
                default: Write(addr, (uint8_t)(m ^ 1 << bit)); break;
            }
            break;
        }
        case 0x1A:
        case 0x3A: {  // DECW / INCW
            const uint8_t d = Fetch();
            const uint16_t w = (uint16_t)(ReadWordDp(d) + (op == 0x3A ? 1 : -1));
            Write(Dp(d), (uint8_t)w);
            Write(Dp((uint8_t)(d + 1)), (uint8_t)(w >> 8));
            SetFlag(N, w & 0x8000);
            SetFlag(Z, w == 0);
            break;
        }
        case 0x5A: {  // CMPW
            const uint16_t w = ReadWordDp(Fetch()), ya = YA();
            SetFlag(C, ya >= w);
            SetFlag(N, (ya - w) & 0x8000);
            SetFlag(Z, ya == w);
            break;
        }
        case 0x7A:
        case 0x9A: {  // ADDW / SUBW
            const uint16_t w = ReadWordDp(Fetch()), ya = YA();
            int r;
            if (op == 0x7A) {
                r = ya + w;
                SetFlag(H, ((ya & 0xFFF) + (w & 0xFFF)) > 0xFFF);
                SetFlag(V, (~(ya ^ w) & (ya ^ r) & 0x8000) != 0);
                SetFlag(C, r > 0xFFFF);
            } else {
                r = ya - w;
                SetFlag(H, (ya & 0xFFF) >= (w & 0xFFF));
                SetFlag(V, ((ya ^ w) & (ya ^ r) & 0x8000) != 0);
                SetFlag(C, r >= 0);
            }
            a = (uint8_t)r;
            y = (uint8_t)(r >> 8);
            SetFlag(N, r & 0x8000);
            SetFlag(Z, (r & 0xFFFF) == 0);
            break;
        }
        case 0xBA: {  // MOVW YA, dp
            const uint16_t w = ReadWordDp(Fetch());
            a = (uint8_t)w;
            y = (uint8_t)(w >> 8);
            SetFlag(N, w & 0x8000);
            SetFlag(Z, w == 0);
            break;
        }
        case 0xDA: {  // MOVW dp, YA
            const uint8_t d = Fetch();
            Read(Dp(d));
            Write(Dp(d), a);
            Write(Dp((uint8_t)(d + 1)), y);
            break;
        }
        case 0xFA: {  // MOV dp, dp (no dummy read)
            const uint8_t v = Read(Dp(Fetch()));
            Write(Dp(Fetch()), v);
            break;
        }

        // Column D
        case 0x0D: Push(psw); break;
        case 0x2D: Push(a); break;
        case 0x4D: Push(x); break;
        case 0x6D: Push(y); break;
        case 0x1D: SetNZ(--x); break;
        case 0x3D: SetNZ(++x); break;
        case 0x5D: x = a; SetNZ(x); break;
        case 0x7D: a = x; SetNZ(a); break;
        case 0x8D: y = Fetch(); SetNZ(y); break;
        case 0x9D: x = sp; SetNZ(x); break;
        case 0xAD: Alu(3, y, Fetch()); break;
        case 0xBD: sp = x; break;
        case 0xCD: x = Fetch(); SetNZ(x); break;
        case 0xDD: a = y; SetNZ(a); break;
        case 0xED: psw ^= C; break;
        case 0xFD: y = a; SetNZ(y); break;

        // Column E
        case 0x0E:
        case 0x4E: {  // TSET1 / TCLR1
            const uint16_t addr = Fetch16();
            const uint8_t m = Read(addr);
            SetNZ((uint8_t)(a - m));
            Write(addr, op == 0x0E ? (uint8_t)(m | a) : (uint8_t)(m & ~a));
            break;
        }
        case 0x1E: Alu(3, x, Read(Fetch16())); break;
        case 0x3E: Alu(3, x, Read(Dp(Fetch()))); break;
        case 0x5E: Alu(3, y, Read(Fetch16())); break;
        case 0x7E: Alu(3, y, Read(Dp(Fetch()))); break;
        case 0x2E: {
            const uint8_t m = Read(Dp(Fetch()));
            cycles += Branch(a != m);
            break;
        }
        case 0xDE: {
            const uint8_t m = Read(Dp((uint8_t)(Fetch() + x)));
            cycles += Branch(a != m);
            break;
        }
        case 0x6E: {
            const uint16_t addr = Dp(Fetch());
            const uint8_t m = (uint8_t)(Read(addr) - 1);
            Write(addr, m);
            cycles += Branch(m != 0);
            break;
        }
        case 0xFE: --y; cycles += Branch(y != 0); break;
        case 0x8E: psw = Pop(); break;
        case 0xAE: a = Pop(); break;
        case 0xCE: x = Pop(); break;
        case 0xEE: y = Pop(); break;
        case 0x9E: {  // DIV YA, X; the hardware's results when the quotient overflows
            const uint32_t ya = YA();
            SetFlag(V, y >= x);
            SetFlag(H, (y & 15) >= (x & 15));
            if (y < (x << 1)) {
                a = (uint8_t)(ya / x);
                y = (uint8_t)(ya % x);
            } else {
                a = (uint8_t)(255 - (ya - (x << 9)) / (256 - x));
                y = (uint8_t)(x + (ya - (x << 9)) % (256 - x));
            }
            SetNZ(a);
            break;
        }
        case 0xBE: {  // DAS
            if (!(psw & C) || a > 0x99) {
                a = (uint8_t)(a - 0x60);
                psw &= (uint8_t)~C;
            }
            if (!(psw & H) || (a & 15) > 9) a = (uint8_t)(a - 6);
            SetNZ(a);
            break;
        }

        // Column F
        case 0x0F:  // BRK
            Push16(pc);
            Push(psw);
            psw = (uint8_t)((psw | B) & ~I);
            pc = ReadWord(0xFFDE);
            break;
        case 0x1F: pc = ReadWord((uint16_t)(Fetch16() + x)); break;
        case 0x2F: Branch(true); break;  // BRA: its 4 cycles are in the table
        case 0x3F: {
            const uint16_t addr = Fetch16();
            Push16(pc);
            pc = addr;
            break;
        }
        case 0x4F: {
            const uint8_t u = Fetch();
            Push16(pc);
            pc = (uint16_t)(0xFF00 | u);
            break;
        }
        case 0x5F: pc = Fetch16(); break;
        case 0x6F: pc = Pop16(); break;
        case 0x7F:
            psw = Pop();
            pc = Pop16();
            break;
        case 0x8F: {  // MOV dp, #imm
            const uint8_t imm = Fetch();
            Store(Dp(Fetch()), imm);
            break;
        }
        case 0x9F: a = (uint8_t)((a >> 4) | (a << 4)); SetNZ(a); break;
        case 0xAF: Write(Dp(x++), a); break;
        case 0xBF: a = Read(Dp(x++)); SetNZ(a); break;
        case 0xCF: {
            const uint16_t r = (uint16_t)(y * a);
            a = (uint8_t)r;
            y = (uint8_t)(r >> 8);
            SetNZ(y);
            break;
        }
        case 0xDF: {  // DAA
            if ((psw & C) || a > 0x99) {
                a = (uint8_t)(a + 0x60);
                psw |= C;
            }
            if ((psw & H) || (a & 15) > 9) a = (uint8_t)(a + 6);
            SetNZ(a);
            break;
        }
        case 0xEF:
        case 0xFF: halted = true; break;

        // Moves in rows C-F, columns 4-9
        case 0xC4: Store(Dp(Fetch()), a); break;
        case 0xC5: Store(Fetch16(), a); break;
        case 0xC6: Store(Dp(x), a); break;
        case 0xC7: Store(ReadWordDp((uint8_t)(Fetch() + x)), a); break;
        case 0xC8: Alu(3, x, Fetch()); break;
        case 0xC9: Store(Fetch16(), x); break;
        case 0xD4: Store(Dp((uint8_t)(Fetch() + x)), a); break;
        case 0xD5: Store((uint16_t)(Fetch16() + x), a); break;
        case 0xD6: Store((uint16_t)(Fetch16() + y), a); break;
        case 0xD7: Store((uint16_t)(ReadWordDp(Fetch()) + y), a); break;
        case 0xD8: Store(Dp(Fetch()), x); break;
        case 0xD9: Store(Dp((uint8_t)(Fetch() + y)), x); break;
        case 0xE4: a = Read(Dp(Fetch())); SetNZ(a); break;
        case 0xE5: a = Read(Fetch16()); SetNZ(a); break;
        case 0xE6: a = Read(Dp(x)); SetNZ(a); break;
        case 0xE7: a = Read(ReadWordDp((uint8_t)(Fetch() + x))); SetNZ(a); break;
        case 0xE8: a = Fetch(); SetNZ(a); break;
        case 0xE9: x = Read(Fetch16()); SetNZ(x); break;
        case 0xF4: a = Read(Dp((uint8_t)(Fetch() + x))); SetNZ(a); break;
        case 0xF5: a = Read((uint16_t)(Fetch16() + x)); SetNZ(a); break;
        case 0xF6: a = Read((uint16_t)(Fetch16() + y)); SetNZ(a); break;
        case 0xF7: a = Read((uint16_t)(ReadWordDp(Fetch()) + y)); SetNZ(a); break;
        case 0xF8: x = Read(Dp(Fetch())); SetNZ(x); break;
        case 0xF9: x = Read(Dp((uint8_t)(Fetch() + y))); SetNZ(x); break;

        // Rows C-F, columns B and C
        case 0xCB: Store(Dp(Fetch()), y); break;
        case 0xCC: Store(Fetch16(), y); break;
        case 0xDB: Store(Dp((uint8_t)(Fetch() + x)), y); break;
        case 0xDC: SetNZ(--y); break;
        case 0xEB: y = Read(Dp(Fetch())); SetNZ(y); break;
        case 0xEC: y = Read(Fetch16()); SetNZ(y); break;
        case 0xFB: y = Read(Dp((uint8_t)(Fetch() + x))); SetNZ(y); break;
        case 0xFC: SetNZ(++y); break;
        default: break;
    }
    return cycles;
}
