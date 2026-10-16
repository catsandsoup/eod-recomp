#pragma once
// Transport only: decompressing a capture yields the unchanged EODGSD01 ABI.
// Concatenated gzip members are standard gzip (RFC1952); commit one member per
// completed vblank so size limits never expose a partially drawn next frame.
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#ifdef EOD_HAVE_ZLIB
#include <zlib.h>
#endif

namespace gs::capture {
// Strict decimal opt-in limits: reject signs, trailing garbage and overflow.
inline bool parse_limit(const char *text, uint64_t minimum, uint64_t maximum,
                        uint64_t fallback, uint64_t &result) {
    if (!text) { result = fallback; return true; }
    if (!*text) return false;
    uint64_t value = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        const uint64_t digit = uint64_t(*p - '0');
        if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10)) return false;
        value = value * 10 + digit;
    }
    if (value < minimum || value > maximum) return false;
    result = value; return true;
}
inline bool gzip_path(const char *path) {
    const size_t n = std::strlen(path);
    return n >= 3 && std::strcmp(path + n - 3, ".gz") == 0;
}
class Writer {
    FILE *file = nullptr;
    uint64_t disk = 0, decoded = 0, disk_limit, decoded_limit;
    size_t pending_limit;
    std::vector<uint8_t> pending;
    bool ok = false, active = false;
#ifdef EOD_HAVE_ZLIB
    z_stream stream{};
    bool pump(const void *data, size_t size, int flush) {
        if (!ok || size > UINT32_MAX) return false;
        if (!active) {
            stream = {};
            if (deflateInit2(&stream, 1, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
                return ok = false;
            active = true;
        }
        stream.next_in = (Bytef *)data;
        stream.avail_in = (uInt)size;
        uint8_t chunk[32768];
        int result;
        do {
            stream.next_out = chunk;
            stream.avail_out = sizeof chunk;
            result = deflate(&stream, flush);
            // The previous chunk may have been exactly full, with no input or
            // output remaining on this final Z_NO_FLUSH call.
            if (result == Z_BUF_ERROR && flush == Z_NO_FLUSH && !stream.avail_in) return true;
            if (result != Z_OK && result != Z_STREAM_END) return ok = false;
            const size_t count = sizeof chunk - stream.avail_out;
            if (count > pending_limit - pending.size()) return ok = false;
            pending.insert(pending.end(), chunk, chunk + count);
        } while (stream.avail_in || !stream.avail_out || (flush == Z_FINISH && result != Z_STREAM_END));
        return true;
    }
#endif
public:
    Writer(const char *path, uint64_t disk_cap = 256ull << 20,
           uint64_t decoded_cap = 16ull << 30, size_t pending_cap = 32u << 20)
        : disk_limit(disk_cap), decoded_limit(decoded_cap), pending_limit(pending_cap) {
#ifdef EOD_HAVE_ZLIB
        file = std::fopen(path, "wb"); ok = file != nullptr;
#else
        (void)path;
#endif
    }
    ~Writer() {
#ifdef EOD_HAVE_ZLIB
        if (active) deflateEnd(&stream);
#endif
        if (file) std::fclose(file);
    }
    bool good() const { return ok; }
    bool write(const void *data, size_t size) {
        if (!ok || size > decoded_limit - decoded) return ok = false;
        decoded += size;
#ifdef EOD_HAVE_ZLIB
        return pump(data, size, Z_NO_FLUSH);
#else
        (void)data; return false;
#endif
    }
    bool checkpoint() {
#ifdef EOD_HAVE_ZLIB
        if (!pump(nullptr, 0, Z_FINISH)) return false;
        deflateEnd(&stream); active = false;
        if (pending.size() > disk_limit - disk) return ok = false;
        // A physical write error cannot be repaired into a valid capture; callers
        // report it and must not treat the resulting file as accepted evidence.
        if (std::fwrite(pending.data(), 1, pending.size(), file) != pending.size() || std::fflush(file))
            return ok = false;
        disk += pending.size(); pending.clear();
        return true;
#else
        return false;
#endif
    }
    uint64_t bytes() const { return disk; }
};

class Reader {
    FILE *file = nullptr;
    bool error = false, compressed = false;
    uint64_t decoded = 0;
#ifdef EOD_HAVE_ZLIB
    gzFile zip = nullptr;
#endif
public:
    explicit Reader(const char *path) : compressed(gzip_path(path)) {
        if (!compressed) file = std::fopen(path, "rb");
#ifdef EOD_HAVE_ZLIB
        else { zip = gzopen(path, "rb"); if (zip) gzbuffer(zip, 128u << 10); }
#endif
    }
    ~Reader() { if (file) std::fclose(file);
#ifdef EOD_HAVE_ZLIB
        if (zip) gzclose(zip);
#endif
    }
    bool good() const {
#ifdef EOD_HAVE_ZLIB
        return !error && (file || zip);
#else
        return !error && file;
#endif
    }
    bool failed() const { return error; }
    size_t read(void *data, size_t size) {
        if (!good() || size > (64ull << 30) - decoded) { error = true; return 0; }
        size_t done = 0;
        while (done < size) {
            const size_t count = std::min(size - done, size_t(1u << 20));
            size_t n = 0;
#ifdef EOD_HAVE_ZLIB
            if (zip) {
                int result = gzread(zip, (uint8_t *)data + done, (unsigned)count);
                if (result < 0) { error = true; break; }
                n = (size_t)result;
                int code; gzerror(zip, &code);
                if (code != Z_OK && code != Z_STREAM_END) error = true;
            } else
#endif
            { n = std::fread((uint8_t *)data + done, 1, count, file); if (std::ferror(file)) error = true; }
            done += n;
            if (n != count || error) break;
        }
        decoded += done;
        return done;
    }
    bool rewind() {
        error = false; decoded = 0;
#ifdef EOD_HAVE_ZLIB
        if (zip) return gzrewind(zip) == 0;
#endif
        return file && std::fseek(file, 0, SEEK_SET) == 0;
    }
    // 1 command, 0 clean EOF, -1 truncated/invalid/oversized stream.
    int command(std::vector<uint8_t> &out) {
        uint32_t h[4];
        const size_t n = read(h, sizeof h);
        if (!n) return error ? -1 : 0;
        if (n != sizeof h || h[3] > (64u << 20) || h[0] < 1 || h[0] > 4 ||
            (h[0] == 1 && (h[1] >= 4 || h[3] % 16)) ||
            (h[0] == 2 && h[3] != 8) || (h[0] >= 3 && h[3])) return -1;
        const size_t payload = (size_t(h[3]) + 15) & ~size_t(15);
        out.resize(sizeof h + payload);
        std::memcpy(out.data(), h, sizeof h);
        if (read(out.data() + sizeof h, payload) != payload || error) return -1;
        return 1;
    }
};
} // namespace gs::capture
