// Game Boy (LR35902) interpreter. Its regular encoding lets most opcodes be handled by high-bit
// range (LD r,r' / ALU / the 0x00-0x3F group / the CB prefix), backed by a 256-entry machine-cycle
// table. Undefined opcodes lock the CPU, which GBS code relies on to signal a finished song.
#include "cpu_gb.h"

namespace {

// Machine cycles (4 clocks each) per opcode, with conditional branches not taken. 0 marks
// the opcodes that don't exist.
constexpr uint8_t kCycles[256] = {
    1, 3, 2, 2, 1, 1, 2, 1, 5, 2, 2, 2, 1, 1, 2, 1,  // 0
    1, 3, 2, 2, 1, 1, 2, 1, 3, 2, 2, 2, 1, 1, 2, 1,  // 1
    2, 3, 2, 2, 1, 1, 2, 1, 2, 2, 2, 2, 1, 1, 2, 1,  // 2
    2, 3, 2, 2, 3, 3, 3, 1, 2, 2, 2, 2, 1, 1, 2, 1,  // 3
    1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1,  // 4
    1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1,  // 5
    1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1,  // 6
    2, 2, 2, 2, 2, 2, 1, 2, 1, 1, 1, 1, 1, 1, 2, 1,  // 7
    1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1,  // 8
    1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1,  // 9
    1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1,  // A
    1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1,  // B
    2, 3, 3, 4, 3, 4, 2, 4, 2, 4, 3, 1, 3, 6, 2, 4,  // C
    2, 3, 3, 0, 3, 4, 2, 4, 2, 4, 3, 0, 3, 0, 2, 4,  // D
    3, 3, 2, 0, 0, 4, 2, 4, 4, 1, 4, 0, 0, 0, 2, 4,  // E
    3, 3, 2, 1, 0, 4, 2, 4, 3, 2, 4, 1, 0, 0, 2, 4,  // F
};

}  // namespace

void CpuGb::Reset() {
    a = f = b = c = d = e = h = l = 0;
    sp = 0xFFFE;
    pc = 0;
    time = 0;
    halted = jammed = false;
}

void CpuGb::Push(uint16_t v) {
    Write(--sp, (uint8_t)(v >> 8));
    Write(--sp, (uint8_t)v);
}

uint16_t CpuGb::Pop() {
    const uint8_t lo = Read(sp++);
    return (uint16_t)(lo | Read(sp++) << 8);
}

void CpuGb::Call(uint16_t addr, uint16_t idle) {
    Push(idle);
    pc = addr;
    halted = jammed = false;
}

void CpuGb::Run(uint32_t end, uint16_t idle) {
    while (time < end) {
        if (pc == idle || halted || jammed) {
            time = end;
            break;
        }
        Step();
    }
}

uint16_t CpuGb::Pair(int i) const {
    switch (i) {
        case 0: return (uint16_t)(b << 8 | c);
        case 1: return (uint16_t)(d << 8 | e);
        case 2: return HL();
        default: return sp;
    }
}

void CpuGb::SetPair(int i, uint16_t v) {
    switch (i) {
        case 0: b = (uint8_t)(v >> 8), c = (uint8_t)v; break;
        case 1: d = (uint8_t)(v >> 8), e = (uint8_t)v; break;
        case 2: SetHL(v); break;
        default: sp = v; break;
    }
}

uint8_t CpuGb::Reg(int i) {
    switch (i) {
        case 0: return b;
        case 1: return c;
        case 2: return d;
        case 3: return e;
        case 4: return h;
        case 5: return l;
        case 6: return Read(HL());
        default: return a;
    }
}

void CpuGb::SetReg(int i, uint8_t v) {
    switch (i) {
        case 0: b = v; break;
        case 1: c = v; break;
        case 2: d = v; break;
        case 3: e = v; break;
        case 4: h = v; break;
        case 5: l = v; break;
        case 6: Write(HL(), v); break;
        default: a = v; break;
    }
}

bool CpuGb::Condition(int cc) const {
    switch (cc) {
        case 0: return !(f & FZ);
        case 1: return (f & FZ) != 0;
        case 2: return !(f & FC);
        default: return (f & FC) != 0;
    }
}

// ADD ADC SUB SBC AND XOR OR CP
void CpuGb::Alu(int op, uint8_t v) {
    const int carry = (op == 1 || op == 3) && (f & FC) ? 1 : 0;
    int r;
    switch (op) {
        case 0:
        case 1:
            r = a + v + carry;
            f = (uint8_t)(((r & 0xFF) ? 0 : FZ) | (((a & 15) + (v & 15) + carry) > 15 ? FH : 0) | (r > 0xFF ? FC : 0));
            a = (uint8_t)r;
            break;
        case 2:
        case 3:
        case 7:
            r = a - v - carry;
            f = (uint8_t)(FN | ((r & 0xFF) ? 0 : FZ) | ((a & 15) < (v & 15) + carry ? FH : 0) | (r < 0 ? FC : 0));
            if (op != 7) a = (uint8_t)r;
            break;
        case 4: a &= v; f = (uint8_t)((a ? 0 : FZ) | FH); break;
        case 5: a ^= v; f = a ? 0 : FZ; break;
        default: a |= v; f = a ? 0 : FZ; break;
    }
}

// RLC RRC RL RR SLA SRA SWAP SRL
uint8_t CpuGb::CbShift(int op, uint8_t v) {
    const int carryIn = (f & FC) ? 1 : 0;
    int carry = 0;
    uint8_t r;
    switch (op) {
        case 0: carry = v >> 7; r = (uint8_t)(v << 1 | carry); break;
        case 1: carry = v & 1; r = (uint8_t)(v >> 1 | carry << 7); break;
        case 2: carry = v >> 7; r = (uint8_t)(v << 1 | carryIn); break;
        case 3: carry = v & 1; r = (uint8_t)(v >> 1 | carryIn << 7); break;
        case 4: carry = v >> 7; r = (uint8_t)(v << 1); break;
        case 5: carry = v & 1; r = (uint8_t)((v >> 1) | (v & 0x80)); break;
        case 6: r = (uint8_t)(v << 4 | v >> 4); break;
        default: carry = v & 1; r = v >> 1; break;
    }
    f = (uint8_t)((r ? 0 : FZ) | (carry ? FC : 0));
    return r;
}

void CpuGb::Step() {
    const uint8_t op = Fetch();
    Cycles(kCycles[op]);

    if (op >= 0x40 && op < 0x80) {
        if (op == 0x76) halted = true;
        else SetReg((op >> 3) & 7, Reg(op & 7));
        return;
    }
    if (op >= 0x80 && op < 0xC0) {
        Alu((op >> 3) & 7, Reg(op & 7));
        return;
    }
    if (op < 0x40) {
        const int r = (op >> 3) & 7;  // register for INC/DEC/LD r,d8
        const int rp = op >> 4;       // register pair
        switch (op & 15) {
            case 0x4:
            case 0xC: {
                const uint8_t v = (uint8_t)(Reg(r) + 1);
                SetReg(r, v);
                f = (uint8_t)((f & FC) | (v ? 0 : FZ) | ((v & 15) == 0 ? FH : 0));
                return;
            }
            case 0x5:
            case 0xD: {
                const uint8_t v = (uint8_t)(Reg(r) - 1);
                SetReg(r, v);
                f = (uint8_t)((f & FC) | FN | (v ? 0 : FZ) | ((v & 15) == 15 ? FH : 0));
                return;
            }
            case 0x6:
            case 0xE: SetReg(r, Fetch()); return;
            case 0x1: SetPair(rp, Fetch16()); return;
            case 0x3: SetPair(rp, (uint16_t)(Pair(rp) + 1)); return;
            case 0xB: SetPair(rp, (uint16_t)(Pair(rp) - 1)); return;
            case 0x9: {
                const uint16_t hl = HL(), v = Pair(rp);
                const int sum = hl + v;
                f = (uint8_t)((f & FZ) | (((hl & 0xFFF) + (v & 0xFFF)) > 0xFFF ? FH : 0) | (sum > 0xFFFF ? FC : 0));
                SetHL((uint16_t)sum);
                return;
            }
            case 0x2:
            case 0xA: {
                uint16_t addr;
                switch (rp) {
                    case 0: addr = Pair(0); break;
                    case 1: addr = Pair(1); break;
                    case 2: addr = HL(); SetHL((uint16_t)(addr + 1)); break;
                    default: addr = HL(); SetHL((uint16_t)(addr - 1)); break;
                }
                if (op & 8) a = Read(addr);
                else Write(addr, a);
                return;
            }
            default: break;
        }
        switch (op) {
            case 0x00: return;
            case 0x08: {
                const uint16_t addr = Fetch16();
                Write(addr, (uint8_t)sp);
                Write((uint16_t)(addr + 1), (uint8_t)(sp >> 8));
                return;
            }
            case 0x10: return;  // STOP: nothing to stop here
            case 0x18: {
                const int8_t off = (int8_t)Fetch();
                pc = (uint16_t)(pc + off);
                return;
            }
            case 0x20: case 0x28: case 0x30: case 0x38: {
                const int8_t off = (int8_t)Fetch();
                if (Condition((op >> 3) & 3)) {
                    pc = (uint16_t)(pc + off);
                    Cycles(1);
                }
                return;
            }
            case 0x07: { const int cy = a >> 7; a = (uint8_t)(a << 1 | cy); f = cy ? FC : 0; return; }
            case 0x0F: { const int cy = a & 1; a = (uint8_t)(a >> 1 | cy << 7); f = cy ? FC : 0; return; }
            case 0x17: { const int cy = a >> 7; a = (uint8_t)(a << 1 | ((f & FC) ? 1 : 0)); f = cy ? FC : 0; return; }
            case 0x1F: { const int cy = a & 1; a = (uint8_t)(a >> 1 | ((f & FC) ? 0x80 : 0)); f = cy ? FC : 0; return; }
            case 0x27: {  // DAA
                int v = a;
                if (!(f & FN)) {
                    if ((f & FC) || v > 0x99) {
                        v += 0x60;
                        f |= FC;
                    }
                    if ((f & FH) || (v & 15) > 9) v += 6;
                } else {
                    if (f & FC) v -= 0x60;
                    if (f & FH) v -= 6;
                }
                a = (uint8_t)v;
                f = (uint8_t)((f & (FN | FC)) | (a ? 0 : FZ));
                return;
            }
            case 0x2F: a = (uint8_t)~a; f |= FN | FH; return;
            case 0x37: f = (uint8_t)((f & FZ) | FC); return;
            case 0x3F: f = (uint8_t)((f & (FZ | FC)) ^ FC); return;
            default: return;
        }
    }

    // 0xC0-0xFF
    switch (op) {
        case 0xC0: case 0xC8: case 0xD0: case 0xD8:
            if (Condition((op >> 3) & 3)) {
                pc = Pop();
                Cycles(3);
            }
            return;
        case 0xC9: case 0xD9: pc = Pop(); return;  // RET, RETI
        case 0xC2: case 0xCA: case 0xD2: case 0xDA: {
            const uint16_t addr = Fetch16();
            if (Condition((op >> 3) & 3)) {
                pc = addr;
                Cycles(1);
            }
            return;
        }
        case 0xC3: pc = Fetch16(); return;
        case 0xE9: pc = HL(); return;
        case 0xC4: case 0xCC: case 0xD4: case 0xDC: {
            const uint16_t addr = Fetch16();
            if (Condition((op >> 3) & 3)) {
                Push(pc);
                pc = addr;
                Cycles(3);
            }
            return;
        }
        case 0xCD: {
            const uint16_t addr = Fetch16();
            Push(pc);
            pc = addr;
            return;
        }
        case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
            Push(pc);
            pc = (uint16_t)(rstBase + (op & 0x38));
            return;
        case 0xC1: case 0xD1: case 0xE1: SetPair((op >> 4) & 3, Pop()); return;
        case 0xF1: {
            const uint16_t v = Pop();
            a = (uint8_t)(v >> 8);
            f = (uint8_t)(v & 0xF0);
            return;
        }
        case 0xC5: case 0xD5: case 0xE5: Push(Pair((op >> 4) & 3)); return;
        case 0xF5: Push((uint16_t)(a << 8 | f)); return;
        case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
            Alu((op >> 3) & 7, Fetch());
            return;
        case 0xE0: Write((uint16_t)(0xFF00 | Fetch()), a); return;
        case 0xF0: a = Read((uint16_t)(0xFF00 | Fetch())); return;
        case 0xE2: Write((uint16_t)(0xFF00 | c), a); return;
        case 0xF2: a = Read((uint16_t)(0xFF00 | c)); return;
        case 0xEA: Write(Fetch16(), a); return;
        case 0xFA: a = Read(Fetch16()); return;
        case 0xE8:
        case 0xF8: {
            const uint8_t raw = Fetch();
            const uint16_t r = (uint16_t)(sp + (int8_t)raw);
            f = (uint8_t)((((sp & 15) + (raw & 15)) > 15 ? FH : 0) | (((sp & 0xFF) + raw) > 0xFF ? FC : 0));
            if (op == 0xE8) sp = r;
            else SetHL(r);
            return;
        }
        case 0xF9: sp = HL(); return;
        case 0xF3: case 0xFB: return;  // DI, EI: no interrupts here
        case 0xCB: {
            const uint8_t cb = Fetch();
            const int r = cb & 7, bit = (cb >> 3) & 7;
            const bool hl = r == 6;
            switch (cb >> 6) {
                case 0:
                    SetReg(r, CbShift(bit, Reg(r)));
                    Cycles(hl ? 3 : 1);
                    return;
                case 1:
                    f = (uint8_t)((f & FC) | FH | ((Reg(r) >> bit) & 1 ? 0 : FZ));
                    Cycles(hl ? 2 : 1);
                    return;
                case 2:
                    SetReg(r, (uint8_t)(Reg(r) & ~(1 << bit)));
                    Cycles(hl ? 3 : 1);
                    return;
                default:
                    SetReg(r, (uint8_t)(Reg(r) | 1 << bit));
                    Cycles(hl ? 3 : 1);
                    return;
            }
        }
        default:  // opcodes that don't exist lock up the CPU
            jammed = true;
            --pc;
            return;
    }
}
