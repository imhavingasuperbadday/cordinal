// The Game Boy's Sharp LR35902 (SM83) CPU. Time is counted in 4.19 MHz clocks; interrupts
// aren't emulated since the GBS player calls the music routines itself.
#pragma once
#include <cstdint>

class CpuGb {
public:
    struct Bus {
        virtual uint8_t Read(uint16_t addr) = 0;
        virtual void Write(uint16_t addr, uint8_t value, uint32_t time) = 0;
    };

    explicit CpuGb(Bus& bus) : bus_(bus) {}

    void Reset();
    // Calls a routine: it runs until it returns to `idle` (an address nothing executes).
    void Call(uint16_t addr, uint16_t idle);
    // Runs until `end`, or until the CPU is idle, halted or jammed; then the time jumps to `end`.
    void Run(uint32_t end, uint16_t idle);
    void Step();

    uint8_t a = 0, f = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0;
    uint16_t sp = 0xFFFE, pc = 0;
    uint16_t rstBase = 0;  // RST n jumps to rstBase + n (GBS files relocate the vectors)
    uint32_t time = 0;
    bool halted = false, jammed = false;
    bool doubleSpeed = false;  // Game Boy Color double speed: instructions take half the time

private:
    // F register flag bits (the low nibble reads back as zero).
    enum : uint8_t { FZ = 0x80, FN = 0x40, FH = 0x20, FC = 0x10 };

    uint8_t Read(uint16_t addr) { return bus_.Read(addr); }
    void Write(uint16_t addr, uint8_t v) { bus_.Write(addr, v, time); }
    uint8_t Fetch() { return Read(pc++); }
    uint16_t Fetch16() {
        const uint8_t lo = Fetch();
        return (uint16_t)(lo | Fetch() << 8);
    }
    void Push(uint16_t v);
    uint16_t Pop();
    uint16_t Pair(int i) const;  // BC, DE, HL, SP
    void SetPair(int i, uint16_t v);
    uint16_t HL() const { return (uint16_t)(h << 8 | l); }
    void SetHL(uint16_t v) { h = (uint8_t)(v >> 8), l = (uint8_t)v; }
    uint8_t Reg(int i);  // B C D E H L (HL) A
    void SetReg(int i, uint8_t v);
    bool Condition(int cc) const;
    void Alu(int op, uint8_t v);
    uint8_t CbShift(int op, uint8_t v);
    void Cycles(int m) { time += doubleSpeed ? m * 2 : m * 4; }

    Bus& bus_;
};
