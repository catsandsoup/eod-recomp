// Exercise the target SDK 2.8 MC packets across independent host processes.
#include "iop/iop.h"
#include "core/log.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>

static uint8_t ram[65536];
uint8_t *g_ram = ram;
bool g_log_enabled[LOG_CAT_COUNT] = {};
std::string g_game_root;
void log_write(LogCat, const char *, ...) {}
void *ee_ptr(uint32_t a) { if (a >= sizeof ram) std::abort(); return ram + a; }
void ee_write8_slow(uint32_t, uint8_t) { std::abort(); }
uint8_t ee_read8_slow(uint32_t) { std::abort(); }
static RpcHandler handler;
void iop_register_server(uint32_t, const char *, RpcHandler h) { handler = std::move(h); }
static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
static void word(uint8_t *p, int at, uint32_t value) { memcpy(p + at, &value, 4); }
static int call(uint32_t command, uint8_t *packet) {
    RpcCall c{}; c.fno = command;
    CHECK(handler(c, packet) == 4);
    int result; memcpy(&result, packet, 4); return result;
}
static int open(int port, const char *name, uint32_t flags) {
    uint8_t packet[0x414] = {};
    word(packet, 0, port); word(packet, 8, flags);
    snprintf((char *)packet + 0x14, 1024, "%s", name);
    return call(2, packet);
}
static int desc(uint32_t command, int fd, int offset = 0, int origin = 0) {
    uint8_t packet[48] = {};
    word(packet, 0, fd); word(packet, 0x10, offset); word(packet, 0x14, origin);
    return call(command, packet);
}
static uint8_t pattern(int port, int byte) { return (uint8_t)(byte * 37 + port * 61); }
static int write(int fd, int port, uint32_t src, int size) {
    for (int i = 0; i < size; i++) ram[src + i] = pattern(port, i);
    // Target producer 0x0038C530 sends an inline unaligned head plus an
    // aligned EE buffer, with size excluding that head.
    int head = size <= 16 ? size : ((16 - (src & 15)) & 15);
    uint8_t packet[48] = {};
    word(packet, 0, fd); word(packet, 12, size - head);
    word(packet, 20, head); word(packet, 24, src + head);
    memcpy(packet + 32, ram + src, head);
    return call(6, packet);
}
static void read_check(int fd, int port, int size) {
    constexpr uint32_t dest = 0x3003, end = 0x5000;
    memset(ram + dest - 1, 0xA5, size + 2); memset(ram + end, 0xA5, 192);
    uint8_t packet[48] = {};
    word(packet, 0, fd); word(packet, 12, size + 10); // short EOF read
    word(packet, 24, dest); word(packet, 28, end);
    CHECK(call(5, packet) == size);
    for (int i = 0; i < size; i++) CHECK(ram[dest + i] == pattern(port, i));
    CHECK(ram[dest - 1] == 0xA5 && ram[dest + size] == 0xA5);
    // Callback head/tail sizes must be zero after direct HLE writes.
    uint32_t first, second; memcpy(&first, ram + end, 4); memcpy(&second, ram + end + 4, 4);
    CHECK(first == 0 && second == 0);
}
static int phase(bool writing, const char *root) {
    setenv("EOD_MEMCARD", root, 1); iop_mc_init();
    for (int port = 0; port < 2; port++) {
        const char *path = "/BASLUS-20910/career";
        if (writing) {
            CHECK(open(port, "/BASLUS-20910", 0x40) == 0);
            int fd = open(port, path, 0x200); CHECK(fd >= 0);
            CHECK(write(fd, port, 0x2001, 1025) == 1025);
            CHECK(desc(3, fd) == 0);
            // MCMAN 0x200 recreates/truncates an existing file, despite
            // the public header describing the bit as O_CREAT.
            fd = open(port, path, 0x200); CHECK(fd >= 0);
            CHECK(write(fd, port, 0x2000, 257) == 257);
            CHECK(desc(3, fd) == 0);
            fd = open(port, "/BASLUS-20910/short", 0x200); CHECK(fd >= 0);
            CHECK(write(fd, port, 0x2003, 11) == 11); CHECK(desc(3, fd) == 0);
        } else {
            int fd = open(port, path, 1); CHECK(fd >= 0);
            CHECK(desc(4, fd, 0, 2) == 257); // game's existence/length check
            CHECK(desc(4, fd, 0, 0) == 0); read_check(fd, port, 257);
            CHECK(desc(3, fd) == 0);
            fd = open(port, "/BASLUS-20910/short", 1); CHECK(fd >= 0);
            read_check(fd, port, 11); CHECK(desc(3, fd) == 0);
        }
    }
    for (int port = 0; port < 2; port++) {
        std::filesystem::path folder = std::filesystem::path(root) / (port ? "port1" : "port0") / "BASLUS-20910";
        CHECK(std::filesystem::exists(folder / "career"));
        CHECK(std::filesystem::exists(folder / "short"));
        if (!writing) {
            uint8_t packet[0x414] = {};
            word(packet, 0, port);
            snprintf((char *)packet + 0x14, 1024, "%s", "/BASLUS-20910/short");
            CHECK(call(0x0F, packet) == 0);
            CHECK(!std::filesystem::exists(folder / "short"));
        }
    }
    return failures ? 1 : 0;
}
int main(int argc, char **argv) {
    if (argc == 3) return phase(!strcmp(argv[1], "--write"), argv[2]);
    char root[] = "mc-test-XXXXXX";
    if (!mkdtemp(root)) return 2;
    int result = 0;
    for (const char *mode : {"--write", "--read"}) {
        pid_t child = fork();
        if (child == 0) { execl(argv[0], argv[0], mode, root, (char *)nullptr); _exit(2); }
        int status;
        if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
            result = 1; break;
        }
    }
    std::filesystem::remove_all(root);
    printf("memory-card packet persistence across processes: %s\n", result ? "FAILED" : "passed");
    return result;
}
