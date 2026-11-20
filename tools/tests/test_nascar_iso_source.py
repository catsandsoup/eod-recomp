"""Source adapter fixtures use synthetic ISO9660 metadata and inert archive bytes."""

from hashlib import sha256
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from iso9660_reader import FAMILY_ARCHIVES, NascarSource, NascarSourceCancelled, NascarSourceError


SECTOR = 2048
STOCK_BYTES = b"fixture stock"
PAINT_BYTES = b"fixture paint"


def both(value, size):
    return value.to_bytes(size, "little") + value.to_bytes(size, "big")


def record(identifier, sector, size, *, directory=False):
    identifier = identifier.encode("ascii")
    length = 33 + len(identifier) + (len(identifier) % 2 == 0)
    result = bytearray(length)
    result[0] = length
    result[2:10] = both(sector, 4)
    result[10:18] = both(size, 4)
    result[25] = 2 if directory else 0
    result[28:32] = both(1, 2)
    result[32] = len(identifier)
    result[33:33 + len(identifier)] = identifier
    return result


def make_iso(*, boot="SLUS_204.41", boot_target=None, paint=True, nested=True,
             stock=STOCK_BYTES):
    stock_blocks = (len(stock) + SECTOR - 1) // SECTOR
    sectors = 26 + stock_blocks
    image = bytearray(SECTOR * sectors)

    def write(sector, data):
        image[sector * SECTOR:sector * SECTOR + len(data)] = data

    def directory(sector, parent, records):
        write(sector, record("\x00", sector, SECTOR, directory=True)
              + record("\x01", parent, SECTOR, directory=True) + b"".join(records))

    cnf = f"BOOT2 = cdrom0:\\{boot_target or boot};1\r\nVER = 1.00\r\n".encode()
    pvd = bytearray(SECTOR)
    pvd[:7] = b"\x01CD001\x01"
    pvd[80:88] = both(sectors, 4)
    pvd[120:124] = both(1, 2)
    pvd[124:128] = both(1, 2)
    pvd[128:132] = both(SECTOR, 2)
    pvd[156:190] = record("\x00", 18, SECTOR, directory=True)
    pvd[881] = 1
    write(16, pvd)
    write(17, b"\xffCD001\x01")
    directory(18, 18, [record("dAtA", 19, SECTOR, directory=True),
                       record("system.cnf;1", 22, len(cnf)),
                       record(f"{boot.lower()};1", 23, 3)])
    vehicle_records = [record("Stock.Car;1", 26, len(stock)), record("OTHER.CAR;1", 25, 5)]
    data_records = [record("rEs", 21, SECTOR, directory=True)]
    if nested:
        directory(20, 19, vehicle_records)
        data_records.append(record("cAr", 20, SECTOR, directory=True))
    else:
        data_records.extend(vehicle_records)
    directory(19, 18, data_records)
    directory(21, 19, [record("Paint256.Res;1", 24, len(PAINT_BYTES))] if paint else [])
    write(22, cnf)
    write(23, b"elf")
    write(24, PAINT_BYTES)
    write(25, b"other")
    write(26, stock)
    return image


class TestNascarIsoSource(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def iso(self, image):
        source = self.root / "disc.iso"
        source.write_bytes(image)
        return source

    def test_iso_83_names_are_discovered_and_only_selected_archives_stage(self):
        image = make_iso()
        path = self.iso(image)
        path.chmod(0o444)
        source = NascarSource(path)
        self.assertEqual(source.archives, {"CAR/STOCK.CAR", "RES/PAINT256.RES"})
        self.assertEqual(source.available_families, ("stock",))
        with source.stage(["CAR/STOCK.CAR"]) as staged:
            self.assertEqual(list(staged.iterdir()), [staged / "STOCK.CAR"])
            self.assertEqual((staged / "STOCK.CAR").read_bytes(), STOCK_BYTES)
        self.assertFalse(staged.exists())
        self.assertEqual(sha256(path.read_bytes()).digest(), sha256(image).digest())

    def test_flat_data_stock_layout_used_by_retail_disc(self):
        source = NascarSource(self.iso(make_iso(nested=False)))
        with source.stage(FAMILY_ARCHIVES["stock"]) as staged:
            self.assertEqual((staged / "STOCK.CAR").read_bytes(), STOCK_BYTES)
            self.assertEqual((staged / "RES" / "PAINT256.RES").read_bytes(), PAINT_BYTES)

    def test_extracted_root_and_data_paths_stage_private_copies(self):
        disc = self.root / "disc"
        data = disc / "dAtA"
        stock = data / "CaR" / "sToCk.cAr"
        paint = data / "rEs" / "pAiNt256.rEs"
        stock.parent.mkdir(parents=True)
        paint.parent.mkdir()
        stock.write_bytes(STOCK_BYTES)
        paint.write_bytes(PAINT_BYTES)
        stock.chmod(0o444)
        for path in (disc, data):
            with self.subTest(path=path), NascarSource(path).stage(FAMILY_ARCHIVES["stock"]) as staged:
                self.assertEqual((staged / "RES" / "PAINT256.RES").read_bytes(), PAINT_BYTES)
                (staged / "STOCK.CAR").write_bytes(b"conversion scratch")
            self.assertFalse(staged.exists())
            self.assertEqual(stock.read_bytes(), STOCK_BYTES)

    def test_wrong_boot_id_and_wrong_boot_target_are_rejected(self):
        for kwargs in ({"boot": "SLUS_209.10"}, {"boot_target": "SLUS_209.10"}):
            with self.subTest(kwargs=kwargs), self.assertRaisesRegex(NascarSourceError, r"SLUS_204\.41"):
                NascarSource(self.iso(make_iso(**kwargs)))

    def test_foreign_extracted_boot_metadata_is_rejected(self):
        data = self.root / "DATA"
        data.mkdir()
        (data / "STOCK.CAR").write_bytes(STOCK_BYTES)
        (self.root / "SYSTEM.CNF").write_text("BOOT2 = cdrom0:\\SLUS_209.10;1")
        with self.assertRaisesRegex(NascarSourceError, r"SLUS_204\.41"):
            NascarSource(data)

    def test_missing_paint_prevents_staging_stock_family(self):
        source = NascarSource(self.iso(make_iso(paint=False)))
        self.assertEqual(source.available_families, ())
        with self.assertRaisesRegex(NascarSourceError, r"PAINT256\.RES"):
            with source.stage(FAMILY_ARCHIVES["stock"]):
                self.fail("missing paint was accepted")

    def test_truncated_image_directory_records_and_out_of_bounds_extents(self):
        image = make_iso()
        truncated_record = bytearray(image)
        truncated_record[20 * SECTOR + 68 + 32] = 255
        out_of_bounds = bytearray(image)
        out_of_bounds[20 * SECTOR + 70:20 * SECTOR + 78] = both(len(image) // SECTOR, 4)
        for image in (image[:-1], truncated_record, out_of_bounds):
            with self.subTest(), self.assertRaisesRegex(NascarSourceError, r"(?i)truncated|invalid|bounds"):
                NascarSource(self.iso(image))

    def test_unsafe_iso_names_and_archive_requests_are_rejected(self):
        image = make_iso()
        struct.pack_into("B", image, 20 * SECTOR + 68 + 33, ord("/"))
        with self.assertRaises(NascarSourceError):
            NascarSource(self.iso(image))
        source = NascarSource(self.iso(make_iso()))
        with self.assertRaisesRegex(NascarSourceError, "archive"):
            with source.stage(["../STOCK.CAR"]):
                self.fail("unsafe archive request was accepted")

    def test_extracted_symlink_cannot_read_outside_data(self):
        data = self.root / "DATA"
        data.mkdir()
        outside = self.root / "original.car"
        outside.write_bytes(STOCK_BYTES)
        try:
            (data / "STOCK.CAR").symlink_to(outside)
        except OSError as exc:
            self.skipTest(f"Creating symlinks is unavailable: {exc}")
        with self.assertRaises(NascarSourceError):
            NascarSource(data)

    def test_staging_parent_cannot_be_inside_an_extracted_disc(self):
        data = self.root / "DATA"
        data.mkdir()
        (data / "STOCK.CAR").write_bytes(STOCK_BYTES)
        source = NascarSource(data)
        with self.assertRaisesRegex(NascarSourceError, "outside"):
            with source.stage(["CAR/STOCK.CAR"], temp_dir=data):
                self.fail("staging inside source was accepted")
        self.assertEqual(list(data.iterdir()), [data / "STOCK.CAR"])

    def test_cancellation_removes_partial_staging_and_keeps_iso(self):
        path = self.iso(make_iso(stock=b"x" * (3 * 1024 * 1024)))
        source = NascarSource(path)
        saw_partial_copy = False

        def cancel_during_copy():
            nonlocal saw_partial_copy
            staged = list(self.root.glob("*/DATA/STOCK.CAR"))
            saw_partial_copy = any(0 < car.stat().st_size < 3 * 1024 * 1024 for car in staged)
            return saw_partial_copy

        with self.assertRaises(NascarSourceCancelled):
            with source.stage(["CAR/STOCK.CAR"], cancelled=cancel_during_copy, temp_dir=self.root):
                self.fail("cancelled import yielded staging")
        self.assertTrue(saw_partial_copy)
        self.assertEqual(list(self.root.iterdir()), [path])


if __name__ == "__main__":
    unittest.main()
