// 6502 (2A03) interpreter. The addressing mode and base-cycle tables live here; Step() decodes
// one instruction, adding a cycle for page-crossing reads and taken branches. Includes the
// stable undocumented opcodes NSF rips use; the KIL opcodes jam the CPU until Reset.
#include "cpu6502.h"

namespace {

enum Mode : uint8_t { IMP, ACC, IMM, ZP, ZPX, ZPY, ABS, ABX, ABY, IZX, IZY, REL, IND };

// Addressing mode of each opcode. Rows are the high nibble.
constexpr uint8_t kMode[256] = {
    IMP, IZX, IMP, IZX, ZP,  ZP,  ZP,  ZP,  IMP, IMM, ACC, IMM, ABS, ABS, ABS, ABS,  // 0
    REL, IZY, IMP, IZY, ZPX, ZPX, ZPX, ZPX, IMP, ABY, IMP, ABY, ABX, ABX, ABX, ABX,  // 1
    ABS, IZX, IMP, IZX, ZP,  ZP,  ZP,  ZP,  IMP, IMM, ACC, IMM, ABS, ABS, ABS, ABS,  // 2
    REL, IZY, IMP, IZY, ZPX, ZPX, ZPX, ZPX, IMP, ABY, IMP, ABY, ABX, ABX, ABX, ABX,  // 3
    IMP, IZX, IMP, IZX, ZP,  ZP,  ZP,  ZP,  IMP, IMM, ACC, IMM, ABS, ABS, ABS, ABS,  // 4
    REL, IZY, IMP, IZY, ZPX, ZPX, ZPX, ZPX, IMP, ABY, IMP, ABY, ABX, ABX, ABX, ABX,  // 5
    IMP, IZX, IMP, IZX, ZP,  ZP,  ZP,  ZP,  IMP, IMM, ACC, IMM, IND, ABS, ABS, ABS,  // 6
    REL, IZY, IMP, IZY, ZPX, ZPX, ZPX, ZPX, IMP, ABY, IMP, ABY, ABX, ABX, ABX, ABX,  // 7
    IMM, IZX, IMM, IZX, ZP,  ZP,  ZP,  ZP,  IMP, IMM, IMP, IMM, ABS, ABS, ABS, ABS,  // 8
    REL, IZY, IMP, IZY, ZPX, ZPX, ZPY, ZPY, IMP, ABY, IMP, ABY, ABX, ABX, ABY, ABY,  // 9
    IMM, IZX, IMM, IZX, ZP,  ZP,  ZP,  ZP,  IMP, IMM, IMP, IMM, ABS, ABS, ABS, ABS,  // A
    REL, IZY, IMP, IZY, ZPX, ZPX, ZPY, ZPY, IMP, ABY, IMP, ABY, ABX, ABX, ABY, ABY,  // B
    IMM, IZX, IMM, IZX, ZP,  ZP,  ZP,  ZP,  IMP, IMM, IMP, IMM, ABS, ABS, ABS, ABS,  // C
    REL, IZY, IMP, IZY, ZPX, ZPX, ZPX, ZPX, IMP, ABY, IMP, ABY, ABX, ABX, ABX, ABX,  // D
    IMM, IZX, IMM, IZX, ZP,  ZP,  ZP,  ZP,  IMP, IMM, IMP, IMM, ABS, ABS, ABS, ABS,  // E
    REL, IZY, IMP, IZY, ZPX, ZPX, ZPX, ZPX, IMP, ABY, IMP, ABY, ABX, ABX, ABX, ABX,  // F
};

// Base cycles. Reads that cross a page and taken branches add more.
constexpr uint8_t kCycles[256] = {
    7, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6,  // 0
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,  // 1
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6,  // 2
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,  // 3
    6, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6,  // 4
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,  // 5
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6,  // 6
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,  // 7
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,  // 8
    2, 6, 2, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,  // 9
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,  // A
    2, 5, 2, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,  // B
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,  // C
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,  // D
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,  // E
    2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,  // F
};

// Reads (not stores or read-modify-writes) take a cycle more when indexing crosses a page.
bool PagePenalty(uint8_t op) {
    const int col = op & 15;
    if ((op & 0x10) && (col == 1 || col == 9 || col == 0xD)) return op != 0x91 && op != 0x99 && op != 0x9D;
    switch (op) {
        case 0xBC: case 0xBE: case 0xB3: case 0xBF: case 0xBB:
        case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: return true;
        default: return false;
    }
}

}  // namespace

void Cpu6502::Reset() {
    a = x = y = 0;
    s = 0xFD;
    p = I | U;
    pc = 0;
    time = 0;
    jammed = false;
}

void Cpu6502::Call(uint16_t addr, uint16_t idle) {
    // Like JSR from `idle`: RTS adds 1 to the pushed address.
    const uint16_t ret = (uint16_t)(idle - 1);
    Push((uint8_t)(ret >> 8));
    Push((uint8_t)ret);
    pc = addr;
    jammed = false;
}

void Cpu6502::Run(uint32_t end, uint16_t idle) {
    while (time < end) {
        if (pc == idle || jammed) {
            time = end;
            break;
        }
        Step();
    }
}

void Cpu6502::Adc(uint8_t m) {
    const unsigned sum = a + m + (p & C);
    if (decimal && (p & D)) {  // NMOS BCD: Z from the binary sum, N and V before the high digit's fix-up
        unsigned lo = (a & 15) + (m & 15) + (p & C), hi = (a & 0xF0) + (m & 0xF0);
        if (lo > 9) lo += 6;
        if (lo > 15) hi += 16;
        SetFlag(Z, (sum & 0xFF) == 0);
        SetFlag(N, hi & 0x80);
        SetFlag(V, (~(a ^ m) & (a ^ hi) & 0x80) != 0);
        if (hi > 0x90) hi += 0x60;
        SetFlag(C, hi > 0xFF);
        a = (uint8_t)((hi & 0xF0) | (lo & 15));
        return;
    }
    SetFlag(C, sum > 0xFF);
    SetFlag(V, (~(a ^ m) & (a ^ sum) & 0x80) != 0);
    a = (uint8_t)sum;
    SetNZ(a);
}

void Cpu6502::Sbc(uint8_t m) {
    if (!(decimal && (p & D))) {
        Adc((uint8_t)~m);
        return;
    }
    // BCD: the flags are the binary subtraction's, the result is digit-corrected.
    const unsigned borrow = (p & C) ? 0 : 1, diff = (unsigned)a - m - borrow;
    int lo = (a & 15) - (m & 15) - (int)borrow, hi = (a & 0xF0) - (m & 0xF0);
    if (lo & 16) {
        lo -= 6;
        hi -= 16;
    }
    if (hi & 0x100) hi -= 0x60;
    SetFlag(C, diff < 0x100);
    SetFlag(V, ((a ^ m) & (a ^ diff) & 0x80) != 0);
    SetNZ((uint8_t)diff);
    a = (uint8_t)((hi & 0xF0) | (lo & 15));
}

void Cpu6502::Interrupt(uint16_t vector) {
    Push((uint8_t)(pc >> 8));
    Push((uint8_t)pc);
    Push((uint8_t)((p & ~B) | U));
    p |= I;
    pc = (uint16_t)(Read(vector) | Read((uint16_t)(vector + 1)) << 8);
    time += 7;
}

void Cpu6502::Compare(uint8_t reg, uint8_t m) {
    SetFlag(C, reg >= m);
    SetNZ((uint8_t)(reg - m));
}

uint8_t Cpu6502::Asl(uint8_t m) {
    SetFlag(C, m & 0x80);
    m = (uint8_t)(m << 1);
    SetNZ(m);
    return m;
}

uint8_t Cpu6502::Lsr(uint8_t m) {
    SetFlag(C, m & 1);
    m >>= 1;
    SetNZ(m);
    return m;
}

uint8_t Cpu6502::Rol(uint8_t m) {
    const uint8_t carry = p & C;
    SetFlag(C, m & 0x80);
    m = (uint8_t)((m << 1) | carry);
    SetNZ(m);
    return m;
}

uint8_t Cpu6502::Ror(uint8_t m) {
    const uint8_t carry = p & C;
    SetFlag(C, m & 1);
    m = (uint8_t)((m >> 1) | (carry << 7));
    SetNZ(m);
    return m;
}

void Cpu6502::Branch(bool taken) {
    const int8_t offset = (int8_t)Fetch();
    if (!taken) return;
    const uint16_t target = (uint16_t)(pc + offset);
    time += ((target ^ pc) & 0x100) ? 2 : 1;
    pc = target;
}

void Cpu6502::Step() {
    const uint8_t op = Fetch();
    time += kCycles[op];
    const uint8_t mode = kMode[op];

    // Effective address. Immediate operands are read through it too.
    uint16_t ea = 0;
    uint8_t baseHigh = 0;  // high byte of an indexed address before indexing
    bool crossed = false;
    auto indexed = [&](uint16_t base, uint8_t index) {
        ea = (uint16_t)(base + index);
        baseHigh = (uint8_t)(base >> 8);
        crossed = ((base ^ ea) & 0x100) != 0;
    };
    // SHA, SHX, SHY and TAS store the value ANDed with the base's high byte + 1; when indexing
    // crosses a page, that value also replaces the high byte of the address.
    auto storeHigh = [&](uint8_t v) {
        v &= (uint8_t)(baseHigh + 1);
        Write(crossed ? (uint16_t)((v << 8) | (ea & 0xFF)) : ea, v);
    };
    switch (mode) {
        case IMM: ea = pc++; break;
        case ZP: ea = Fetch(); break;
        case ZPX: ea = (uint8_t)(Fetch() + x); break;
        case ZPY: ea = (uint8_t)(Fetch() + y); break;
        case ABS: ea = Fetch(); ea |= (uint16_t)(Fetch() << 8); break;
        case ABX:
        case ABY: {
            uint16_t base = Fetch();
            base |= (uint16_t)(Fetch() << 8);
            indexed(base, mode == ABX ? x : y);
            break;
        }
        case IZX: {
            const uint8_t z = (uint8_t)(Fetch() + x);
            ea = (uint16_t)(Read(z) | Read((uint8_t)(z + 1)) << 8);
            break;
        }
        case IZY: {
            const uint8_t z = Fetch();
            indexed((uint16_t)(Read(z) | Read((uint8_t)(z + 1)) << 8), y);
            break;
        }
        case IND: {
            uint16_t ptr = Fetch();
            ptr |= (uint16_t)(Fetch() << 8);
            // The 6502 doesn't carry into the high byte: JMP ($xxFF) wraps within the page.
            ea = (uint16_t)(Read(ptr) | Read((uint16_t)((ptr & 0xFF00) | ((ptr + 1) & 0xFF))) << 8);
            break;
        }
        default: break;
    }
    if (crossed && PagePenalty(op)) ++time;

    auto load = [&]() { return Read(ea); };
    // Read-modify-write on memory or, in accumulator mode, on A.
    auto modify = [&](uint8_t (Cpu6502::*f)(uint8_t)) {
        if (mode == ACC) {
            a = (this->*f)(a);
            return a;
        }
        const uint8_t m = (this->*f)(Read(ea));
        Write(ea, m);
        return m;
    };
    auto step = [&](int delta) {  // INC/DEC memory
        const uint8_t m = (uint8_t)(Read(ea) + delta);
        Write(ea, m);
        SetNZ(m);
        return m;
    };

    switch (op) {
        // Loads, stores, transfers
        case 0xA9: case 0xA5: case 0xB5: case 0xAD: case 0xBD: case 0xB9: case 0xA1: case 0xB1: a = load(); SetNZ(a); break;
        case 0xA2: case 0xA6: case 0xB6: case 0xAE: case 0xBE: x = load(); SetNZ(x); break;
        case 0xA0: case 0xA4: case 0xB4: case 0xAC: case 0xBC: y = load(); SetNZ(y); break;
        case 0x85: case 0x95: case 0x8D: case 0x9D: case 0x99: case 0x81: case 0x91: Write(ea, a); break;
        case 0x86: case 0x96: case 0x8E: Write(ea, x); break;
        case 0x84: case 0x94: case 0x8C: Write(ea, y); break;
        case 0xAA: x = a; SetNZ(x); break;
        case 0xA8: y = a; SetNZ(y); break;
        case 0x8A: a = x; SetNZ(a); break;
        case 0x98: a = y; SetNZ(a); break;
        case 0xBA: x = s; SetNZ(x); break;
        case 0x9A: s = x; break;

        // Arithmetic and logic
        case 0x09: case 0x05: case 0x15: case 0x0D: case 0x1D: case 0x19: case 0x01: case 0x11: a |= load(); SetNZ(a); break;
        case 0x29: case 0x25: case 0x35: case 0x2D: case 0x3D: case 0x39: case 0x21: case 0x31: a &= load(); SetNZ(a); break;
        case 0x49: case 0x45: case 0x55: case 0x4D: case 0x5D: case 0x59: case 0x41: case 0x51: a ^= load(); SetNZ(a); break;
        case 0x69: case 0x65: case 0x75: case 0x6D: case 0x7D: case 0x79: case 0x61: case 0x71: Adc(load()); break;
        case 0xE9: case 0xEB: case 0xE5: case 0xF5: case 0xED: case 0xFD: case 0xF9: case 0xE1: case 0xF1: Sbc(load()); break;
        case 0xC9: case 0xC5: case 0xD5: case 0xCD: case 0xDD: case 0xD9: case 0xC1: case 0xD1: Compare(a, load()); break;
        case 0xE0: case 0xE4: case 0xEC: Compare(x, load()); break;
        case 0xC0: case 0xC4: case 0xCC: Compare(y, load()); break;
        case 0x24: case 0x2C: {
            const uint8_t m = load();
            p = (uint8_t)((p & ~(N | V | Z)) | (m & (N | V)) | ((a & m) ? 0 : Z));
            break;
        }

        // Shifts, increments
        case 0x0A: case 0x06: case 0x16: case 0x0E: case 0x1E: modify(&Cpu6502::Asl); break;
        case 0x4A: case 0x46: case 0x56: case 0x4E: case 0x5E: modify(&Cpu6502::Lsr); break;
        case 0x2A: case 0x26: case 0x36: case 0x2E: case 0x3E: modify(&Cpu6502::Rol); break;
        case 0x6A: case 0x66: case 0x76: case 0x6E: case 0x7E: modify(&Cpu6502::Ror); break;
        case 0xE6: case 0xF6: case 0xEE: case 0xFE: step(1); break;
        case 0xC6: case 0xD6: case 0xCE: case 0xDE: step(-1); break;
        case 0xE8: SetNZ(++x); break;
        case 0xC8: SetNZ(++y); break;
        case 0xCA: SetNZ(--x); break;
        case 0x88: SetNZ(--y); break;

        // Flags
        case 0x18: p &= (uint8_t)~C; break;
        case 0x38: p |= C; break;
        case 0x58: p &= (uint8_t)~I; break;
        case 0x78: p |= I; break;
        case 0xB8: p &= (uint8_t)~V; break;
        case 0xD8: p &= (uint8_t)~D; break;
        case 0xF8: p |= D; break;

        // Stack
        case 0x48: Push(a); break;
        case 0x68: a = Pull(); SetNZ(a); break;
        case 0x08: Push(p | B | U); break;
        case 0x28: p = (uint8_t)((Pull() & ~B) | U); break;

        // Jumps and branches
        case 0x4C: case 0x6C: pc = ea; break;
        case 0x20: {
            const uint16_t ret = (uint16_t)(pc - 1);
            Push((uint8_t)(ret >> 8));
            Push((uint8_t)ret);
            pc = ea;
            break;
        }
        case 0x60: {
            uint16_t ret = Pull();
            ret |= (uint16_t)(Pull() << 8);
            pc = (uint16_t)(ret + 1);
            break;
        }
        case 0x40: {
            p = (uint8_t)((Pull() & ~B) | U);
            uint16_t ret = Pull();
            ret |= (uint16_t)(Pull() << 8);
            pc = ret;
            break;
        }
        case 0x00: {
            const uint16_t ret = (uint16_t)(pc + 1);
            Push((uint8_t)(ret >> 8));
            Push((uint8_t)ret);
            Push(p | B | U);
            p |= I;
            pc = (uint16_t)(Read(0xFFFE) | Read(0xFFFF) << 8);
            break;
        }
        case 0x10: Branch(!(p & N)); break;
        case 0x30: Branch(p & N); break;
        case 0x50: Branch(!(p & V)); break;
        case 0x70: Branch(p & V); break;
        case 0x90: Branch(!(p & C)); break;
        case 0xB0: Branch(p & C); break;
        case 0xD0: Branch(!(p & Z)); break;
        case 0xF0: Branch(p & Z); break;

        // No-ops, documented and not; the addressed ones still read their operand
        case 0xEA: case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA: break;
        case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: case 0x04: case 0x44: case 0x64:
        case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: case 0x0C:
        case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: break;

        // Undocumented combinations
        case 0x07: case 0x17: case 0x0F: case 0x1F: case 0x1B: case 0x03: case 0x13:  // SLO
            a |= modify(&Cpu6502::Asl);
            SetNZ(a);
            break;
        case 0x27: case 0x37: case 0x2F: case 0x3F: case 0x3B: case 0x23: case 0x33:  // RLA
            a &= modify(&Cpu6502::Rol);
            SetNZ(a);
            break;
        case 0x47: case 0x57: case 0x4F: case 0x5F: case 0x5B: case 0x43: case 0x53:  // SRE
            a ^= modify(&Cpu6502::Lsr);
            SetNZ(a);
            break;
        case 0x67: case 0x77: case 0x6F: case 0x7F: case 0x7B: case 0x63: case 0x73:  // RRA
            Adc(modify(&Cpu6502::Ror));
            break;
        case 0x87: case 0x97: case 0x8F: case 0x83: Write(ea, a & x); break;  // SAX
        case 0xA7: case 0xB7: case 0xAF: case 0xBF: case 0xA3: case 0xB3: a = x = load(); SetNZ(a); break;  // LAX
        case 0xC7: case 0xD7: case 0xCF: case 0xDF: case 0xDB: case 0xC3: case 0xD3: Compare(a, step(-1)); break;  // DCP
        case 0xE7: case 0xF7: case 0xEF: case 0xFF: case 0xFB: case 0xE3: case 0xF3: Sbc(step(1)); break;  // ISC
        case 0x0B: case 0x2B: a &= load(); SetNZ(a); SetFlag(C, a & 0x80); break;  // ANC
        case 0x4B: a &= load(); a = Lsr(a); break;  // ALR
        case 0x6B: {  // ARR
            a &= load();
            a = (uint8_t)((a >> 1) | ((p & C) << 7));
            SetNZ(a);
            SetFlag(C, a & 0x40);
            SetFlag(V, ((a >> 6) ^ (a >> 5)) & 1);
            break;
        }
        case 0x8B: a = (a | 0xEE) & x & load(); SetNZ(a); break;  // XAA (unstable; the usual "magic" 0xEE)
        case 0xAB: a = x = (a | 0xEE) & load(); SetNZ(a); break;  // LXA (likewise)
        case 0xCB: {                                      // AXS
            const uint8_t m = load(), ax = a & x;
            SetFlag(C, ax >= m);
            x = (uint8_t)(ax - m);
            SetNZ(x);
            break;
        }
        case 0xBB: a = x = s = load() & s; SetNZ(a); break;  // LAS
        case 0x9C: storeHigh(y); break;              // SHY
        case 0x9E: storeHigh(x); break;              // SHX
        case 0x93: case 0x9F: storeHigh(a & x); break;  // SHA
        case 0x9B: s = a & x; storeHigh(s); break;   // TAS

        default:  // the KIL opcodes stop the CPU
            jammed = true;
            break;
    }
}
