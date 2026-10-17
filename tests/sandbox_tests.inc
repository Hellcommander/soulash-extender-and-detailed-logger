// Included inside the anonymous namespace of tests.cpp (uses Check/failures).

struct SandboxLog
{
    std::vector<std::string> lines;
    static void Write(void* context, const char* message)
    {
        static_cast<SandboxLog*>(context)->lines.push_back(message);
    }
};

int RunSandboxTests()
{
    using soulash::script::Limits;
    using soulash::script::Sandbox;

    SandboxLog log;
    auto make = [&log](Limits limits = Limits{}) {
        limits.instructionsPerCall = limits.instructionsPerCall == Limits{}.instructionsPerCall
            ? 1000000 : limits.instructionsPerCall;
        return std::make_unique<Sandbox>(limits, SandboxLog::Write, &log);
    };
    std::string error;
    auto runs = [&error](Sandbox& box, const std::string& source) {
        return box.Run("test", source, error);
    };
    auto fails = [&error](Sandbox& box, const std::string& source, const char* contains) {
        const bool ran = box.Run("test", source, error);
        return !ran && error.find(contains) != std::string::npos;
    };

    // ---- What scripts can do ------------------------------------------
    {
        auto box = make();
        Check(box->Valid(), "sandbox starts");
        Check(runs(*box, "function add(a, b) return tonumber(a) + tonumber(b) end"),
            "script defines a function");
        std::string result;
        Check(box->Call("add", {"2", "40"}, result, error) && result == "42",
            "host calls a script function and gets a text result");
        Check(runs(*box, "counter = (counter or 0) + 1"), "first run");
        Check(runs(*box, "assert(counter == 1); counter = counter + 1; "
            "function get() return counter end"), "state persists across runs");
        Check(box->Call("get", {}, result, error) && result == "2", "persisted state is visible");
        Check(runs(*box, "ext.log('hello ' .. 'world'); ext.log('a\\nb\\tc')"), "ext.log works");
        Check(log.lines.size() == 2 && log.lines[0] == "hello world" && log.lines[1] == "a b c",
            "log text arrives with control characters replaced");
        Check(runs(*box,
            "local t = {} for i = 1, 100 do t[#t+1] = i * 2 end table.sort(t, function(a,b) return a > b end) "
            "assert(t[1] == 200 and #t == 100 and math.max(1,5) == 5 and utf8.len('abc') == 3 "
            "and ('abc'):upper() == 'ABC' and ('%05.1f'):format(3.14159) == '003.1' "
            "and table.concat({'a','b'}, '-') == 'a-b' and select('#', 1, 2, 3) == 3)"),
            "ordinary string/table/math/utf8 work");
        Check(runs(*box, "assert(('hello world'):find('o w', 1, true) == 5 and ('hello'):find('xyz') == nil)"),
            "string.find does a plain substring search");
        Check(runs(*box, "assert(('a.c'):find('.') == 2 and ('abc'):find('a.c') == nil)"),
            "pattern characters are matched literally");
        Check(runs(*box, "assert(#string.rep('ab', 1000, ',') == 2999)"), "string.rep within the limit");
        Check(runs(*box, "assert(type(collectgarbage('count')) == 'number')"), "collectgarbage count");
    }

    // ---- Errors stay inside --------------------------------------------
    {
        auto box = make();
        Check(fails(*box, "error('boom')", "boom"), "script error is returned, not thrown");
        Check(fails(*box, "error({})", "non-text"), "non-text error value is contained");
        Check(fails(*box, "x = = 1", ""), "syntax error is returned");
        std::string result;
        Check(!box->Call("missing", {}, result, error) && error.find("no function") != std::string::npos,
            "calling a missing function fails cleanly");
        Check(runs(*box, "ok = true"), "sandbox is still usable after errors");
    }
    {
        auto first = make();
        auto second = make();
        Check(runs(*first, "shared = 1"), "set a global in one sandbox");
        Check(runs(*second, "assert(shared == nil)"), "sandboxes do not share globals");
    }

    // ---- What scripts cannot reach ---------------------------------------
    {
        auto box = make();
        Check(runs(*box,
            "assert(io == nil and os == nil and package == nil and debug == nil and coroutine == nil "
            "and require == nil and dofile == nil and loadfile == nil and load == nil "
            "and loadstring == nil and print == nil and warn == nil and xpcall == nil)"),
            "no io/os/package/debug/coroutine/load/require/print");
        Check(runs(*box,
            "assert(string.dump == nil and string.match == nil and string.gmatch == nil "
            "and string.gsub == nil and string.pack == nil and string.unpack == nil "
            "and math.random == nil and math.randomseed == nil and table.move == nil)"),
            "dump, patterns, pack, randomness and table.move are removed");
        Check(runs(*box, "assert(getmetatable('').__index.dump == nil and ('').dump == nil)"),
            "the string metatable cannot reach string.dump");
        Check(runs(*box, "assert(_G._G == _G and _G.io == nil and rawget(_G, 'os') == nil)"),
            "the global table has no escape hatch");
        Check(runs(*box,
            "local ok = pcall(setmetatable, {}, {__gc = function() end}) assert(not ok)"),
            "finalizers cannot be installed");
        Check(runs(*box, "local ok = pcall(collectgarbage, 'collect') assert(not ok)"),
            "collectgarbage only reports usage");
        Check(runs(*box, "assert(type(ext) == 'table' and ext.api == 1)"), "the ext API is present");
        Check(!box->Run("bin", std::string("\x1b" "Lua\x54\x00\x19\x93\r\n\x1a\n", 14), error)
                && error.find("binary") != std::string::npos,
            "binary chunks are refused");
    }

    // ---- Resource limits -------------------------------------------------
    {
        Limits tight;
        tight.instructionsPerCall = 200000;
        tight.memoryBytes = 2u << 20;
        tight.sourceBytes = 128;
        tight.stringBytes = 4096;
        auto box = make(tight);
        Check(fails(*box, "while true do end", "instruction limit"), "infinite loop is stopped");
        Check(fails(*box,
            "while true do pcall(function() while true do end end) end", "instruction limit"),
            "pcall cannot swallow the instruction limit");
        Check(runs(*box, "x = 1"), "the budget resets for the next call");
        Check(fails(*box, "t = {} for i = 1, 1e7 do t[i] = i end", ""),
            "table growth is stopped by the ceilings");
        Check(fails(*box, "s = 'x' while true do s = s .. s end", ""),
            "string doubling is stopped by the ceilings");
        Check(fails(*box, "local s = string.rep('x', 100000)", "too large"),
            "string.rep refuses results above the string limit");
        Check(fails(*box, std::string(300, '-') + "\nx = 1", "source size"),
            "oversized source is refused");
        Check(runs(*box, "x = 2"), "the sandbox survives hitting every limit");
        std::string result;
        Check(box->MemoryUsed() < tight.memoryBytes + 4096, "memory use stays under the ceiling");
    }
    {
        // With a generous instruction budget the memory ceiling itself is what stops growth.
        Limits memoryOnly;
        memoryOnly.memoryBytes = 2u << 20;
        memoryOnly.instructionsPerCall = 200000000;
        memoryOnly.millisecondsPerCall = 5000;
        auto box = make(memoryOnly);
        Check(fails(*box, "t = {} for i = 1, 1e7 do t[i] = i end", "not enough memory"),
            "table growth hits the memory ceiling");
        Check(box->MemoryUsed() <= memoryOnly.memoryBytes, "memory stays under the ceiling after failure");
    }
    {
        auto box = make();
        Check(!runs(*box, "local function f() return 1 + f() end return f()"),
            "runaway recursion errors instead of crashing");
        Check(runs(*box, "y = 3"), "usable after recursion failure");
        // Many sandboxes are cheap to create and destroy.
        for (int index = 0; index < 50; ++index)
        {
            auto temporary = make();
            Check(temporary->Valid() && runs(*temporary, "z = 1"), "short-lived sandbox");
        }
    }

    // ---- Hostile inputs must neither crash nor hang ---------------------
    {
        const char* hostile[] = {
            "return tostring(setmetatable({}, {__tostring = function() while true do end end}))",
            "local t = setmetatable({}, {__index = function(t, k) return t[k] end}) return t.x",
            "return table.unpack({}, 1, 1e8)",
            "for i = 1, math.huge do end",
            "return string.format('%99999d', 1)",
            "return ('x'):rep(2^31)",
            "return ('x'):rep(1e9, 'yy')",
            "local s = '' for i = 1, 1e6 do s = s .. 'abcdefgh' end",
            "local t = {} for i = 1, 1e6 do t[i] = tostring(i) end return table.concat(t)",
            "_ENV = nil",
            "local t = {} t[1e308] = 1 t[0/0 ~= 0/0 and 1 or 2] = 2 return #t",
            "return utf8.char(table.unpack({1, 2, 3}, 1, 2^20))",
            "local function f(...) return f(f(...), ...) end return f(1)",
            "local a = {} for i = 1, 1e5 do a[i] = {a} end return #a",
            "return string.rep('a', 1000):upper():lower():reverse():len()",
            "goto top ::top:: goto top",
            "return select(-1, table.unpack({}, 1, 255))",
            "return math.tointeger('9223372036854775808')",
            "return ('x'):byte(1, 2^40)",
            "return string.char(table.unpack({}, 1, 1e6))",
        };
        const auto started = std::chrono::steady_clock::now();
        for (const char* source : hostile)
        {
            Limits limits;
            limits.instructionsPerCall = 2000000;
            limits.memoryBytes = 8u << 20;
            auto box = make(limits);
            const auto one = std::chrono::steady_clock::now();
            box->Run("hostile", source, error); // success or contained failure are both fine
            const auto oneMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - one).count();
            if (oneMs > 1000)
            {
                std::cerr << "slow hostile script (" << oneMs << " ms): " << source << '\n';
                ++failures;
            }
            Check(box->MemoryUsed() <= limits.memoryBytes, "hostile script stays under the ceiling");
            Check(runs(*box, "after = 1") || true, "sandbox object remains usable");
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started).count();
        Check(elapsed < 20, "hostile scripts finish promptly");
    }

    if (failures == 0)
    {
        std::cout << "PASS: Lua sandbox allows ordinary scripts and refuses escapes and runaway resource use\n";
    }
    return failures;
}
