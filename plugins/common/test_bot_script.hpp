// TestBot pure parts (no game, no SDL, no Steam calls): the step-script parser,
// SDL key names, and the decoder for steam_api64.dll's flat ISteamUserStats
// wrappers. Header-only so tests/tests.cpp can exercise it directly.
//
// Script format: one command per line; '#' starts a comment; blank lines are
// ignored; keywords are case-insensitive. Times are milliseconds.
//   wait <ms>                       pause
//   wait_world [timeout_ms]         until the player is in the game world (default 120000; timeout aborts the run)
//   key <key> [hold_ms]             press and release (default hold 50)
//   key_down <key> / key_up <key>   half a key press
//   mod <none|ctrl|shift|alt>[+...] set SDL's modifier state (SDL_SetModState)
//   wheel <notches>                 mouse wheel, +up / -down, one notch per frame
//   click <x> <y> [left|right|middle]  move the mouse there (window pixels) and click
//   expect_bar_set <n>              check the current ability-bar set (AbilityUser+0x68)
//   log <text>                      write a marker line to the diagnostics log
// <key>: a-z, 0-9, f1-f12, space, enter|return, escape|esc, tab, backspace,
// up, down, left, right, lctrl, rctrl, lshift, rshift, lalt, ralt, or an SDL
// keycode in decimal or 0x hex.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace soulash_plugins::testbot
{
    enum class Op
    {
        Wait,
        WaitWorld,
        Key,
        KeyDown,
        KeyUp,
        Mod,
        Wheel,
        Click,
        ExpectBarSet,
        Log
    };

    struct Step
    {
        Op op = Op::Wait;
        int line = 0;
        std::int64_t a = 0;   // ms / notches / x / expected set / modifier mask
        std::int64_t b = 0;   // hold ms / y
        std::int64_t c = 0;   // mouse button
        std::uint32_t key = 0;
        std::string text;     // log text
    };

    struct ParseResult
    {
        bool ok = false;
        int errorLine = 0;
        std::string error;
        std::vector<Step> steps;
    };

    constexpr std::size_t MaxScriptBytes = 64 * 1024;
    constexpr std::size_t MaxSteps = 2000;
    constexpr std::size_t MaxLineLength = 256;
    constexpr std::int64_t MaxWaitMs = 10 * 60 * 1000;

    constexpr int ModNone = 0x0000;
    constexpr int ModLShift = 0x0001;
    constexpr int ModLCtrl = 0x0040;
    constexpr int ModLAlt = 0x0100;

    inline std::string Lower(std::string_view text)
    {
        std::string out(text);
        for (char& c : out)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    inline bool ParseInteger(std::string_view text, std::int64_t& value)
    {
        if (text.empty() || text.size() > 20)
        {
            return false;
        }
        const std::string copy(text);
        char* end = nullptr;
        const bool hex = copy.size() > 2 && copy[0] == '0' && (copy[1] == 'x' || copy[1] == 'X');
        const long long parsed = hex ? static_cast<long long>(std::strtoull(copy.c_str() + 2, &end, 16))
                                     : std::strtoll(copy.c_str(), &end, 10);
        if (end == nullptr || *end != '\0' || (hex && copy.size() == 2))
        {
            return false;
        }
        value = parsed;
        return true;
    }

    // SDL2 keycodes (SDL_Keycode): printable keys are their ASCII value,
    // others are scancode | (1 << 30).
    inline bool ParseKeyName(std::string_view name, std::uint32_t& keycode)
    {
        const std::string key = Lower(name);
        constexpr std::uint32_t Scan = 1u << 30;
        if (key.size() == 1 && ((key[0] >= 'a' && key[0] <= 'z') || (key[0] >= '0' && key[0] <= '9')))
        {
            keycode = static_cast<unsigned char>(key[0]);
            return true;
        }
        if (key.size() >= 2 && key.size() <= 3 && key[0] == 'f')
        {
            std::int64_t n = 0;
            if (ParseInteger(std::string_view(key).substr(1), n) && n >= 1 && n <= 12)
            {
                keycode = Scan | static_cast<std::uint32_t>(58 + n - 1);   // SDL_SCANCODE_F1 = 58
                return true;
            }
            return false;
        }
        struct Named
        {
            const char* name;
            std::uint32_t code;
        };
        static const Named names[] = {
            {"space", 32}, {"enter", 13}, {"return", 13}, {"escape", 27}, {"esc", 27}, {"tab", 9},
            {"backspace", 8}, {"right", Scan | 79}, {"left", Scan | 80}, {"down", Scan | 81}, {"up", Scan | 82},
            {"lctrl", Scan | 224}, {"lshift", Scan | 225}, {"lalt", Scan | 226},
            {"rctrl", Scan | 228}, {"rshift", Scan | 229}, {"ralt", Scan | 230},
        };
        for (const Named& entry : names)
        {
            if (key == entry.name)
            {
                keycode = entry.code;
                return true;
            }
        }
        std::int64_t numeric = 0;
        if (ParseInteger(key, numeric) && numeric > 0 && numeric <= 0x7FFFFFFF)
        {
            keycode = static_cast<std::uint32_t>(numeric);
            return true;
        }
        return false;
    }

    // "none", "ctrl", "ctrl+shift", ... -> SDL_Keymod (left keys); -1 if invalid.
    inline int ParseModMask(std::string_view text)
    {
        const std::string mods = Lower(text);
        if (mods == "none")
        {
            return ModNone;
        }
        int mask = 0;
        std::size_t start = 0;
        while (start <= mods.size())
        {
            const std::size_t plus = mods.find('+', start);
            const std::string part = mods.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
            if (part == "ctrl")
            {
                mask |= ModLCtrl;
            }
            else if (part == "shift")
            {
                mask |= ModLShift;
            }
            else if (part == "alt")
            {
                mask |= ModLAlt;
            }
            else
            {
                return -1;
            }
            if (plus == std::string::npos)
            {
                break;
            }
            start = plus + 1;
        }
        return mask;
    }

    inline std::vector<std::string_view> SplitWords(std::string_view line)
    {
        std::vector<std::string_view> words;
        std::size_t i = 0;
        while (i < line.size())
        {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            {
                ++i;
            }
            const std::size_t start = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t')
            {
                ++i;
            }
            if (i > start)
            {
                words.push_back(line.substr(start, i - start));
            }
        }
        return words;
    }

    inline ParseResult ParseScript(std::string_view text)
    {
        ParseResult result;
        auto fail = [&result](int line, const std::string& message) {
            result.ok = false;
            result.errorLine = line;
            result.error = message;
            result.steps.clear();
            return result;
        };
        if (text.size() > MaxScriptBytes)
        {
            return fail(0, "script larger than 64 KiB");
        }
        int lineNumber = 0;
        std::size_t position = 0;
        while (position <= text.size())
        {
            const std::size_t end = text.find('\n', position);
            std::string_view line = text.substr(position, end == std::string_view::npos ? std::string_view::npos : end - position);
            position = end == std::string_view::npos ? text.size() + 1 : end + 1;
            ++lineNumber;
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            if (line.size() > MaxLineLength)
            {
                return fail(lineNumber, "line longer than 256 characters");
            }
            const std::size_t hash = line.find('#');
            const std::string_view code = hash == std::string_view::npos ? line : line.substr(0, hash);
            const std::vector<std::string_view> words = SplitWords(code);
            if (words.empty())
            {
                continue;
            }
            if (result.steps.size() >= MaxSteps)
            {
                return fail(lineNumber, "more than 2000 steps");
            }
            const std::string command = Lower(words[0]);
            Step step;
            step.line = lineNumber;
            const std::size_t args = words.size() - 1;
            auto integer = [&](std::size_t index, std::int64_t& value, std::int64_t low, std::int64_t high) {
                return index < words.size() && ParseInteger(words[index], value) && value >= low && value <= high;
            };
            if (command == "wait")
            {
                step.op = Op::Wait;
                if (args != 1 || !integer(1, step.a, 0, MaxWaitMs))
                {
                    return fail(lineNumber, "wait <ms 0..600000>");
                }
            }
            else if (command == "wait_world")
            {
                step.op = Op::WaitWorld;
                step.a = 120000;
                if (args > 1 || (args == 1 && !integer(1, step.a, 1, MaxWaitMs)))
                {
                    return fail(lineNumber, "wait_world [timeout_ms 1..600000]");
                }
            }
            else if (command == "key" || command == "key_down" || command == "key_up")
            {
                step.op = command == "key" ? Op::Key : (command == "key_down" ? Op::KeyDown : Op::KeyUp);
                step.b = 50;
                const bool holdAllowed = step.op == Op::Key;
                if (args < 1 || args > (holdAllowed ? 2u : 1u) || !ParseKeyName(words[1], step.key))
                {
                    return fail(lineNumber, command + " <key>" + (holdAllowed ? " [hold_ms]" : "") + " (unknown key?)");
                }
                if (args == 2 && !integer(2, step.b, 0, 60000))
                {
                    return fail(lineNumber, "key hold must be 0..60000 ms");
                }
            }
            else if (command == "mod")
            {
                step.op = Op::Mod;
                const int mask = args == 1 ? ParseModMask(words[1]) : -1;
                if (mask < 0)
                {
                    return fail(lineNumber, "mod <none|ctrl|shift|alt>[+...]");
                }
                step.a = mask;
            }
            else if (command == "wheel")
            {
                step.op = Op::Wheel;
                if (args != 1 || !integer(1, step.a, -100, 100) || step.a == 0)
                {
                    return fail(lineNumber, "wheel <notches -100..100, not 0>");
                }
            }
            else if (command == "click")
            {
                step.op = Op::Click;
                step.c = 1;
                if (args < 2 || args > 3 || !integer(1, step.a, 0, 16384) || !integer(2, step.b, 0, 16384))
                {
                    return fail(lineNumber, "click <x> <y> [left|right|middle]");
                }
                if (args == 3)
                {
                    const std::string button = Lower(words[3]);
                    step.c = button == "left" ? 1 : (button == "middle" ? 2 : (button == "right" ? 3 : 0));
                    if (step.c == 0)
                    {
                        return fail(lineNumber, "click button must be left, right or middle");
                    }
                }
            }
            else if (command == "expect_bar_set")
            {
                step.op = Op::ExpectBarSet;
                if (args != 1 || !integer(1, step.a, 0, 63))
                {
                    return fail(lineNumber, "expect_bar_set <0..63>");
                }
            }
            else if (command == "log")
            {
                step.op = Op::Log;
                const std::size_t at = static_cast<std::size_t>(words[0].data() - code.data()) + words[0].size();
                std::string_view rest = code.substr(at);
                while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t'))
                {
                    rest.remove_prefix(1);
                }
                while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t'))
                {
                    rest.remove_suffix(1);
                }
                if (rest.empty())
                {
                    return fail(lineNumber, "log <text>");
                }
                for (const char c : rest)
                {
                    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E)
                    {
                        return fail(lineNumber, "log text must be printable ASCII");
                    }
                }
                step.text.assign(rest);
            }
            else
            {
                return fail(lineNumber, "unknown command '" + std::string(words[0]) + "'");
            }
            result.steps.push_back(std::move(step));
        }
        result.ok = true;
        return result;
    }

    // steam_api64.dll's flat wrappers are `mov rax,[rcx]; jmp qword ptr [rax+d]`
    // (48 8B 01 48 FF 60 d8 | 48 FF A0 d32). Returns the vtable byte offset, or
    // -1 for anything else.
    inline int DecodeVtableJump(const unsigned char* code)
    {
        if (code == nullptr || code[0] != 0x48 || code[1] != 0x8B || code[2] != 0x01 || code[3] != 0x48
            || code[4] != 0xFF)
        {
            return -1;
        }
        int offset = -1;
        if (code[5] == 0x60)
        {
            offset = code[6];
        }
        else if (code[5] == 0xA0)
        {
            std::int32_t d32;
            std::memcpy(&d32, code + 6, sizeof(d32));
            offset = d32;
        }
        else if (code[5] == 0x20)
        {
            offset = 0;
        }
        if (offset < 0 || offset % 8 != 0 || offset >= 0x800)
        {
            return -1;
        }
        return offset;
    }
}
