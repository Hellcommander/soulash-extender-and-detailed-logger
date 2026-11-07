#include "script_sandbox.hpp"

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace soulash::script
{
    namespace
    {
        constexpr int HookInterval = 100;
        constexpr unsigned long long BytesPerUnit = 32; // native work charged per input/allocated bytes
        constexpr std::size_t MaximumMessage = 480;

        // Lives for the whole life of a lua_State; reached through its
        // allocator's user data.
        struct State
        {
            std::size_t used = 0;
            std::size_t limit = 0;
            unsigned long long budget = 0;   // instructions left in this call
            bool aborted = false;            // budget exhausted: not catchable
            bool armed = false;              // true only while a Run / Call is executing
            std::chrono::steady_clock::time_point deadline;
            std::size_t stringLimit = 0;
            LogFunction log = nullptr;
            void* logContext = nullptr;
        };

        void* Allocate(void* userData, void* block, std::size_t oldSize, std::size_t newSize)
        {
            auto* state = static_cast<State*>(userData);
            if (block == nullptr)
            {
                oldSize = 0; // for a null block, oldSize encodes the object type
            }
            if (newSize == 0)
            {
                std::free(block);
                state->used -= oldSize;
                return nullptr;
            }
            if (newSize > oldSize && state->used - oldSize + newSize > state->limit)
            {
                return nullptr; // Lua turns this into a "not enough memory" error
            }
            if (state->armed && newSize > oldSize)
            {
                // Native copying work scales with what is allocated.
                const unsigned long long units = (newSize - oldSize) / BytesPerUnit;
                if (units >= state->budget)
                {
                    state->aborted = true;
                    state->budget = 0;
                    return nullptr;
                }
                state->budget -= units;
            }
            void* grown = std::realloc(block, newSize);
            if (grown != nullptr)
            {
                state->used = state->used - oldSize + newSize;
            }
            return grown;
        }

        State* StateOf(lua_State* L)
        {
            void* userData = nullptr;
            lua_getallocf(L, &userData);
            return static_cast<State*>(userData);
        }

        void CountHook(lua_State* L, lua_Debug*)
        {
            State* state = StateOf(L);
            if (state->aborted || state->budget <= static_cast<unsigned long long>(HookInterval)
                || std::chrono::steady_clock::now() >= state->deadline)
            {
                state->aborted = true;
                luaL_error(L, "instruction limit exceeded");
            }
            state->budget -= static_cast<unsigned long long>(HookInterval);
        }

        void Charge(lua_State* L, unsigned long long units)
        {
            State* state = StateOf(L);
            if (units >= state->budget)
            {
                state->aborted = true;
                state->budget = 0;
                luaL_error(L, "instruction limit exceeded");
            }
            state->budget -= units;
        }

        // Wraps a library function so that its native work is charged in
        // proportion to its inputs and results. Upvalue 1 is the original
        // function, upvalue 2 an integer cost multiplier.
        int ChargedCall(lua_State* L)
        {
            const int arguments = lua_gettop(L);
            unsigned long long bytes = static_cast<unsigned long long>(arguments);
            for (int index = 1; index <= arguments; ++index)
            {
                const int type = lua_type(L, index);
                if (type == LUA_TSTRING || type == LUA_TTABLE)
                {
                    bytes += static_cast<unsigned long long>(lua_rawlen(L, index));
                }
            }
            const unsigned long long multiplier =
                static_cast<unsigned long long>(lua_tointeger(L, lua_upvalueindex(2)));
            Charge(L, (bytes * multiplier) / BytesPerUnit + 1);

            lua_pushvalue(L, lua_upvalueindex(1));
            lua_insert(L, 1);
            lua_call(L, arguments, LUA_MULTRET);
            const int results = lua_gettop(L);
            Charge(L, static_cast<unsigned long long>(results) / 4 + 1);
            return results;
        }

        void WrapField(lua_State* L, const char* name, lua_Integer multiplier)
        {
            lua_getfield(L, -1, name);
            if (lua_isfunction(L, -1))
            {
                lua_pushinteger(L, multiplier);
                lua_pushcclosure(L, ChargedCall, 2);
                lua_setfield(L, -2, name);
            }
            else
            {
                lua_pop(L, 1);
            }
        }

        void WrapLibrary(lua_State* L, const char* library)
        {
            lua_getglobal(L, library);   // [lib]
            lua_newtable(L);             // [lib, names]: collect names before modifying lib
            int count = 0;
            lua_pushnil(L);              // [lib, names, nil]
            while (lua_next(L, -3) != 0) // [lib, names, key, value]
            {
                if (lua_type(L, -2) == LUA_TSTRING && lua_isfunction(L, -1))
                {
                    lua_pushvalue(L, -2);
                    lua_rawseti(L, -4, ++count); // names[count] = key
                }
                lua_pop(L, 1);           // drop value, keep key
            }
            for (int index = 1; index <= count; ++index)
            {
                lua_rawgeti(L, -1, index);                 // [lib, names, name]
                const char* name = lua_tostring(L, -1);    // anchored by the names table
                lua_pop(L, 1);
                const bool sorting =
                    std::strcmp(library, "table") == 0 && std::strcmp(name, "sort") == 0;
                lua_pushvalue(L, -2);                      // [lib, names, lib]
                WrapField(L, name, sorting ? 16 : 1);
                lua_pop(L, 1);
            }
            lua_pop(L, 2); // names, lib
        }

        // pcall / xpcall that cannot swallow an abort: once the instruction
        // budget is spent every enclosing protected call re-raises.
        int SafePcall(lua_State* L)
        {
            luaL_checkany(L, 1);
            const int status = lua_pcall(L, lua_gettop(L) - 1, LUA_MULTRET, 0);
            if (StateOf(L)->aborted)
            {
                return lua_error(L);
            }
            if (status != LUA_OK)
            {
                lua_pushboolean(L, 0);
                lua_insert(L, -2);
                return 2;
            }
            lua_pushboolean(L, 1);
            lua_insert(L, 1);
            return lua_gettop(L);
        }

        int SafeSetMetatable(lua_State* L)
        {
            luaL_checktype(L, 1, LUA_TTABLE);
            if (lua_type(L, 2) == LUA_TTABLE)
            {
                // A finalizer runs with hooks disabled, so it could spin forever.
                lua_pushstring(L, "__gc");
                lua_rawget(L, 2);
                const bool hasFinalizer = !lua_isnil(L, -1);
                lua_pop(L, 1);
                if (hasFinalizer)
                {
                    return luaL_error(L, "__gc is not allowed");
                }
            }
            lua_settop(L, 2);
            lua_setmetatable(L, 1);
            lua_settop(L, 1);
            return 1;
        }

        int Log(lua_State* L)
        {
            State* state = StateOf(L);
            std::size_t length = 0;
            const char* text = luaL_tolstring(L, 1, &length);
            char message[MaximumMessage + 1];
            if (length > MaximumMessage)
            {
                length = MaximumMessage;
            }
            for (std::size_t index = 0; index < length; ++index)
            {
                const unsigned char value = static_cast<unsigned char>(text[index]);
                message[index] = value < 0x20 || value == 0x7f ? ' ' : static_cast<char>(value);
            }
            message[length] = '\0';
            if (state->log != nullptr)
            {
                state->log(state->logContext, message);
            }
            return 0;
        }

        int CollectGarbage(lua_State* L)
        {
            const char* option = luaL_optstring(L, 1, "count");
            if (std::strcmp(option, "count") != 0)
            {
                return luaL_error(L, "collectgarbage: only \"count\" is available");
            }
            lua_pushnumber(L, static_cast<lua_Number>(StateOf(L)->used) / 1024.0);
            return 1;
        }

        // string.rep with a ceiling on the result.
        int RepLimited(lua_State* L)
        {
            std::size_t length = 0;
            const char* text = luaL_checklstring(L, 1, &length);
            const lua_Integer count = luaL_checkinteger(L, 2);
            std::size_t separatorLength = 0;
            const char* separator = luaL_optlstring(L, 3, "", &separatorLength);
            if (count <= 0)
            {
                lua_pushliteral(L, "");
                return 1;
            }
            const unsigned long long total =
                static_cast<unsigned long long>(count) * (length + separatorLength);
            if (length + separatorLength != 0 && total > StateOf(L)->stringLimit)
            {
                return luaL_error(L, "resulting string too large");
            }
            luaL_Buffer buffer;
            luaL_buffinit(L, &buffer);
            for (lua_Integer index = 0; index < count; ++index)
            {
                luaL_addlstring(&buffer, text, length);
                if (index + 1 < count && separatorLength != 0)
                {
                    luaL_addlstring(&buffer, separator, separatorLength);
                }
            }
            luaL_pushresult(&buffer);
            return 1;
        }

        // string.find restricted to plain substring search (no patterns).
        int FindPlain(lua_State* L)
        {
            std::size_t subjectLength = 0;
            std::size_t needleLength = 0;
            const char* subject = luaL_checklstring(L, 1, &subjectLength);
            const char* needle = luaL_checklstring(L, 2, &needleLength);
            lua_Integer init = luaL_optinteger(L, 3, 1);
            if (init < 0)
            {
                init = static_cast<lua_Integer>(subjectLength) + init + 1;
            }
            if (init < 1)
            {
                init = 1;
            }
            if (static_cast<std::size_t>(init) - 1 > subjectLength)
            {
                lua_pushnil(L);
                return 1;
            }
            const char* start = subject + (init - 1);
            const std::size_t remaining = subjectLength - static_cast<std::size_t>(init - 1);
            if (needleLength == 0)
            {
                lua_pushinteger(L, init);
                lua_pushinteger(L, init - 1);
                return 2;
            }
            for (std::size_t offset = 0; offset + needleLength <= remaining; ++offset)
            {
                if (std::memcmp(start + offset, needle, needleLength) == 0)
                {
                    lua_pushinteger(L, init + static_cast<lua_Integer>(offset));
                    lua_pushinteger(L, init + static_cast<lua_Integer>(offset + needleLength) - 1);
                    return 2;
                }
            }
            lua_pushnil(L);
            return 1;
        }

        void RemoveField(lua_State* L, int table, const char* name)
        {
            lua_pushnil(L);
            lua_setfield(L, table < 0 ? table - 1 : table, name);
        }

        // Runs protected; sets up the whitelisted environment.
        int PrepareEnvironment(lua_State* L)
        {
            // Only these libraries are linked into the extender at all (see
            // third_party/lua/README.md): base, table, string, math, utf8.
            luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
            lua_pop(L, 1);
            const luaL_Reg libraries[] = {
                {LUA_TABLIBNAME, luaopen_table},
                {LUA_STRLIBNAME, luaopen_string},
                {LUA_MATHLIBNAME, luaopen_math},
                {LUA_UTF8LIBNAME, luaopen_utf8},
                {nullptr, nullptr}};
            for (const luaL_Reg* library = libraries; library->func != nullptr; ++library)
            {
                luaL_requiref(L, library->name, library->func, 1);
                lua_pop(L, 1);
            }

            lua_pushglobaltable(L);
            for (const char* name : {"dofile", "loadfile", "load", "loadstring", "warn", "print",
                     "require", "collectgarbage", "pcall", "xpcall", "setmetatable"})
            {
                RemoveField(L, -1, name);
            }
            lua_pushcfunction(L, SafePcall);
            lua_setfield(L, -2, "pcall");
            lua_pushcfunction(L, SafeSetMetatable);
            lua_setfield(L, -2, "setmetatable");
            lua_pushcfunction(L, CollectGarbage);
            lua_setfield(L, -2, "collectgarbage");

            lua_getfield(L, -1, "string");
            for (const char* name : {"dump", "match", "gmatch", "gsub", "pack", "unpack", "packsize"})
            {
                RemoveField(L, -1, name);
            }
            lua_pushcfunction(L, RepLimited);
            lua_setfield(L, -2, "rep");
            lua_pushcfunction(L, FindPlain);
            lua_setfield(L, -2, "find");
            lua_pop(L, 1);

            // table.move loops in native code over a caller-chosen range, so a
            // sparse table makes it unbounded; the instruction hook cannot see it.
            lua_getfield(L, -1, "table");
            RemoveField(L, -1, "move");
            lua_pop(L, 1);

            lua_getfield(L, -1, "math");
            for (const char* name : {"random", "randomseed"})
            {
                RemoveField(L, -1, name);
            }
            lua_pop(L, 1);

            // Charge native work proportionally for the library functions that
            // can loop over their inputs without executing VM instructions.
            lua_pop(L, 1); // global table
            WrapLibrary(L, "string");
            WrapLibrary(L, "table");
            WrapLibrary(L, "utf8");
            lua_pushglobaltable(L);
            for (const char* name : {"tostring", "tonumber", "rawequal"})
            {
                WrapField(L, name, 1);
            }

            lua_newtable(L);
            lua_pushcfunction(L, Log);
            lua_setfield(L, -2, "log");
            lua_pushinteger(L, 1);
            lua_setfield(L, -2, "api");
            lua_setfield(L, -2, "ext");
            lua_pop(L, 1); // global table
            return 0;
        }

        struct CallRequest
        {
            const char* function;
            const std::vector<std::string>* arguments;
        };

        // Runs under lua_pcall: allocation failures while pushing arguments or
        // converting the result become ordinary errors instead of a panic.
        int CallThunk(lua_State* L)
        {
            const auto* request = static_cast<const CallRequest*>(lua_touserdata(L, 1));
            if (lua_getglobal(L, request->function) != LUA_TFUNCTION)
            {
                return luaL_error(L, "no function named %s", request->function);
            }
            const int count = static_cast<int>(request->arguments->size());
            luaL_checkstack(L, count + 4, "too many arguments");
            for (int index = 0; index < count; ++index)
            {
                const std::string& argument = (*request->arguments)[static_cast<std::size_t>(index)];
                lua_pushlstring(L, argument.data(), argument.size());
            }
            lua_call(L, count, 1);
            if (lua_isnil(L, -1))
            {
                lua_pushliteral(L, "");
            }
            else
            {
                luaL_tolstring(L, -1, nullptr);
            }
            return 1;
        }

        std::string TakeError(lua_State* L)
        {
            // Only an actual string is read: converting another type could
            // allocate outside a protected call.
            std::string message = "script error (non-text value)";
            if (lua_type(L, -1) == LUA_TSTRING)
            {
                std::size_t length = 0;
                const char* text = lua_tolstring(L, -1, &length);
                message.assign(text, length < MaximumMessage ? length : MaximumMessage);
            }
            lua_pop(L, 1);
            return message;
        }
    }

    struct Sandbox::Impl
    {
        Limits limits;
        State state;
        lua_State* L = nullptr;
    };

    Sandbox::Sandbox(const Limits& limits, LogFunction log, void* logContext)
        : impl_(new (std::nothrow) Impl)
    {
        if (impl_ == nullptr)
        {
            return;
        }
        impl_->limits = limits;
        impl_->state.limit = limits.memoryBytes;
        impl_->state.stringLimit = limits.stringBytes;
        impl_->state.log = log;
        impl_->state.logContext = logContext;
        impl_->L = lua_newstate(Allocate, &impl_->state);
        if (impl_->L == nullptr)
        {
            return;
        }
        lua_pushcfunction(impl_->L, PrepareEnvironment);
        if (lua_pcall(impl_->L, 0, 0, 0) != LUA_OK)
        {
            lua_close(impl_->L);
            impl_->L = nullptr;
            return;
        }
        lua_sethook(impl_->L, CountHook, LUA_MASKCOUNT, HookInterval);
    }

    Sandbox::~Sandbox()
    {
        if (impl_ != nullptr)
        {
            if (impl_->L != nullptr)
            {
                lua_close(impl_->L);
            }
            delete impl_;
        }
    }

    bool Sandbox::Valid() const
    {
        return impl_ != nullptr && impl_->L != nullptr;
    }

    std::size_t Sandbox::MemoryUsed() const
    {
        return impl_ != nullptr ? impl_->state.used : 0;
    }

    bool Sandbox::Run(const std::string& chunkName, const std::string& source, std::string& error)
    {
        error.clear();
        if (!Valid())
        {
            error = "sandbox unavailable";
            return false;
        }
        if (source.size() > impl_->limits.sourceBytes)
        {
            error = "script exceeds the source size limit";
            return false;
        }
        if (!source.empty() && static_cast<unsigned char>(source[0]) == 0x1B)
        {
            error = "binary chunks are not allowed";
            return false;
        }

        lua_State* L = impl_->L;
        impl_->state.aborted = false;
        impl_->state.budget = impl_->limits.instructionsPerCall;
        impl_->state.deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(impl_->limits.millisecondsPerCall);
        impl_->state.armed = true;
        const int top = lua_gettop(L);
        const std::string name = "=" + chunkName.substr(0, 60);
        if (luaL_loadbufferx(L, source.data(), source.size(), name.c_str(), "t") != LUA_OK)
        {
            error = TakeError(L);
            lua_settop(L, top);
            return false;
        }
        if (lua_pcall(L, 0, 0, 0) != LUA_OK)
        {
            error = TakeError(L);
            lua_settop(L, top);
            return false;
        }
        lua_settop(L, top);
        return true;
    }

    bool Sandbox::Call(
        const std::string& function,
        const std::vector<std::string>& arguments,
        std::string& result,
        std::string& error)
    {
        error.clear();
        result.clear();
        if (!Valid())
        {
            error = "sandbox unavailable";
            return false;
        }

        lua_State* L = impl_->L;
        impl_->state.aborted = false;
        impl_->state.budget = impl_->limits.instructionsPerCall;
        impl_->state.deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(impl_->limits.millisecondsPerCall);
        impl_->state.armed = true;
        const int top = lua_gettop(L);
        const CallRequest request{function.c_str(), &arguments};
        lua_pushcfunction(L, CallThunk);
        lua_pushlightuserdata(L, const_cast<CallRequest*>(&request));
        if (lua_pcall(L, 1, 1, 0) != LUA_OK)
        {
            error = TakeError(L);
            lua_settop(L, top);
            return false;
        }
        if (lua_type(L, -1) == LUA_TSTRING)
        {
            std::size_t length = 0;
            const char* text = lua_tolstring(L, -1, &length);
            if (length > impl_->limits.stringBytes)
            {
                length = impl_->limits.stringBytes;
            }
            result.assign(text, length);
        }
        lua_settop(L, top);
        return true;
    }
}
