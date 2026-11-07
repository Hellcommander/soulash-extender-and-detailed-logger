#include "x64_length.hpp"

namespace soulash::x64
{
    namespace
    {
        enum class Imm : std::uint8_t
        {
            None,
            Byte,
            Word,        // 16-bit (ret imm16, enter's first operand)
            WordByte,    // enter
            Z,           // 16 with 0x66 (no REX.W), else 32
            V,           // mov r, imm: 64 with REX.W, 16 with 0x66, else 32
            Moffs,       // 64-bit address, 32 with 0x67
            Rel8,
            Rel32
        };

        struct OpInfo
        {
            bool valid = false;
            bool modrm = false;
            Imm imm = Imm::None;
        };

        constexpr OpInfo Invalid{};
        constexpr OpInfo Plain{true, false, Imm::None};
        constexpr OpInfo Modrm{true, true, Imm::None};

        constexpr OpInfo WithImm(Imm imm) { return {true, false, imm}; }
        constexpr OpInfo ModrmImm(Imm imm) { return {true, true, imm}; }

        OpInfo OneByte(std::uint8_t op)
        {
            if (op < 0x40)
            {
                const unsigned low = op & 7u;
                if (low < 4) return Modrm;
                if (low == 4) return WithImm(Imm::Byte);
                if (low == 5) return WithImm(Imm::Z);
                return Invalid; // push/pop seg, daa/das/aaa/aas are invalid in 64-bit mode
            }
            if (op < 0x50) return Invalid;               // REX, consumed as a prefix
            if (op < 0x60) return Plain;                 // push/pop r64
            switch (op)
            {
            case 0x60: case 0x61: case 0x62: return Invalid;
            case 0x63: return Modrm;
            case 0x68: return WithImm(Imm::Z);
            case 0x69: return ModrmImm(Imm::Z);
            case 0x6A: return WithImm(Imm::Byte);
            case 0x6B: return ModrmImm(Imm::Byte);
            case 0x6C: case 0x6D: case 0x6E: case 0x6F: return Plain;
            default: break;
            }
            if (op >= 0x70 && op <= 0x7F) return WithImm(Imm::Rel8);
            switch (op)
            {
            case 0x80: case 0x83: return ModrmImm(Imm::Byte);
            case 0x81: return ModrmImm(Imm::Z);
            case 0x82: return Invalid;
            default: break;
            }
            if (op >= 0x84 && op <= 0x8F) return Modrm;
            if (op >= 0x90 && op <= 0x9F) return op == 0x9A ? Invalid : Plain;
            if (op >= 0xA0 && op <= 0xA3) return WithImm(Imm::Moffs);
            if (op >= 0xA4 && op <= 0xA7) return Plain;
            if (op == 0xA8) return WithImm(Imm::Byte);
            if (op == 0xA9) return WithImm(Imm::Z);
            if (op >= 0xAA && op <= 0xAF) return Plain;
            if (op >= 0xB0 && op <= 0xB7) return WithImm(Imm::Byte);
            if (op >= 0xB8 && op <= 0xBF) return WithImm(Imm::V);
            switch (op)
            {
            case 0xC0: case 0xC1: return ModrmImm(Imm::Byte);
            case 0xC2: return WithImm(Imm::Word);
            case 0xC3: return Plain;
            case 0xC6: return ModrmImm(Imm::Byte);
            case 0xC7: return ModrmImm(Imm::Z);
            case 0xC8: return WithImm(Imm::WordByte);
            case 0xC9: return Plain;
            case 0xCA: return WithImm(Imm::Word);
            case 0xCB: case 0xCC: return Plain;
            case 0xCD: return WithImm(Imm::Byte);
            case 0xCE: return Invalid;
            case 0xCF: return Plain;
            default: break;
            }
            if (op >= 0xD0 && op <= 0xD3) return Modrm;
            if (op >= 0xD4 && op <= 0xD6) return Invalid;
            if (op == 0xD7) return Plain;
            if (op >= 0xD8 && op <= 0xDF) return Modrm;
            if (op >= 0xE0 && op <= 0xE3) return WithImm(Imm::Rel8);
            if (op >= 0xE4 && op <= 0xE7) return WithImm(Imm::Byte);
            if (op == 0xE8 || op == 0xE9) return WithImm(Imm::Rel32);
            if (op == 0xEA) return Invalid;
            if (op == 0xEB) return WithImm(Imm::Rel8);
            if (op >= 0xEC && op <= 0xEF) return Plain;
            switch (op)
            {
            case 0xF1: case 0xF4: case 0xF5: return Plain;
            case 0xF6: case 0xF7: return Modrm; // immediate depends on the reg field
            case 0xFE: case 0xFF: return Modrm;
            default: break;
            }
            if (op >= 0xF8 && op <= 0xFD) return Plain;
            return Invalid;
        }

        OpInfo TwoByte(std::uint8_t op)
        {
            switch (op)
            {
            case 0x00: case 0x01: case 0x02: case 0x03: return Modrm;
            case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0B: return Plain;
            case 0x0D: return Modrm;
            case 0x0E: return Plain;
            case 0x0F: return ModrmImm(Imm::Byte); // 3DNow!: suffix byte is the opcode
            default: break;
            }
            if (op >= 0x10 && op <= 0x1F) return Modrm;
            if (op >= 0x20 && op <= 0x23) return Modrm;
            if (op >= 0x28 && op <= 0x2F) return Modrm;
            if (op >= 0x30 && op <= 0x35) return Plain;
            if (op == 0x37) return Plain;
            if (op >= 0x40 && op <= 0x6F) return Modrm;
            if (op >= 0x70 && op <= 0x73) return ModrmImm(Imm::Byte);
            if (op >= 0x74 && op <= 0x76) return Modrm;
            if (op == 0x77) return Plain;
            if (op >= 0x78 && op <= 0x7F) return Modrm;
            if (op >= 0x80 && op <= 0x8F) return WithImm(Imm::Rel32);
            if (op >= 0x90 && op <= 0x9F) return Modrm;
            switch (op)
            {
            case 0xA0: case 0xA1: case 0xA2: case 0xA8: case 0xA9: case 0xAA: return Plain;
            case 0xA3: case 0xA5: case 0xAB: case 0xAD: case 0xAE: case 0xAF: return Modrm;
            case 0xA4: case 0xAC: return ModrmImm(Imm::Byte);
            case 0xA6: case 0xA7: return Invalid;
            case 0xBA: return ModrmImm(Imm::Byte);
            case 0xC2: case 0xC4: case 0xC5: case 0xC6: return ModrmImm(Imm::Byte);
            default: break;
            }
            if (op >= 0xB0 && op <= 0xBF) return Modrm;
            if (op >= 0xC0 && op <= 0xC7) return Modrm;
            if (op >= 0xC8 && op <= 0xCF) return Plain; // bswap
            if (op >= 0xD0) return Modrm;
            return Invalid;
        }

        bool IsLegacyPrefix(std::uint8_t b)
        {
            switch (b)
            {
            case 0x66: case 0x67: case 0xF0: case 0xF2: case 0xF3:
            case 0x26: case 0x2E: case 0x36: case 0x3E: case 0x64: case 0x65:
                return true;
            default:
                return false;
            }
        }

        struct Cursor
        {
            const std::uint8_t* data;
            std::size_t available;
            std::size_t position = 0;

            bool Has(std::size_t count) const { return position + count <= available; }
        };

        // Consumes ModRM, SIB and displacement. Returns false if truncated.
        bool ConsumeModrm(Cursor& cursor, Decoded& out, unsigned& reg)
        {
            if (!cursor.Has(1)) return false;
            const std::uint8_t modrm = cursor.data[cursor.position];
            const unsigned mod = modrm >> 6;
            const unsigned rm = modrm & 7u;
            reg = (modrm >> 3) & 7u;
            ++cursor.position;
            if (mod == 3) return true;

            std::size_t displacement = 0;
            if (rm == 4)
            {
                if (!cursor.Has(1)) return false;
                const std::uint8_t sib = cursor.data[cursor.position];
                ++cursor.position;
                if (mod == 0 && (sib & 7u) == 5) displacement = 4;
            }
            else if (mod == 0 && rm == 5)
            {
                out.ripRelative = true;
                out.ripDisplacementOffset = static_cast<unsigned>(cursor.position);
                displacement = 4;
            }
            if (mod == 1) displacement = 1;
            else if (mod == 2) displacement = 4;

            if (!cursor.Has(displacement)) return false;
            cursor.position += displacement;
            return true;
        }

        bool ConsumeImmediate(
            Cursor& cursor, Imm imm, bool operandSize16, bool rexW, bool addressSize32, Decoded& out)
        {
            std::size_t size = 0;
            switch (imm)
            {
            case Imm::None: return true;
            case Imm::Byte: size = 1; break;
            case Imm::Word: size = 2; break;
            case Imm::WordByte: size = 3; break;
            case Imm::Z: size = (operandSize16 && !rexW) ? 2 : 4; break;
            case Imm::V: size = rexW ? 8 : (operandSize16 ? 2 : 4); break;
            case Imm::Moffs: size = addressSize32 ? 4 : 8; break;
            case Imm::Rel8: size = 1; break;
            case Imm::Rel32: size = 4; break;
            }
            if (!cursor.Has(size)) return false;
            if (imm == Imm::Rel8 || imm == Imm::Rel32)
            {
                out.relative = true;
                out.relativeOffset = static_cast<unsigned>(cursor.position);
                out.relativeSize = static_cast<unsigned>(size);
            }
            cursor.position += size;
            return true;
        }

        // VEX / EVEX instructions: opcode, ModRM (except vzeroupper/all), imm8 for a few.
        bool DecodeVector(Cursor& cursor, unsigned map, Decoded& out)
        {
            if (!cursor.Has(1)) return false;
            const std::uint8_t opcode = cursor.data[cursor.position++];
            out.opcode = opcode;
            out.twoByte = true;
            if (map == 1 && opcode == 0x77)
            {
                return true;
            }
            unsigned reg = 0;
            if (!ConsumeModrm(cursor, out, reg)) return false;
            const bool hasImm8 = map == 3
                || (map == 1 && ((opcode >= 0x70 && opcode <= 0x73) || opcode == 0xC2
                    || (opcode >= 0xC4 && opcode <= 0xC6)));
            if (hasImm8)
            {
                if (!cursor.Has(1)) return false;
                ++cursor.position;
            }
            return true;
        }
    }

    bool Decode(const std::uint8_t* code, std::size_t available, Decoded& decoded)
    {
        decoded = Decoded{};
        if (code == nullptr || available == 0)
        {
            return false;
        }
        if (available > 15)
        {
            available = 15;
        }

        Cursor cursor{code, available};
        bool operandSize16 = false;
        bool addressSize32 = false;
        bool rexW = false;

        // Legacy prefixes, then at most one REX.
        while (cursor.Has(1) && IsLegacyPrefix(code[cursor.position]))
        {
            operandSize16 = operandSize16 || code[cursor.position] == 0x66;
            addressSize32 = addressSize32 || code[cursor.position] == 0x67;
            ++cursor.position;
        }
        if (cursor.Has(1) && (code[cursor.position] & 0xF0) == 0x40)
        {
            rexW = (code[cursor.position] & 0x08) != 0;
            ++cursor.position;
        }
        if (!cursor.Has(1))
        {
            return false;
        }

        const std::uint8_t first = code[cursor.position++];
        unsigned reg = 0;

        if (first == 0xC5)
        {
            // 2-byte VEX: map 1 implied.
            if (!cursor.Has(1)) return false;
            ++cursor.position;
            if (!DecodeVector(cursor, 1, decoded)) return false;
        }
        else if (first == 0xC4)
        {
            if (!cursor.Has(2)) return false;
            const unsigned map = code[cursor.position] & 0x1Fu;
            cursor.position += 2;
            if (map < 1 || map > 3) return false;
            if (!DecodeVector(cursor, map, decoded)) return false;
        }
        else if (first == 0x62)
        {
            if (!cursor.Has(3)) return false;
            const unsigned map = code[cursor.position] & 0x07u;
            cursor.position += 3;
            if (map < 1 || map > 3) return false;
            if (!DecodeVector(cursor, map, decoded)) return false;
        }
        else if (first == 0x0F)
        {
            if (!cursor.Has(1)) return false;
            const std::uint8_t second = code[cursor.position++];
            decoded.twoByte = true;
            decoded.opcode = second;
            if (second == 0x38 || second == 0x3A)
            {
                if (!cursor.Has(1)) return false;
                ++cursor.position; // third opcode byte
                if (!ConsumeModrm(cursor, decoded, reg)) return false;
                if (second == 0x3A)
                {
                    if (!cursor.Has(1)) return false;
                    ++cursor.position;
                }
            }
            else
            {
                const OpInfo info = TwoByte(second);
                if (!info.valid) return false;
                if (info.modrm && !ConsumeModrm(cursor, decoded, reg)) return false;
                if (!ConsumeImmediate(cursor, info.imm, operandSize16, rexW, addressSize32, decoded))
                {
                    return false;
                }
                if (second >= 0x80 && second <= 0x8F)
                {
                    decoded.flow = Flow::ConditionalJump;
                }
                else if (second == 0x0B)
                {
                    decoded.flow = Flow::Trap;
                }
            }
        }
        else
        {
            const OpInfo info = OneByte(first);
            if (!info.valid) return false;
            decoded.opcode = first;
            if (info.modrm && !ConsumeModrm(cursor, decoded, reg)) return false;

            Imm imm = info.imm;
            if ((first == 0xF6 || first == 0xF7) && reg < 2)
            {
                imm = first == 0xF6 ? Imm::Byte : Imm::Z;
            }
            if (first == 0xFF && reg == 7) return false;
            if (!ConsumeImmediate(cursor, imm, operandSize16, rexW, addressSize32, decoded))
            {
                return false;
            }

            if (first >= 0x70 && first <= 0x7F)
            {
                decoded.flow = Flow::ConditionalJump;
            }
            else if (first == 0xE8)
            {
                decoded.flow = Flow::Call;
            }
            else if (first == 0xE9 || first == 0xEB)
            {
                decoded.flow = Flow::Jump;
            }
            else if (first >= 0xE0 && first <= 0xE3)
            {
                decoded.flow = Flow::Other;
            }
            else if (first == 0xC2 || first == 0xC3 || first == 0xCA || first == 0xCB
                || first == 0xCF)
            {
                decoded.flow = Flow::Return;
            }
            else if (first == 0xCC || first == 0xCD || first == 0xF1 || first == 0xF4)
            {
                decoded.flow = Flow::Trap;
            }
            else if (first == 0xFF)
            {
                if (reg == 2 || reg == 3) decoded.flow = Flow::Call;
                else if (reg == 4 || reg == 5) decoded.flow = Flow::Jump;
            }
            else if (first == 0xC7 && reg == 7)
            {
                // xbegin rel32 (C7 F8): relative, not relocatable here.
                decoded.flow = Flow::Other;
                decoded.relative = true;
                decoded.relativeSize = 4;
                decoded.relativeOffset = static_cast<unsigned>(cursor.position) - 4;
            }
        }

        if (cursor.position > 15)
        {
            return false;
        }
        decoded.length = static_cast<unsigned>(cursor.position);
        return true;
    }
}
