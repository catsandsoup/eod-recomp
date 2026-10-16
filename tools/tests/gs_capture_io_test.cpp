// Bounded gzip transport must preserve the legacy byte stream and publish only
// completed vblank members, even when an unfinished frame exceeds a cap.
#include "../../runtime/gs/gs_capture_io.h"
#include <cstdio>
#include <cstring>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    uint64_t limit = 0;
    REQUIRE(gs::capture::parse_limit(nullptr, 1, 2048, 256, limit) && limit == 256);
    REQUIRE(gs::capture::parse_limit("1", 1, 2048, 256, limit) && limit == 1);
    REQUIRE(gs::capture::parse_limit("2048", 1, 2048, 256, limit) && limit == 2048);
    for (const char *invalid : {"", "0", "2049", "-1", "+1", "1x", " 1", "99999999999999999999999999999"})
        REQUIRE(!gs::capture::parse_limit(invalid, 1, 2048, 256, limit));
    REQUIRE(gs::capture::parse_limit("64", 1, 64, 16, limit) && limit == 64);
    REQUIRE(!gs::capture::parse_limit("65", 1, 64, 16, limit));
    const char *path = "gs-capture-io-test.gz";
    const char header[] = "EODGSD01";
    const uint32_t frame[4] = {3, 1, 0, 0};
    {
        gs::capture::Writer out(path, 4096, 1024, 128);
        REQUIRE(out.good());
        REQUIRE(out.write(header, sizeof header));
        REQUIRE(out.checkpoint());
        REQUIRE(out.write(frame, sizeof frame));
        REQUIRE(out.checkpoint());
        // No checkpoint: this partial next frame must never become visible.
        REQUIRE(out.write(frame, sizeof frame));
    }
    {
        gs::capture::Reader in(path);
        char bytes[sizeof header]; uint32_t decoded[4];
        REQUIRE(in.read(bytes, sizeof bytes) == sizeof bytes);
        REQUIRE(std::memcmp(bytes, header, sizeof bytes) == 0);
        REQUIRE(in.read(decoded, sizeof decoded) == sizeof decoded);
        REQUIRE(std::memcmp(decoded, frame, sizeof frame) == 0);
        REQUIRE(in.read(decoded, sizeof decoded) == 0);
        REQUIRE(!in.failed());
    }
    // Truncated gzip trailers are errors, even if all decoded bytes arrived.
    {
        FILE *f = std::fopen(path, "rb"); REQUIRE(f);
        std::fseek(f, 0, SEEK_END); long length = std::ftell(f); std::rewind(f);
        std::vector<unsigned char> bytes((size_t)length - 3);
        REQUIRE(std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size()); std::fclose(f);
        f = std::fopen(path, "wb"); REQUIRE(f);
        REQUIRE(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size()); std::fclose(f);
        gs::capture::Reader in(path); char result[128];
        in.read(result, sizeof result); REQUIRE(in.failed());
    }
    // Decoded limit rejects a frame and retains the previous complete member.
    {
        gs::capture::Writer out(path, 4096, sizeof header, 128);
        REQUIRE(out.write(header, sizeof header)); REQUIRE(out.checkpoint());
        REQUIRE(!out.write(frame, sizeof frame));
    }
    {
        gs::capture::Reader in(path); char bytes[32];
        REQUIRE(in.read(bytes, sizeof bytes) == sizeof header);
        REQUIRE(!in.failed());
    }
    // Physical cap cannot publish an incomplete gzip member.
    {
        gs::capture::Writer out(path, 1, 1024, 128);
        REQUIRE(out.write(header, sizeof header)); REQUIRE(!out.checkpoint());
    }
    // Pending compressed frame bound fails without an unbounded allocation.
    {
        gs::capture::Writer out(path, 4096, 1024, 1);
        REQUIRE(!out.write(frame, sizeof frame));
    }
    // Streaming command readers reject truncated headers and oversized payloads
    // before allocating or dispatching them.
    {
        FILE *f = std::fopen("gs-capture-io-test.raw", "wb"); REQUIRE(f);
        uint32_t invalid[4] = {1, 1, 0, (64u << 20) + 16};
        REQUIRE(std::fwrite(invalid, 1, sizeof invalid, f) == sizeof invalid); std::fclose(f);
        gs::capture::Reader in("gs-capture-io-test.raw"); std::vector<uint8_t> command;
        REQUIRE(in.command(command) == -1); REQUIRE(command.empty());
    }
    {
        FILE *f = std::fopen("gs-capture-io-test.raw", "wb"); REQUIRE(f);
        REQUIRE(std::fwrite(frame, 1, 3, f) == 3); std::fclose(f);
        gs::capture::Reader in("gs-capture-io-test.raw"); std::vector<uint8_t> command;
        REQUIRE(in.command(command) == -1);
    }
    std::remove(path); std::remove("gs-capture-io-test.raw");
    return 0;
}
