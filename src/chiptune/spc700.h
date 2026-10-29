// The SNES sound CPU (Sony SPC700), timed per instruction at 1.024 MHz.
#pragma once
#include <cstdint>

class Spc700 {
public:
    struct Bus {
        virtual uint8_t Read(uint16_t addr) = 0;
        virtual void Write(uint16_t addr, uint8_t value) = 0;
    };

    explicit Spc700(Bus& bus) : bus_(bus) {}

    // Executes one instruction and returns its cycles.
    int Step();

    uint16_t pc = 0;
    uint8_t a = 0, x = 0, y = 0, sp = 0xEF, psw = 0x02;
    bool halted = false;  // SLEEP and STOP wait for an interrupt the SNES never sends

private:
    enum : uint8_t { C = 1, Z = 2, I = 4, H = 8, B = 16, P = 32, V = 64, N = 128 };

    uint8_t Read(uint16_t addr) { return bus_.Read(addr); }
    void Write(uint16_t addr, uint8_t v) { bus_.Write(addr, v); }
    // Stores read the destination first, as the hardware does (it matters for the timer
    // counters at $FD-$FF, which clear when read).
    void Store(uint16_t addr, uint8_t v) {
        Read(addr);
        Write(addr, v);
    }
    uint8_t Fetch() { return Read(pc++); }
    uint16_t Fetch16() {
        const uint8_t lo = Fetch();
        return (uint16_t)(lo | Fetch() << 8);
    }
    // Direct-page base: $0000 or $0100, selected by the P flag.
    uint16_t Dp(uint8_t offset) const { return (uint16_t)(((psw & P) ? 0x100 : 0) | offset); }
    uint16_t ReadWordDp(uint8_t offset) {
        const uint8_t lo = Read(Dp(offset));
        return (uint16_t)(lo | Read(Dp((uint8_t)(offset + 1))) << 8);
    }
    uint16_t ReadWord(uint16_t addr) {
        const uint8_t lo = Read(addr);
        return (uint16_t)(lo | Read((uint16_t)(addr + 1)) << 8);
    }
    void Push(uint8_t v) { Write((uint16_t)(0x100 | sp--), v); }
    uint8_t Pop() { return Read((uint16_t)(0x100 | ++sp)); }
    void Push16(uint16_t v) {
        Push((uint8_t)(v >> 8));
        Push((uint8_t)v);
    }
    uint16_t Pop16() {
        const uint8_t lo = Pop();
        return (uint16_t)(lo | Pop() << 8);
    }
    void SetNZ(uint8_t v) { psw = (uint8_t)((psw & ~(N | Z)) | (v & N) | (v ? 0 : Z)); }
    void SetFlag(uint8_t f, bool on) { psw = on ? (uint8_t)(psw | f) : (uint8_t)(psw & ~f); }
    uint16_t YA() const { return (uint16_t)(y << 8 | a); }

    uint8_t Alu(int op, uint8_t l, uint8_t r);  // OR AND EOR CMP ADC SBC
    uint8_t Shift(int op, uint8_t v);           // ASL ROL LSR ROR
    int Branch(bool taken);                     // returns the extra cycles

    Bus& bus_;
};
