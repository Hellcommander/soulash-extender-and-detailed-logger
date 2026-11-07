#pragma once

#include <cstddef>
#include <cstdint>

namespace soulash::x64
{
    // How an instruction transfers control.
    enum class Flow : std::uint8_t
    {
        Sequential,
        Call,             // call rel32 / call r/m
        Jump,             // jmp rel8 / rel32 / r/m (unconditional)
        ConditionalJump,  // jcc rel8 / rel32
        Return,           // ret, retf, iret
        Trap,             // int3, ud2, hlt, int n, syscall-like stops
        Other             // loop*, jrcxz, xbegin: relative but not relocatable here
    };

    struct Decoded
    {
        unsigned length = 0;
        Flow flow = Flow::Sequential;

        // rel8 / rel32 branch operand (call, jmp, jcc, loop, jrcxz, xbegin).
        bool relative = false;
        unsigned relativeOffset = 0;  // offset of the displacement in the instruction
        unsigned relativeSize = 0;    // 1 or 4

        // ModRM operand addressed relative to RIP (disp32 follows the ModRM byte).
        bool ripRelative = false;
        unsigned ripDisplacementOffset = 0;

        // Opcode (without prefixes). twoByte means it followed 0x0F.
        bool twoByte = false;
        std::uint8_t opcode = 0;
    };

    // Decodes one x64 (64-bit mode) instruction starting at `code`, reading at
    // most `available` bytes. Returns false for truncated input, opcodes that
    // are invalid in 64-bit mode, or encodings this decoder does not know.
    // It never reads past `available`. It computes lengths and control-flow
    // facts only; it does not disassemble.
    bool Decode(const std::uint8_t* code, std::size_t available, Decoded& decoded);
}
