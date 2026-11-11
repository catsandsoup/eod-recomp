"""Bounded in-memory caches installed over modding_backend's readers.

Archive directories and member payloads are keyed by the file's identity
(resolved path, size, mtime_ns, inode, device), so an edited or replaced file is
a different key and is never served stale. Decoded textures are keyed by the
payload digest (edited texture bytes therefore decode separately). All caches
are LRU with entry and byte bounds; callers receive copies of mutable values.
"""
import collections
import hashlib
import os
import threading

# Scratch archives the backend builds in temporary folders are read once to
# verify them; caching their payloads would only evict useful entries.
SCRATCH_PREFIXES = ("eod-build-", "eod-install-", "eod-bus-paint-", ".eod-")


class LRU:
    def __init__(self, name, max_entries, max_bytes):
        self.name, self.max_entries, self.max_bytes = name, max_entries, max_bytes
        self.data = collections.OrderedDict()
        self.bytes = 0
        self.hits = self.misses = self.evictions = 0
        self.lock = threading.Lock()

    def get(self, key):
        with self.lock:
            item = self.data.get(key)
            if item is None:
                self.misses += 1
                return None
            self.data.move_to_end(key)
            self.hits += 1
            return item[0]

    def put(self, key, value, size=1):
        if size > self.max_bytes:
            return
        with self.lock:
            old = self.data.pop(key, None)
            if old is not None:
                self.bytes -= old[1]
            self.data[key] = (value, size)
            self.bytes += size
            while self.data and (len(self.data) > self.max_entries or self.bytes > self.max_bytes):
                _, (_, freed) = self.data.popitem(last=False)
                self.bytes -= freed
                self.evictions += 1

    def clear(self):
        with self.lock:
            self.data.clear()
            self.bytes = 0

    def stats(self):
        with self.lock:
            return dict(entries=len(self.data), bytes=self.bytes, maxEntries=self.max_entries, maxBytes=self.max_bytes,
                        hits=self.hits, misses=self.misses, evictions=self.evictions)


MB = 1 << 20
DIRECTORIES = LRU("directories", 512, 64 * MB)
PAYLOADS = LRU("payloads", 4096, int(os.environ.get("EOD_KIT_PAYLOAD_CACHE_MB", "256")) * MB)
DECODED = LRU("decoded", 2048, int(os.environ.get("EOD_KIT_DECODE_CACHE_MB", "256")) * MB)
RESPONSES = LRU("responses", 256, int(os.environ.get("EOD_KIT_RESPONSE_CACHE_MB", "256")) * MB)
ALL = (DIRECTORIES, PAYLOADS, DECODED, RESPONSES)


def file_key(path):
    """(path, size, mtime_ns, inode, device) or None when the file is unreadable."""
    try:
        text = os.fspath(path)
        st = os.stat(text)
    except (OSError, TypeError):
        return None
    return (os.path.realpath(text), st.st_size, st.st_mtime_ns, st.st_ino, st.st_dev)


def _scratch(path):
    parts = os.fspath(path).split(os.sep)
    return any(p.startswith(SCRATCH_PREFIXES) for p in parts[:-1])


def install(mb):
    """Wrap modding_backend.directory / payload / decode_preview with caches (idempotent)."""
    if getattr(mb, "_kit_cache_installed", False):
        return
    original_directory, original_payload, original_decode = mb.directory, mb.payload, mb.decode_preview

    def directory(path):
        key = file_key(path)
        if key is None or _scratch(path):
            return original_directory(path)
        hit = DIRECTORIES.get(key)
        if hit is None:
            hit = original_directory(path)
            DIRECTORIES.put(key, hit, 200 + 120 * len(hit))
        return [dict(e) for e in hit]

    def payload(path, name, entries=None, verify_crc=True):
        key = file_key(path)
        if key is None or _scratch(path):
            return original_payload(path, name, entries, verify_crc)
        key = key + (name, bool(verify_crc))
        hit = PAYLOADS.get(key)
        if hit is None:
            data, entry = original_payload(path, name, entries, verify_crc)
            hit = (data, dict(entry))
            PAYLOADS.put(key, hit, len(data) + 256)
        return hit[0], dict(hit[1])

    def decode_preview(data):
        key = (len(data), hashlib.blake2b(data, digest_size=20).digest())
        hit = DECODED.get(key)
        if hit is None:
            w, h, rgba, meta = original_decode(data)
            hit = (w, h, rgba, dict(meta))
            DECODED.put(key, hit, len(rgba) + 512)
        w, h, rgba, meta = hit
        return w, h, rgba, dict(meta)

    directory.__wrapped__, payload.__wrapped__, decode_preview.__wrapped__ = original_directory, original_payload, original_decode
    mb.directory, mb.payload, mb.decode_preview = directory, payload, decode_preview
    mb._kit_cache_installed = True
    # Modules that imported these names before the patch keep their own bindings.
    import sys
    for name in ("mod_package", "mod_install", "asset_registry", "texture_export"):
        module = sys.modules.get(name)
        if module is None:
            continue
        for attr, value in (("directory", directory), ("payload", payload), ("decode_preview", decode_preview)):
            if hasattr(module, attr):
                setattr(module, attr, value)


def stats():
    return {c.name: c.stats() for c in ALL}


def clear():
    for c in ALL:
        c.clear()
