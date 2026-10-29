// The NES's 6502 core (the 2A03 has no decimal mode; the C64's 6510 sets `decimal`), with the
// stable undocumented opcodes that NSF and SID rips sometimes use. Timing is per instruction,
// which is all music playback needs.
#pragma once
#include <cstdint>

class Cpu6502 {
public:
    struct Bus {
        virtual uint8_t Read(uint16_t addr) = 0;
        // `time` is the CPU cycle in the current frame at which the write happens.
        virtual void Write(uint16_t addr, uint8_t value, uint32_t time) = 0;
    };

    explicit Cpu6502(Bus& bus) : bus_(bus) {}

    void Reset();
    // Calls a routine: it runs until it returns to `idle` (an address nothing executes).
    void Call(uint16_t addr, uint16_t idle);
    // Runs instructions until `end`, or until the CPU reaches `idle` or jams; then the time
    // jumps to `end`.
    void Run(uint32_t end, uint16_t idle);
    // Executes one instruction.
    void Step();
    // Takes an IRQ or NMI through the vector at `vector`, as the hardware does.
    void Interrupt(uint16_t vector);
    bool InterruptsMasked() const { return (p & I) != 0; }
    void MaskInterrupts(bool on) { SetFlag(I, on); }

    uint16_t pc = 0;
    uint8_t a = 0, x = 0, y = 0, s = 0xFD, p = 0x24;
    uint32_t time = 0;  // cycles into the current frame
    bool jammed = false;
    bool decimal = false;  // ADC and SBC honour the D flag

private:
    // Status-register bits, C at bit 0 through N at bit 7; U is the always-set unused bit.
    enum : uint8_t { C = 1, Z = 2, I = 4, D = 8, B = 16, U = 32, V = 64, N = 128 };

    uint8_t Read(uint16_t addr) { return bus_.Read(addr); }
    void Write(uint16_t addr, uint8_t v) { bus_.Write(addr, v, time); }
    uint8_t Fetch() { return Read(pc++); }
    void Push(uint8_t v) { Write((uint16_t)(0x100 | s--), v); }
    uint8_t Pull() { return Read((uint16_t)(0x100 | ++s)); }
    void SetNZ(uint8_t v) { p = (uint8_t)((p & ~(N | Z)) | (v & N) | (v ? 0 : Z)); }
    void SetFlag(uint8_t f, bool on) { p = on ? (uint8_t)(p | f) : (uint8_t)(p & ~f); }

    void Adc(uint8_t m);
    void Sbc(uint8_t m);
    void Compare(uint8_t reg, uint8_t m);
    uint8_t Asl(uint8_t m);
    uint8_t Lsr(uint8_t m);
    uint8_t Rol(uint8_t m);
    uint8_t Ror(uint8_t m);
    void Branch(bool taken);

    Bus& bus_;
};
