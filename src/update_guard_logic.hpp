// Update guard: the pure part (no files, no Windows calls) so it can be unit
// tested. src/update_guard.cpp does the I/O around it.
//
// State file (<proxy dir>\SoulashExtender\UpdateGuard.state), one key=value per
// line, rewritten whole at session start, after plugins load and at a clean
// exit:
//   format=1
//   exe_sha256=<64 hex>         game build of the last session
//   game_version=0.10.5.1       version.txt of that build
//   original_sha256=<64 hex>    BugSplat64_original.dll (vendor DLL) at that time
//   started_utc=2026-11-05T10:00:00Z
//   clean_exit=0|1              1 once the game shut down normally
//   plugins_loaded=0|1          1 when native plugins were loaded that session
//   safe_mode=0|1
//   unclean_streak=N            consecutive unclean sessions with plugins loaded
//   known_build=<sha>|<version>|<first seen utc>   (repeated, oldest first, at most 16)
#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace soulash::update_guard
{
    struct KnownBuild
    {
        std::string sha256;
        std::string version;
        std::string firstSeenUtc;
    };

    struct State
    {
        std::string exeSha256;
        std::string gameVersion;
        std::string originalSha256;
        std::string startedUtc;
        bool cleanExit = false;
        bool pluginsLoaded = false;
        bool safeMode = false;
        unsigned uncleanStreak = 0;
        std::vector<KnownBuild> knownBuilds;
    };

    struct Config
    {
        unsigned safeModeAfterUncleanExits = 2;   // 0 = never enter safe mode automatically
        bool forceSafeMode = false;
    };

    struct Decision
    {
        bool firstStart = true;          // no usable previous state
        bool gameUpdated = false;        // exe hash differs from the previous session
        bool firstRunOnBuild = false;    // this exe hash was never seen before
        bool vendorDllChanged = false;   // BugSplat64_original.dll differs from the previous session
        std::string previousSha256;
        std::string previousVersion;
        bool previousClean = true;
        bool previousHadPlugins = false;
        unsigned uncleanStreak = 0;
        bool safeMode = false;
        std::string safeModeReason;
        State next;                      // what this session starts with
    };

    constexpr std::size_t MaxKnownBuilds = 16;
    constexpr std::size_t MaxStateBytes = 16 * 1024;

    inline bool IsHex64(std::string_view text)
    {
        return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    }

    // Short printable values only (versions, timestamps); anything else is dropped.
    inline bool IsSafeValue(std::string_view text, std::size_t maximum)
    {
        return text.size() <= maximum && std::all_of(text.begin(), text.end(), [](char c) {
            return c >= 0x20 && c <= 0x7E && c != '|';
        });
    }

    // Tolerant parser: unknown keys are ignored, malformed values are dropped.
    // Returns false only when the text is not a format=1 state file at all.
    inline bool ParseState(std::string_view text, State& state)
    {
        state = State{};
        if (text.size() > MaxStateBytes)
        {
            return false;
        }
        bool sawFormat = false;
        std::size_t position = 0;
        while (position < text.size())
        {
            std::size_t end = text.find('\n', position);
            if (end == std::string_view::npos)
            {
                end = text.size();
            }
            std::string_view line = text.substr(position, end - position);
            position = end + 1;
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            const std::size_t equals = line.find('=');
            if (equals == std::string_view::npos)
            {
                continue;
            }
            const std::string_view key = line.substr(0, equals);
            const std::string_view value = line.substr(equals + 1);
            if (key == "format")
            {
                sawFormat = value == "1";
            }
            else if (key == "exe_sha256" && IsHex64(value))
            {
                state.exeSha256.assign(value);
            }
            else if (key == "original_sha256" && IsHex64(value))
            {
                state.originalSha256.assign(value);
            }
            else if (key == "game_version" && IsSafeValue(value, 32))
            {
                state.gameVersion.assign(value);
            }
            else if (key == "started_utc" && IsSafeValue(value, 32))
            {
                state.startedUtc.assign(value);
            }
            else if (key == "clean_exit")
            {
                state.cleanExit = value == "1";
            }
            else if (key == "plugins_loaded")
            {
                state.pluginsLoaded = value == "1";
            }
            else if (key == "safe_mode")
            {
                state.safeMode = value == "1";
            }
            else if (key == "unclean_streak" && !value.empty() && value.size() <= 4
                && std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }))
            {
                state.uncleanStreak = static_cast<unsigned>(std::strtoul(std::string(value).c_str(), nullptr, 10));
            }
            else if (key == "known_build")
            {
                const std::size_t first = value.find('|');
                const std::size_t second = first == std::string_view::npos ? first : value.find('|', first + 1);
                if (second == std::string_view::npos)
                {
                    continue;
                }
                KnownBuild build{std::string(value.substr(0, first)),
                    std::string(value.substr(first + 1, second - first - 1)), std::string(value.substr(second + 1))};
                if (IsHex64(build.sha256) && IsSafeValue(build.version, 32) && IsSafeValue(build.firstSeenUtc, 32)
                    && state.knownBuilds.size() < MaxKnownBuilds)
                {
                    state.knownBuilds.push_back(std::move(build));
                }
            }
        }
        return sawFormat;
    }

    inline std::string SerializeState(const State& state)
    {
        std::string text = "format=1\r\n";
        text += "exe_sha256=" + state.exeSha256 + "\r\n";
        text += "game_version=" + state.gameVersion + "\r\n";
        text += "original_sha256=" + state.originalSha256 + "\r\n";
        text += "started_utc=" + state.startedUtc + "\r\n";
        text += std::string("clean_exit=") + (state.cleanExit ? "1" : "0") + "\r\n";
        text += std::string("plugins_loaded=") + (state.pluginsLoaded ? "1" : "0") + "\r\n";
        text += std::string("safe_mode=") + (state.safeMode ? "1" : "0") + "\r\n";
        text += "unclean_streak=" + std::to_string(state.uncleanStreak) + "\r\n";
        for (const KnownBuild& build : state.knownBuilds)
        {
            text += "known_build=" + build.sha256 + "|" + build.version + "|" + build.firstSeenUtc + "\r\n";
        }
        return text;
    }

    // previous == nullptr: no (usable) state file.
    inline Decision Decide(
        const State* previous,
        const Config& config,
        const std::string& exeSha256,
        const std::string& gameVersion,
        const std::string& originalSha256,
        const std::string& nowUtc)
    {
        Decision decision;
        State& next = decision.next;
        next.exeSha256 = exeSha256;
        next.gameVersion = IsSafeValue(gameVersion, 32) ? gameVersion : std::string();
        next.originalSha256 = originalSha256;
        next.startedUtc = nowUtc;

        unsigned streak = 0;
        if (previous != nullptr)
        {
            decision.firstStart = false;
            decision.previousSha256 = previous->exeSha256;
            decision.previousVersion = previous->gameVersion;
            decision.previousClean = previous->cleanExit;
            decision.previousHadPlugins = previous->pluginsLoaded;
            decision.gameUpdated = !previous->exeSha256.empty() && !exeSha256.empty()
                && previous->exeSha256 != exeSha256;
            decision.vendorDllChanged = !previous->originalSha256.empty() && !originalSha256.empty()
                && previous->originalSha256 != originalSha256;
            next.knownBuilds = previous->knownBuilds;
            if (decision.gameUpdated)
            {
                streak = 0;   // a new build gets a fresh chance
            }
            else if (previous->cleanExit)
            {
                streak = 0;
            }
            else if (previous->pluginsLoaded)
            {
                streak = previous->uncleanStreak + 1;
            }
            else
            {
                streak = previous->uncleanStreak;   // unclean without native plugins: not their fault
            }
        }

        if (!exeSha256.empty())
        {
            const bool known = std::any_of(next.knownBuilds.begin(), next.knownBuilds.end(),
                [&exeSha256](const KnownBuild& build) { return build.sha256 == exeSha256; });
            decision.firstRunOnBuild = !known;
            if (!known)
            {
                next.knownBuilds.push_back({exeSha256, next.gameVersion, nowUtc});
                if (next.knownBuilds.size() > MaxKnownBuilds)
                {
                    next.knownBuilds.erase(next.knownBuilds.begin(),
                        next.knownBuilds.begin() + static_cast<std::ptrdiff_t>(next.knownBuilds.size() - MaxKnownBuilds));
                }
            }
        }

        decision.uncleanStreak = streak;
        if (config.forceSafeMode)
        {
            decision.safeMode = true;
            decision.safeModeReason = "[UpdateGuard] ForceSafeMode=1";
        }
        else if (config.safeModeAfterUncleanExits > 0 && streak >= config.safeModeAfterUncleanExits)
        {
            decision.safeMode = true;
            decision.safeModeReason = "the last " + std::to_string(streak)
                + " session(s) with native plugins loaded ended without a clean exit (crash, hang or kill)";
        }
        next.uncleanStreak = streak;
        next.safeMode = decision.safeMode;
        next.cleanExit = false;
        next.pluginsLoaded = false;
        return decision;
    }
}
