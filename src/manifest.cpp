#include "manifest.hpp"

#include <cstddef>
#include <utility>

namespace soulash::native_modules
{
    namespace
    {
        constexpr std::size_t MaximumManifestBytes = 4096;
        constexpr std::size_t MaximumModuleIdBytes = 128;
        constexpr std::size_t MaximumModulePathBytes = 240;

        bool IsAsciiAlphaNumeric(char value)
        {
            return (value >= 'a' && value <= 'z')
                || (value >= 'A' && value <= 'Z')
                || (value >= '0' && value <= '9');
        }

        bool IsValidId(std::string_view id)
        {
            if (id.empty() || id.size() > MaximumModuleIdBytes || !IsAsciiAlphaNumeric(id.front()))
            {
                return false;
            }

            for (const char value : id)
            {
                if (!IsAsciiAlphaNumeric(value) && value != '.' && value != '_' && value != '-')
                {
                    return false;
                }
            }

            return true;
        }

        bool IsValidRelativeDllPath(std::string_view path)
        {
            if (path.empty() || path.size() > MaximumModulePathBytes
                || path.front() == '/' || path.find('\\') != std::string_view::npos
                || path.find(':') != std::string_view::npos)
            {
                return false;
            }

            std::size_t componentStart = 0;
            for (std::size_t index = 0; index <= path.size(); ++index)
            {
                if (index != path.size() && path[index] != '/')
                {
                    const char value = path[index];
                    if (!IsAsciiAlphaNumeric(value) && value != '.' && value != '_' && value != '-')
                    {
                        return false;
                    }
                    continue;
                }

                const std::string_view component = path.substr(componentStart, index - componentStart);
                if (component.empty() || component == "." || component == ".." || component.back() == '.')
                {
                    return false;
                }
                componentStart = index + 1;
            }

            if (path.size() < 4)
            {
                return false;
            }
            const std::size_t extensionStart = path.size() - 4;
            return path[extensionStart] == '.'
                && (path[extensionStart + 1] == 'd' || path[extensionStart + 1] == 'D')
                && (path[extensionStart + 2] == 'l' || path[extensionStart + 2] == 'L')
                && (path[extensionStart + 3] == 'l' || path[extensionStart + 3] == 'L');
        }
    }

    bool ParseManifest(std::string_view text, Manifest& manifest, std::string& error)
    {
        if (text.size() > MaximumManifestBytes)
        {
            error = "manifest exceeds 4096 bytes";
            return false;
        }

        if (text.find('\0') != std::string_view::npos)
        {
            error = "manifest contains a NUL byte";
            return false;
        }

        std::size_t lineStart = 0;
        std::size_t lineNumber = 0;
        bool foundId = false;
        bool foundModule = false;
        Manifest parsed;

        while (lineStart < text.size())
        {
            const std::size_t newline = text.find('\n', lineStart);
            const std::size_t lineEnd = newline == std::string_view::npos ? text.size() : newline;
            std::string_view line = text.substr(lineStart, lineEnd - lineStart);
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }

            ++lineNumber;
            if (line.empty())
            {
                error = "blank lines are not allowed";
                return false;
            }

            for (const unsigned char value : line)
            {
                if (value < 0x20 || value > 0x7e)
                {
                    error = "manifest must contain printable ASCII text";
                    return false;
                }
            }

            if (lineNumber == 1)
            {
                if (line != "SOULASH2_NATIVE_MODULES_V1")
                {
                    error = "unsupported native module manifest header";
                    return false;
                }
            }
            else
            {
                const std::size_t separator = line.find('=');
                if (separator == std::string_view::npos || separator == 0
                    || line.find('=', separator + 1) != std::string_view::npos)
                {
                    error = "manifest fields must use key=value syntax";
                    return false;
                }

                const std::string_view key = line.substr(0, separator);
                const std::string_view value = line.substr(separator + 1);
                if (key == "id")
                {
                    if (foundId)
                    {
                        error = "duplicate id field";
                        return false;
                    }
                    if (!IsValidId(value))
                    {
                        error = "invalid module id";
                        return false;
                    }
                    parsed.id.assign(value);
                    foundId = true;
                }
                else if (key == "module")
                {
                    if (foundModule)
                    {
                        error = "duplicate module field";
                        return false;
                    }
                    if (!IsValidRelativeDllPath(value))
                    {
                        error = "module must be a safe relative .dll path";
                        return false;
                    }
                    parsed.module.assign(value);
                    foundModule = true;
                }
                else
                {
                    error = "unknown manifest field";
                    return false;
                }
            }

            if (newline == std::string_view::npos)
            {
                break;
            }
            lineStart = newline + 1;
        }

        if (lineNumber != 3 || !foundId || !foundModule)
        {
            error = "manifest must contain exactly one id and one module field";
            return false;
        }

        manifest = std::move(parsed);
        error.clear();
        return true;
    }
}
