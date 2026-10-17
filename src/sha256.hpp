#pragma once

#include <windows.h>

namespace soulash
{
    // SHA-256 of everything readable from the handle's current position, as 64
    // lowercase hex characters plus NUL. Fails (returns false) above 512 MiB or
    // on any read or crypto error.
    bool Sha256OfHandle(HANDLE file, char (&hex)[65]);
}
