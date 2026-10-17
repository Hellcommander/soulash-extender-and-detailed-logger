#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace soulash::script
{
    struct Limits
    {
        std::size_t memoryBytes = 16u << 20;               // total allocator ceiling
        unsigned long long instructionsPerCall = 5000000;  // per Run / Call
        std::size_t sourceBytes = 256u << 10;              // chunk text size
        std::size_t stringBytes = 1u << 20;                // string.rep / result ceiling
        unsigned millisecondsPerCall = 200;                // wall-clock ceiling per Run / Call
    };

    using LogFunction = void (*)(void* context, const char* message);

    // One isolated Lua 5.4 interpreter for untrusted mod scripts.
    //
    // What a script can do: arithmetic, string/table/math/utf8 functions, its
    // own functions and tables, ext.log(text), and the global functions the
    // host calls. What it cannot: files, processes, the OS, the module system,
    // the debug library, binary chunks, dynamic code loading, pattern matching
    // (which can run unboundedly inside native code), coroutines, finalizers,
    // randomness or time. Memory, instructions, native work proportional to
    // data size, string sizes and wall-clock time per call are all capped.
    //
    // This lowers risk; it is not a formal proof of safety. Keep scripts to
    // text, keep the limits, and keep the Lua sources unmodified and current.
    class Sandbox
    {
    public:
        Sandbox(const Limits& limits, LogFunction log, void* logContext);
        ~Sandbox();
        Sandbox(const Sandbox&) = delete;
        Sandbox& operator=(const Sandbox&) = delete;

        bool Valid() const;

        // Compiles `source` as text and runs its top-level code under the limits.
        bool Run(const std::string& chunkName, const std::string& source, std::string& error);

        // Calls a global function with string arguments. `result` receives its
        // first return value converted to text (empty for nil).
        bool Call(
            const std::string& function,
            const std::vector<std::string>& arguments,
            std::string& result,
            std::string& error);

        std::size_t MemoryUsed() const;

    private:
        struct Impl;
        Impl* impl_;
    };
}
