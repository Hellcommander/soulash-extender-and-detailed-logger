#include "manifest.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    bool ReadManifest(const std::filesystem::path& path, soulash::native_modules::Manifest& manifest)
    {
        std::error_code filesystemError;
        const std::uintmax_t size = std::filesystem::file_size(path, filesystemError);
        if (filesystemError)
        {
            std::cerr << "cannot inspect manifest: " << filesystemError.message() << '\n';
            return false;
        }
        if (size > 4096)
        {
            std::cerr << "manifest exceeds 4096 bytes\n";
            return false;
        }

        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            std::cerr << "cannot open manifest\n";
            return false;
        }

        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (input.bad())
        {
            std::cerr << "cannot read manifest\n";
            return false;
        }

        std::string error;
        if (!soulash::native_modules::ParseManifest(text, manifest, error))
        {
            std::cerr << "invalid manifest: " << error << '\n';
            return false;
        }
        return true;
    }

    bool ResolveModulePath(
        const std::filesystem::path& root,
        const std::string& module,
        std::filesystem::path& result)
    {
        std::error_code filesystemError;
        const std::filesystem::path absoluteRoot = std::filesystem::absolute(root, filesystemError).lexically_normal();
        if (filesystemError)
        {
            std::cerr << "cannot resolve module root: " << filesystemError.message() << '\n';
            return false;
        }

        result = (absoluteRoot / std::filesystem::u8path(module)).lexically_normal();
        std::wstring rootPrefix = absoluteRoot.native();
        if (!rootPrefix.empty() && rootPrefix.back() != L'\\' && rootPrefix.back() != L'/')
        {
            rootPrefix.push_back(L'\\');
        }

        const std::wstring candidate = result.native();
        if (candidate.size() <= rootPrefix.size()
            || CompareStringOrdinal(candidate.data(), static_cast<int>(rootPrefix.size()),
                rootPrefix.data(), static_cast<int>(rootPrefix.size()), TRUE) != CSTR_EQUAL)
        {
            std::cerr << "resolved module path escapes the module root\n";
            return false;
        }

        return true;
    }

    void PrintUsage()
    {
        std::cerr << "usage: native-module-host.exe --validate <manifest> <module-root>\n"
            << "       native-module-host.exe --load <manifest> <module-root>\n";
    }
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc != 4 || (std::wstring(argv[1]) != L"--validate" && std::wstring(argv[1]) != L"--load"))
    {
        PrintUsage();
        return 2;
    }

    const bool load = std::wstring(argv[1]) == L"--load";
    soulash::native_modules::Manifest manifest;
    if (!ReadManifest(argv[2], manifest))
    {
        return 1;
    }

    std::filesystem::path modulePath;
    if (!ResolveModulePath(argv[3], manifest.module, modulePath))
    {
        return 1;
    }

    if (!load)
    {
        const std::wstring wideId(manifest.id.begin(), manifest.id.end());
        std::wcout << L"Valid manifest for " << wideId
            << L"; module path: " << modulePath.native() << L'\n';
        return 0;
    }

    const DWORD attributes = GetFileAttributesW(modulePath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        std::cerr << "module file does not exist or is not a file\n";
        return 1;
    }

    std::wcerr << L"WARNING: loading " << modulePath.native()
        << L" executes arbitrary native code in this process.\n";
    HMODULE module = LoadLibraryExW(
        modulePath.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr)
    {
        std::cerr << "LoadLibraryExW failed with error " << GetLastError() << '\n';
        return 1;
    }

    std::wcout << L"Loaded " << modulePath.native()
        << L"; module initialization has already executed.\n";
    if (!FreeLibrary(module))
    {
        std::cerr << "FreeLibrary failed with error " << GetLastError() << '\n';
        return 1;
    }
    return 0;
}
