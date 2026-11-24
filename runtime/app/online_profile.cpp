#include "online_profile.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include "macos_app.h"
#include <spawn.h>
extern char **environ;
#endif

[[noreturn]] void app_exit(int code);

namespace online_profile {
namespace fs = std::filesystem;
using Command = game_frontend::Command;
namespace {
char **startup_args;
bool selected;
std::optional<Command> resume;

std::string env_utf8(const char *key) {
#ifdef _WIN32
    const wchar_t *value = _wgetenv(fs::u8path(key).c_str());
    return value ? fs::path(value).u8string() : std::string{};
#else
    const char *value = std::getenv(key);
    return value ? value : "";
#endif
}

fs::path support_dir() {
    const auto override_dir = env_utf8("EOD_APP_SUPPORT");
    if (!override_dir.empty()) return fs::u8path(override_dir);
#ifdef __APPLE__
    return fs::u8path(mac_app_support());
#elif defined(_WIN32)
    const auto local = env_utf8("LOCALAPPDATA");
    if (!local.empty()) return fs::u8path(local) / "TDEODRecomp";
#else
    if (const char *p = std::getenv("XDG_DATA_HOME"); p && *p) return fs::u8path(p) / "TDEODRecomp";
    if (const char *p = std::getenv("HOME"); p && *p) return fs::u8path(p) / ".local/share/TDEODRecomp";
#endif
    return {};
}

bool environment(const char *key, const std::string &value) {
#ifdef _WIN32
    return _wputenv_s(fs::u8path(key).c_str(), fs::u8path(value).c_str()) == 0;
#else
    return (value.empty() ? unsetenv(key) : setenv(key, value.c_str(), 1)) == 0;
#endif
}
}

void initialize(char **argv) {
    startup_args = argv;
    const auto target = env_utf8("EOD_ONLINE_REJOIN");
    const auto local = env_utf8("EOD_ONLINE_PROFILE");
    if (target.empty() || local.empty()) return;
    environment("EOD_ONLINE_REJOIN", {});
    environment("EOD_ONLINE_PROFILE", {});
    if (target.size() < 3 || target.size() > 130 || target[1] != ':') return;
    Command c{};
    switch (target[0]) {
    case 'C': c.kind = Command::Kind::JoinCode; break;
    case 'A': c.kind = Command::Kind::JoinAddress; break;
    case 'F': c.kind = Command::Kind::JoinCrew; break;
    default: return;
    }
    if (local != "disc" && local.rfind("overlay:", 0) != 0) return;
    const std::string path = local == "disc" ? "" : local.substr(8);
#ifdef _WIN32
    // The runtime treats bytes from getenv("EOD_MOD_DIR") as UTF-8.
    selected = _putenv_s("EOD_MOD_DIR", path.c_str()) == 0;
#else
    selected = environment("EOD_MOD_DIR", path);
#endif
    if (selected) { c.text = target.substr(2); resume = std::move(c); }
}

bool selected_for_session() { return selected; }
std::optional<Command> take_invite() { return std::exchange(resume, std::nullopt); }

std::string standalone_overlay() try {
    const fs::path support = support_dir();
    if (support.empty()) return {};
    const fs::path folder = support / "profiles";
    const auto pointer = folder / "active_profile.txt";
    std::error_code ec;
    if (!fs::is_regular_file(pointer, ec) || fs::file_size(pointer, ec) > 4096 || ec) return {};
    std::ifstream input(pointer, std::ios::binary);
    std::string line;
    if (!std::getline(input, line) || line.size() > 4096) return {};
    const auto first = line.find_first_not_of(" \t\r");
    if (first == line.npos || line.find('\0') != line.npos) return {};
    fs::path target = fs::u8path(line.substr(first, line.find_last_not_of(" \t\r") - first + 1));
    if (target.is_relative()) target = folder / target;
    if (!fs::is_directory(target, ec)) return {};
    for (fs::directory_iterator it(target, ec), end; !ec && it != end; it.increment(ec)) {
        auto name = it->path().filename().u8string();
        for (char &c : name) if (c >= 'a' && c <= 'z') c -= 32;
        if (name == "DATA" && it->is_directory(ec)) return target.u8string();
    }
    return {};
} catch (const fs::filesystem_error &) {
    return {};
}

std::vector<std::string> installed_overlays() {
    const auto support = support_dir();
    std::vector<fs::path> paths;
    if (startup_args) {
        const auto exe = fs::u8path(startup_args[0]).parent_path();
        paths.push_back(exe / "Mods/integrated");
#ifdef __APPLE__
        paths.push_back(exe.parent_path() / "Resources/Mods/integrated");
#endif
    }
    if (!support.empty()) {
        paths.push_back(support / "mods/integrated");
        paths.push_back(support / "bundle-mods/integrated");
    }
    const auto active = standalone_overlay();
    if (!active.empty()) paths.push_back(fs::u8path(active));
    std::vector<std::string> result;
    for (const auto &path : paths) {
        std::error_code ec;
        if (!fs::is_directory(path, ec)) continue;
        const auto real = fs::canonical(path, ec).u8string();
        if (!ec && std::find(result.begin(), result.end(), real) == result.end()) result.push_back(real);
    }
    return result;
}

void restart_with(const std::string &overlay, const Command &invite) {
    char type;
    switch (invite.kind) {
    case Command::Kind::JoinCode: type = 'C'; break;
    case Command::Kind::JoinAddress: type = 'A'; break;
    case Command::Kind::JoinCrew: type = 'F'; break;
    default: return;
    }
    if (!startup_args || invite.text.empty() || invite.text.size() > 128) return;
    if (!environment("EOD_ONLINE_PROFILE", overlay.empty() ? "disc" : "overlay:" + overlay)) return;
    if (environment("EOD_ONLINE_REJOIN", std::string(1, type) + ":" + invite.text)) {
        std::fflush(nullptr);
#ifdef _WIN32
        // Reuse the original UTF-16 command line, including quoted disc and configuration paths.
        std::wstring args(GetCommandLineW());
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (length && length < path.size() && CreateProcessW(path.c_str(), args.data(), nullptr, nullptr,
                FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess); app_exit(0);
        }
#elif defined(__APPLE__)
        // Keep the same process and its standard streams, closing game sockets and file handles.
        posix_spawnattr_t attr;
        posix_spawn_file_actions_t actions;
        if (posix_spawnattr_init(&attr) == 0) {
            if (posix_spawn_file_actions_init(&actions) == 0) {
                if (posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC | POSIX_SPAWN_CLOEXEC_DEFAULT) == 0) {
                    for (int fd = 0; fd <= 2; ++fd) posix_spawn_file_actions_addinherit_np(&actions, fd);
                    pid_t child;
                    posix_spawnp(&child, startup_args[0], &actions, &attr, startup_args, environ);
                }
                posix_spawn_file_actions_destroy(&actions);
            }
            posix_spawnattr_destroy(&attr);
        }
#else
        execvp(startup_args[0], startup_args);
#endif
    }
    environment("EOD_ONLINE_REJOIN", {});
    environment("EOD_ONLINE_PROFILE", {});
}
}
