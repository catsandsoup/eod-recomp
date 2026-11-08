// Host paths are UTF-8 throughout the runtime, including on Windows.
#pragma once
#include <cstdio>
#ifdef _WIN32
#include <cerrno>
#include <filesystem>
#include <string>
#endif

inline FILE *host_fopen(const char *path, const char *mode) {
#ifdef _WIN32
    if (!path || !mode) { errno = EINVAL; return nullptr; }
    try {
        std::wstring wide_mode;
        for (const char *p = mode; *p; ++p) wide_mode += static_cast<unsigned char>(*p);
        return _wfopen(std::filesystem::u8path(path).c_str(), wide_mode.c_str());
    } catch (const std::filesystem::filesystem_error &) {
        errno = EINVAL;
        return nullptr;
    }
#else
    return std::fopen(path, mode);
#endif
}
