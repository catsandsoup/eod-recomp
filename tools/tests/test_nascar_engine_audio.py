"""Source ESFX behavior contracts; synthetic data remains independent of retail assets."""
import math
import os
import shutil
import subprocess
import struct
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from nascar_engine_audio import (parse_esfx, encode_esfx, source_caller_gain,
    evaluate_engine, namespace_sample, prepare_audio, source_view_context)


def record(name='eng10.sfx', values=(7806, .65, .96, 100, 3000, 8100, 80000)):
    return struct.pack('<16s7f', name.encode(), *values)


def profile():
    return parse_esfx(struct.pack('<I', 3) + record() * 2 + record('eng12.sfx',
        (7806, .07, .5, 6400, 6750, 8100, 80000)) + b'IsProcessorFeaturePresent' * 9)


class EsfxTests(unittest.TestCase):
    def test_direct_cli_help_avoids_tools_dis_stdlib_shadow(self):
        tool=Path(__file__).resolve().parents[1]/'nascar_engine_audio.py'
        run=subprocess.run([sys.executable,str(tool),'--help'],text=True,capture_output=True)
        self.assertEqual(run.returncode,0,run.stderr)
        self.assertIn('--profiles',run.stdout)

    def test_count_bounded_roundtrip_does_not_parse_exporter_tail(self):
        p = profile()
        self.assertEqual(len(p), 3)
        encoded = encode_esfx(p)
        self.assertEqual(len(encoded), 4 + 3 * 44)
        self.assertEqual(parse_esfx(encoded), p)
        self.assertEqual(p[2].sample, 'eng12.sfx')

    def test_malformed_records_rejected_before_preparation(self):
        bad = [struct.pack('<I', 9), struct.pack('<I', 1) + record()[:-1],
               struct.pack('<I', 1) + record('../evil.sfx'),
               struct.pack('<I', 1) + record(values=(0,.1,.2,0,1,2,3)),
               struct.pack('<I', 1) + record(values=(1,.1,.2,0,2,1,3)),
               struct.pack('<I', 1) + record(values=(1,math.nan,.2,0,1,2,3))]
        for raw in bad:
            with self.subTest(raw=raw[:20]), self.assertRaises(ValueError): parse_esfx(raw)

    def test_linear_rpm_gain_and_envelope_not_target_smoothstep(self):
        r = profile()[0]
        # At quarter ramp and quarter gain interval, exact linear source values.
        self.assertAlmostEqual(r.evaluate(825, 1).volume, .65 * .25 * .6, places=7)
        rpm = 3000 + .25 * (8100-3000)
        self.assertAlmostEqual(r.evaluate(rpm, 1).volume, (.65 + .25*(.96-.65))*.6, places=7)
        self.assertAlmostEqual(r.evaluate(7806, 1).pitch, 1)
        for rpm in (99, 80000, 90000):self.assertFalse(r.evaluate(rpm,1).active)
        self.assertEqual(r.evaluate(100, 1).volume, 0)
        self.assertFalse(r.evaluate(100, 1).active)
        self.assertFalse(r.evaluate(7806, 0).active)
        self.assertAlmostEqual(r.evaluate(8100, 1).volume, .96*.6, places=7)
        self.assertAlmostEqual(r.evaluate(44050, 1).volume, .96*.5*.6, places=7)

    def test_source_caller_gain_random_input_and_player_multiplier(self):
        self.assertAlmostEqual(source_caller_gain(1, .1), (.09375+.15625)*2.2)
        self.assertAlmostEqual(source_caller_gain(0, .1), (.09375+.15625*.1)*2.2)
        self.assertAlmostEqual(source_caller_gain(0, .6), (.09375+.15625*.6)*2.2)
        self.assertAlmostEqual(source_caller_gain(.2, .6), .09375*2.2)
        self.assertAlmostEqual(source_caller_gain(.2,.6,viewed=True,local_mix=True),.09375*2.2*2.32)
        self.assertAlmostEqual(source_caller_gain(.2,.6,viewed=True),.09375*2.2*8/3)
        with self.assertRaises(ValueError):source_caller_gain(0, .9)

    def test_focus_count_high_throttle_and_explicit_silence_gates(self):
        p = profile()
        focused = evaluate_engine(p, 7000, .8, .3, viewed=True, local_mix=True)
        self.assertFalse(focused[0].active)
        self.assertTrue(focused[1].active)
        self.assertGreater(focused[2].volume, 0)
        low = evaluate_engine(p, 7000, .2, .3, viewed=True, local_mix=True)
        self.assertEqual(low[2].volume, 0) # branch-likely correction: HIGH-throttle gated
        self.assertFalse(low[2].active)
        remote = evaluate_engine(p, 7000, .8, .3)
        self.assertTrue(remote[0].active)
        self.assertFalse(remote[1].active)
        self.assertFalse(remote[2].active)
        solo = evaluate_engine(p[:1],7000,.8,.3,viewed=True,local_mix=True)
        self.assertTrue(solo[0].active)
        self.assertTrue(all(not v.active for v in evaluate_engine(p,7000,.8,.3,silenced=True)))

    @unittest.skipUnless(shutil.which('c++'), 'Requires a C++17 compiler for standalone CPU core')
    def test_runtime_cpp_core_matches_independent_source_reference(self):
        repo=Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as temp:
            exe=Path(temp)/'core';fixture=Path(temp)/'profile.ens'
            fixture.write_bytes(encode_esfx(profile()))
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(repo/'src'),
                str(Path(__file__).with_name('nascar_engine_audio_core_test.cpp')),'-o',str(exe)],check=True,capture_output=True)
            result=subprocess.run([str(exe),str(fixture)],check=True,text=True,capture_output=True)
            lines=result.stdout.splitlines()
            self.assertEqual(len(lines),12*4*3*3)
            for line in lines:
                rpm,throttle,focus,index,active,pitch,volume=map(float,line.split())
                expected=evaluate_engine(profile(),rpm,throttle,.3,viewed=focus!=0,local_mix=focus==2)[int(index)]
                self.assertEqual(bool(active),expected.active)
                self.assertAlmostEqual(pitch,expected.pitch,delta=2e-6)
                self.assertAlmostEqual(volume,expected.volume,delta=2e-6)

    def test_source_camera_modes_and_first_view_match(self):
        # Source sound classification is camera-dependent, not input-dependent.
        for mode in range(17):
            state=source_view_context(14,[(14,mode)])
            self.assertTrue(state['viewed'])
            self.assertEqual(state['local_mix'],mode not in (9,14,15))
        self.assertEqual(source_view_context(14,[(14,15),(14,5)]),
            dict(viewed=True,local_mix=False,view_index=0))
        self.assertEqual(source_view_context(14,[(2,5)]),
            dict(viewed=False,local_mix=False,view_index=None))

    def test_namespaces_keep_short_native_names_and_reject_paths(self):
        self.assertEqual(namespace_sample('eng12.sfx'), 'nseng12.sfx')
        with self.assertRaises(ValueError): namespace_sample('a'*14+'.sfx')
        with self.assertRaises(ValueError): namespace_sample('../eng12.sfx')


@unittest.skipUnless(os.environ.get('NASCAR_AUDIO_FIXTURES'), 'Owned source fixture opt-in')
class OwnedSourceAudioTests(unittest.TestCase):
    def test_real_family_pitch_and_native_resource_preservation(self):
        root = Path(os.environ['NASCAR_AUDIO_FIXTURES'])
        mod = parse_esfx((root/'engine-audio-source/eng4e.ens').read_bytes())
        hobby = parse_esfx((root/'engine-audio-source/eng3e.ens').read_bytes())
        self.assertEqual((mod[0].sample,hobby[0].sample), ('eng30.sfx','eng30.sfx'))
        self.assertEqual((mod[0].sample_rpm,hobby[0].sample_rpm),(4043,3791))
        with tempfile.TemporaryDirectory() as temp:
            report=prepare_audio(root/'engine-audio-source',root/'engine-samples-source',
                Path(os.environ['EOD_DISC_DATA'])/'RES',Path(temp))
            self.assertEqual(report['family_profiles'],{'stock':'eng1','truck':'eng1','modified':'eng4','hobby':'eng3','pacecar':'eng1'})
            self.assertEqual(report['native_preservation']['RACESND'],65)
            self.assertEqual(report['native_preservation']['RACE'],355)
            self.assertEqual(len(report['samples']),3)
            self.assertTrue(all(s['byte_identical'] for s in report['samples']))
            self.assertFalse(report['runtime_enabled'])
            self.assertEqual(report['samples'][1]['source_name'],'eng12.sfx')
            self.assertEqual(report['samples'][1]['size'],137200)

if __name__ == '__main__':unittest.main()
