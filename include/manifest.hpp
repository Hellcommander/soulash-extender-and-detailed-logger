#pragma once

#include <string>
#include <string_view>

namespace soulash::native_modules
{
    struct Manifest
    {
        std::string id;
        std::string module;
    };

    bool ParseManifest(std::string_view text, Manifest& manifest, std::string& error);
}
