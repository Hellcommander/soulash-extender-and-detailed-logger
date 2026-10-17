#pragma once

#include <windows.h>

#include "plugin_api.h"

namespace soulash::plugin_services
{
    // Gathers game directory/base/hash once. Safe to call repeatedly.
    void Initialize();

    // Allocates a process-lifetime interface for one plugin. Returns nullptr
    // when the table is full or the id is already in use.
    const SoulashExtenderInterface* CreateInterface(
        const char* pluginId,
        const wchar_t* pluginDirectory);

    // Sends a message from the extender itself to every listener.
    void DispatchCoreMessage(unsigned int type);

    // SHA-256 of everything readable from the handle's current position, as
    // 64 lowercase hex characters plus NUL. Fails above 512 MiB.
    bool Sha256OfHandle(HANDLE file, char (&hex)[65]);
}
