// Plugin side of the update guard's reportStatus (docs/update-guard.md).
//
// ReportStatus() is a no-op on proxies older than the update guard, so a plugin
// using it still loads everywhere. StartupReason captures the plugin's own log
// lines on the loading thread only, so the line a failed install printed last
// ("... ; inactive") becomes the reason in compat-report.txt.
#pragma once

#include "plugin_api.h"

#include <windows.h>

#include <cstring>

namespace soulash_plugins
{
    inline void ReportStatus(const SoulashExtenderInterface* extender, unsigned int status, const char* detail)
    {
        if (extender != nullptr && SOULASH_EXTENDER_HAS(extender, reportStatus))
        {
            extender->reportStatus(extender->pluginHandle, status, detail != nullptr ? detail : "");
        }
    }

    class StartupReason
    {
    public:
        void Begin()
        {
            m_last[0] = '\0';
            m_thread = GetCurrentThreadId();
        }

        void End()
        {
            m_thread = 0;
        }

        // Call from the plugin's Log() with the unprefixed line.
        void Capture(const char* line)
        {
            if (m_thread != 0 && GetCurrentThreadId() == m_thread && line != nullptr)
            {
                strncpy_s(m_last, line, _TRUNCATE);
            }
        }

        const char* Last() const
        {
            return m_last;
        }

    private:
        DWORD m_thread = 0;
        char m_last[200]{};
    };
}
