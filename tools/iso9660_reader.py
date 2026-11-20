"""Read Dirt to Daytona disc metadata and stage selected archives for conversion."""

from collections.abc import Callable, Iterable, Iterator
from contextlib import contextmanager, ExitStack
from dataclasses import dataclass
import os
from pathlib import Path
import re
from tempfile import gettempdir, TemporaryDirectory


FAMILY_ARCHIVES = {
    "stock": ("CAR/STOCK.CAR", "RES/PAINT256.RES"),
    "truck": ("CAR/TRUCK.CAR", "RES/TRUCKSET.RES"),
    "modified": ("CAR/MODIFIED.CAR", "RES/MODFYSET.RES"),
    "hobby": ("CAR/HOBBY.CAR", "RES/HOBBYSET.RES"),
    "pacecar": ("CAR/PACECAR.CAR",),
}
_CAR_ARCHIVES = tuple(keys[0] for keys in FAMILY_ARCHIVES.values())
_RES_ARCHIVES = (
    *(keys[1] for keys in FAMILY_ARCHIVES.values() if len(keys) == 2),
    "RES/COMMON.RES", "RES/RACE.RES", "RES/UIBIG.RES", "RES/PAINTKIT.RES",
)
_ARCHIVES = _CAR_ARCHIVES + _RES_ARCHIVES
_BOOT_ID = "SLUS_204.41"
_SECTOR = 2048
_CHUNK = 1024 * 1024
_BOOT_CONFIG_LIMIT = 16 * 1024


class NascarSourceError(ValueError):
    """The selected source or archive could not be read or validated."""


class NascarSourceCancelled(NascarSourceError):
    """The caller cancelled source staging."""


@dataclass(frozen=True)
class _Record:
    offset: int
    size: int
    is_dir: bool


def _both_endian(data, offset, width):
    little = int.from_bytes(data[offset:offset + width], "little")
    big = int.from_bytes(data[offset + width:offset + width * 2], "big")
    if little != big:
        raise NascarSourceError("Invalid ISO9660 numeric field.")
    return little


class _Iso9660:
    """Primary ISO9660 tree with 2048-byte sectors and single-extent entries."""

    def __init__(self, stream):
        self.stream = stream
        self.limit = os.fstat(stream.fileno()).st_size
        pvd = None
        # The descriptor set begins at sector 16. Boot records may precede the PVD.
        for sector in range(16, 80):
            block = self.read(sector * _SECTOR, _SECTOR)
            if block[1:7] != b"CD001\x01":
                raise NascarSourceError("Invalid ISO9660 volume descriptor.")
            if block[0] == 1 and pvd is None:
                pvd = block
                declared = _both_endian(pvd, 80, 4) * _SECTOR
                if not (sector + 1) * _SECTOR <= declared <= self.limit:
                    raise NascarSourceError("Truncated ISO or invalid ISO9660 volume size.")
                self.limit = declared
            if block[0] == 255:
                break
        else:
            raise NascarSourceError("Missing ISO9660 volume descriptor terminator.")
        if pvd is None or _both_endian(pvd, 128, 2) != _SECTOR:
            raise NascarSourceError("Expected a primary ISO9660 volume with 2048-byte blocks.")
        name, self.root = self._record(pvd[156:190])
        if name != "\x00" or not self.root.is_dir:
            raise NascarSourceError("Invalid ISO9660 root directory.")

    def read(self, offset, length):
        if offset + length > self.limit:
            raise NascarSourceError("ISO9660 extent is outside image bounds.")
        self.stream.seek(offset)
        data = self.stream.read(length)
        if len(data) != length:
            raise NascarSourceError("Truncated ISO9660 extent.")
        return data

    def _record(self, data):
        if len(data) < 34 or data[0] != len(data):
            raise NascarSourceError("Invalid or truncated ISO9660 directory record.")
        name_length = data[32]
        if not name_length or 33 + name_length + (name_length % 2 == 0) > len(data):
            raise NascarSourceError("Invalid ISO9660 directory identifier length.")
        if data[25] & 0x80 or data[26:28] != b"\0\0" or _both_endian(data, 28, 2) != 1:
            raise NascarSourceError("Unsupported ISO9660 multi-extent, interleaved, or multi-volume entry.")
        offset = (_both_endian(data, 2, 4) + data[1]) * _SECTOR
        size = _both_endian(data, 10, 4)
        is_dir = bool(data[25] & 2)
        if offset + size > self.limit or (is_dir and not size):
            raise NascarSourceError("Invalid ISO9660 directory or file extent bounds.")

        raw_name = data[33:33 + name_length]
        if raw_name in (b"\0", b"\1"):
            return raw_name.decode("ascii"), _Record(offset, size, is_dir)
        try:
            name = re.sub(r";[0-9]+$", "", raw_name.decode("ascii")).removesuffix(".")
        except UnicodeDecodeError as exc:
            raise NascarSourceError("Invalid ISO9660 directory identifier.") from exc
        if not re.fullmatch(r"[a-zA-Z0-9_.-]+", name) or name in (".", ".."):
            raise NascarSourceError("Invalid ISO9660 directory identifier.")
        return name.upper(), _Record(offset, size, is_dir)

    def children(self, directory):
        if not directory.is_dir:
            raise NascarSourceError("Expected an ISO9660 directory.")
        children = {}
        for offset in range(0, directory.size, _SECTOR):
            block = self.read(directory.offset + offset, min(_SECTOR, directory.size - offset))
            pos = 0
            while pos < len(block) and block[pos]:
                length = block[pos]
                name, entry = self._record(block[pos:pos + length])
                pos += length
                if name in ("\0", "\1"):
                    continue
                if name in children:
                    raise NascarSourceError(f"Ambiguous ISO9660 directory entry {name}.")
                children[name] = entry
        return children

    def file_size(self, entry):
        if entry.is_dir or not entry.size:
            raise NascarSourceError("Expected a non-empty ISO9660 file.")
        return entry.size

    def boot_config(self, entry, length):
        return self.read(entry.offset, length)

    def chunks(self, entry):
        for offset in range(0, entry.size, _CHUNK):
            yield self.read(entry.offset + offset, min(_CHUNK, entry.size - offset))


class _ExtractedDisc:
    @staticmethod
    def children(directory):
        if directory.is_symlink() or not directory.is_dir():
            raise NascarSourceError("Expected an extracted disc directory without symlinks.")
        children = {}
        for child in directory.iterdir():
            name = child.name.upper()
            if name in children:
                raise NascarSourceError(f"Ambiguous extracted disc entry {child.name}.")
            children[name] = child
        return children

    @staticmethod
    def file_size(entry):
        if entry.is_symlink() or not entry.is_file():
            raise NascarSourceError(f"Expected an extracted disc file without symlinks: {entry.name}.")
        size = entry.stat().st_size
        if not size:
            raise NascarSourceError(f"Empty extracted disc file: {entry.name}.")
        return size

    @staticmethod
    def boot_config(entry, length):
        with entry.open("rb") as stream:
            return stream.read(length)

    @staticmethod
    def chunks(entry):
        with entry.open("rb") as stream:
            while chunk := stream.read(_CHUNK):
                yield chunk


def _verify_boot(tree, root, *, required):
    config = root.get("SYSTEM.CNF")
    executable = root.get(_BOOT_ID)
    error = f"Expected a NASCAR: Dirt to Daytona disc with boot ID {_BOOT_ID}."
    if executable is not None:
        tree.file_size(executable)
    if config is not None:
        size = tree.file_size(config)
        if size > _BOOT_CONFIG_LIMIT:
            raise NascarSourceError("SYSTEM.CNF is too large.")
        config_text = tree.boot_config(config, size)
        if not re.search(rb"^\s*BOOT2\s*=\s*cdrom0:\\SLUS_204\.41(?:;[0-9]+)?[ \t\r]*$",
                         config_text, re.IGNORECASE | re.MULTILINE):
            raise NascarSourceError(error)
    foreign = any(re.fullmatch(r"S[A-Z]{3}_[0-9]{3}\.[0-9]{2}", name) for name in root)
    if executable is None and (required or foreign):
        raise NascarSourceError(error)


def _discover_archives(tree, data):
    car = tree.children(data["CAR"]) if "CAR" in data else {}
    res = tree.children(data["RES"]) if "RES" in data else {}
    found = {}
    for key in _ARCHIVES:
        folder, name = key.split("/")
        matches = (car, data) if folder == "CAR" else (res,)
        candidates = [children[name] for children in matches if name in children]
        if len(candidates) > 1:
            raise NascarSourceError(f"Ambiguous NASCAR archive {key} in DATA and DATA/CAR.")
        if candidates:
            entry = candidates[0]
            tree.file_size(entry)
            found[key] = entry
    if not any(key in found for key in _CAR_ARCHIVES):
        raise NascarSourceError("No Dirt to Daytona car archives found in DATA or DATA/CAR.")
    return found


class NascarSource:
    """Catalog a local .iso, extracted disc root, or extracted DATA folder."""

    def __init__(self, path: str | os.PathLike[str]):
        self.path = Path(os.path.abspath(path))
        try:
            with self._open() as (_, entries):
                self.archives = frozenset(entries)
        except OSError as exc:
            raise NascarSourceError(f"Could not read the NASCAR source: {exc}") from exc
        self.available_families = tuple(
            family for family, keys in FAMILY_ARCHIVES.items() if self.archives.issuperset(keys)
        )

    @contextmanager
    def _open(self):
        if self.path.is_dir():
            tree = _ExtractedDisc()
            root = tree.children(self.path)
            if "DATA" in root:
                _verify_boot(tree, root, required=False)
                data = tree.children(root["DATA"])
            else:
                if self.path.name.upper() == "DATA":
                    _verify_boot(tree, tree.children(self.path.parent), required=False)
                data = root
            yield tree, _discover_archives(tree, data)
        elif self.path.is_file() and self.path.suffix.lower() == ".iso":
            with self.path.open("rb") as stream:
                tree = _Iso9660(stream)
                root = tree.children(tree.root)
                _verify_boot(tree, root, required=True)
                if "DATA" not in root:
                    raise NascarSourceError("The Dirt to Daytona ISO is missing its DATA directory.")
                yield tree, _discover_archives(tree, tree.children(root["DATA"]))
        else:
            raise NascarSourceError("Select a local .iso, extracted disc root, or extracted DATA folder.")

    @contextmanager
    def stage(
        self,
        archives: Iterable[str],
        *,
        cancelled: Callable[[], bool] | None = None,
        temp_dir: str | os.PathLike[str] | None = None,
    ) -> Iterator[Path]:
        """Yield a private converter DATA folder, deleting it on exit or cancellation.

        Archive keys use the catalog's CAR/... and RES/... names. The existing
        converters expect cars directly under DATA. Callers can supply a
        thread-safe cancellation predicate and keep the context open during import.
        """
        keys = tuple(dict.fromkeys(archives))

        def check_cancelled():
            if cancelled is not None and cancelled():
                raise NascarSourceCancelled("NASCAR source staging cancelled.")

        check_cancelled()
        with ExitStack() as stack:
            try:
                stage_parent = Path(temp_dir if temp_dir is not None else gettempdir()).resolve()
                if self.path.is_dir() and stage_parent.is_relative_to(self.path.resolve()):
                    raise NascarSourceError("NASCAR staging must be outside the extracted source.")
                directory = stack.enter_context(
                    TemporaryDirectory(prefix="eod-nascar-source-", dir=stage_parent)
                )
                data = Path(directory) / "DATA"
                with self._open() as (tree, entries):
                    for key in keys:
                        if key not in entries:
                            raise NascarSourceError(f"Missing or unsupported NASCAR archive: {key}.")
                    data.mkdir()
                    for key in keys:
                        check_cancelled()
                        target = data / key.removeprefix("CAR/")
                        target.parent.mkdir(exist_ok=True)
                        with target.open("xb") as stream:
                            for chunk in tree.chunks(entries[key]):
                                check_cancelled()
                                stream.write(chunk)
                check_cancelled()
            except OSError as exc:
                raise NascarSourceError(f"Could not stage the NASCAR source: {exc}") from exc
            yield data
