"""Recompiler patches for Battle-on-every-map stay in step with src/battle_layout.h
and only rewrite the audited instruction sites (requires rabbitizer, like recomp.py)."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import recomp  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


class BattleLayoutTests(unittest.TestCase):
    def test_header_constants(self):
        b = recomp.BATTLE
        self.assertEqual((b['kRanking'], b['kSpawnIndex'], b['kCount'], b['kMasterSize']), (0x814, 0x844, 0x848, 0x84C))
        self.assertEqual(b['kContestTable'], 0x90000)
        self.assertEqual(b['kContestCapacity'], 512)
        self.assertLessEqual(b['kContestTable'] + b['kContestCapacity'] * b['kContestStride'], b['kStrings'])
        self.assertLessEqual(b['kMaxCars'], b['kCapacity'])

    def test_battle_sites_only_move_master_fields(self):
        for va, (old, new) in recomp.WORD_PATCHES.items():
            self.assertEqual(old & 0xFFFF0000, new & 0xFFFF0000, 'opcode and registers are untouched')
            if va == 0x00124054:
                self.assertEqual((old & 0xFFFF, new & 0xFFFF), (320, 512))
                continue
            if va == recomp.ASCARLIST_ALLOC_SITE:
                continue  # covered by test_ascarlist_allocation_holds_added_vehicles
            self.assertTrue(0x166B00 <= va < 0x168000, 'Battle class methods only')
            self.assertIn(old & 0xFFFF, (0x594, 0x5A4, 0x5A8, 0x5AC))
        self.assertEqual(len(recomp.WORD_PATCHES), 27)

    def test_ascarlist_allocation_holds_added_vehicles(self):
        old, new = recomp.WORD_PATCHES[recomp.ASCARLIST_ALLOC_SITE]
        self.assertEqual(old, 0x24040A28, 'native: addiu $a0,$zero,130*0x14')
        self.assertEqual(old >> 16, new >> 16, 'same addiu $a0,$zero,imm')
        self.assertEqual(old & 0xFFFF, 130 * 0x14)
        # retail buycar.val has exactly 130 vehicles (full); integrated has 132
        self.assertGreaterEqual((new & 0xFFFF) // 0x14, 132)
        self.assertLess(new & 0xFFFF, 0x8000, 'addiu immediate stays positive')

    def test_contest_sites_become_absolute(self):
        class Elf:
            def read32(self, va): return recomp.CONTEST_TABLE_SITES[va]
        gen = recomp.Gen(Elf(), set(), {}, {})
        for va, word in recomp.CONTEST_TABLE_SITES.items():
            self.assertEqual(word >> 26, 0x09, 'addiu')
            c = gen.op(va)
            expected = 0x90000 + (word & 0xFFFF) - 0x61A8
            self.assertIn('0x%X' % expected, c)
            self.assertNotIn('0x61A', c)

    def test_owned_executable_matches_sites(self):
        elf_path = ROOT / 'SLUS_209.10'
        if not elf_path.exists():
            self.skipTest('owned executable not present')
        recomp.check_patch_sites(recomp.Elf(str(elf_path)))


if __name__ == '__main__':
    unittest.main()
