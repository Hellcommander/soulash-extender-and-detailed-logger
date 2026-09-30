#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOULASH_EXTENDER_PLUGIN_ABI_VERSION 2u
#define SOULASH_EXTENDER_PLUGIN_INIT_EXPORT "SoulashExtenderPluginInit"

typedef void (WINAPI *SoulashExtenderLogFn)(const char* message);
typedef BOOL (WINAPI *SoulashExtenderReportFileIssueFn)(
    const wchar_t* path,
    unsigned int operation,
    const char* reason);

enum SoulashExtenderFileOperation
{
    SOULASH_EXTENDER_FILE_READ = 1,
    SOULASH_EXTENDER_FILE_WRITE = 2,
    SOULASH_EXTENDER_FILE_PARSE = 3,
    SOULASH_EXTENDER_FILE_VALIDATE = 4
};

typedef struct SoulashExtenderApi
{
    unsigned int structureSize;
    unsigned int abiVersion;
    SoulashExtenderLogFn log;
    SoulashExtenderReportFileIssueFn reportFileIssue;
} SoulashExtenderApi;

typedef BOOL (WINAPI *SoulashExtenderPluginInitFn)(const SoulashExtenderApi* api);

#ifdef __cplusplus
}
#endif
