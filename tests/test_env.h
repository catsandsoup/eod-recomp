// POSIX-style environment changes for portable standalone tests.
#pragma once
#include <cstdlib>
#ifdef _WIN32
inline int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && std::getenv(name)) return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
inline int unsetenv(const char *name) { return _putenv_s(name, "") == 0 ? 0 : -1; }
#endif
