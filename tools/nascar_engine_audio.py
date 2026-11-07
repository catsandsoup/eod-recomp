#!/usr/bin/env python3
"""Source ESFX contracts and offline resources; does not activate untested playback.

Owned source SLUS_204.41:1EE9E8 loads count + 44-byte records,1EF3F8
calculates sample pitch/linear RPM gain/envelope,1EEEC8 supplies throttle gain
and voice gates. See work/nascar-next/BEHAVIOR_RE.md and source disassembly.
The target23C8D0 native mixer has different curves; these resources require an
isolated mixer hook before car engine_sound references may switch to them.
"""
import argparse
from collections import namedtuple
import hashlib
import json
import math
from pathlib import Path
import re
import struct

FAMILY_PROFILES = {'stock':'eng1', 'truck':'eng1', 'modified':'eng4',
                   'hobby':'eng3', 'pacecar':'eng1'}


def _sample_name(name):
    if not re.fullmatch(r'[A-Za-z0-9_]{1,11}\.sfx', name):
        raise ValueError(f'Invalid ESFX sample filename: {name!r}')
    return name


def namespace_sample(name):
    name = 'ns' + _sample_name(name)
    if len(name.encode('ascii')) >= 16:
        raise ValueError('Namespaced sample does not fit source char[16]')
    return name


class Voice(namedtuple('Voice', 'active pitch volume')):
    __slots__=()


class EsfxRecord(namedtuple('EsfxRecord',
        'sample sample_rpm gain_low gain_high outer_low inner_low inner_high outer_high')):
    __slots__=()

    def evaluate(self, rpm, caller_gain):
        if not all(math.isfinite(v) for v in (rpm, caller_gain)):
            raise ValueError('Nonfinite voice input')
        if not self.outer_low <= rpm < self.outer_high:
            return Voice(False, 0, 0)
        x = min(1, max(0, (rpm-self.inner_low)/(self.inner_high-self.inner_low)))
        gain = self.gain_low + x*(self.gain_high-self.gain_low)
        if rpm < self.inner_low:
            envelope = (rpm-self.outer_low)/(self.inner_low-self.outer_low)
        elif rpm > self.inner_high:
            envelope = (rpm-self.outer_high)/(self.inner_high-self.outer_high)
        else:
            envelope = 1
        level = gain*envelope*caller_gain
        # Source1EF4B8..530 pauses zero/negative gain rather than playing silence.
        return Voice(True, rpm/self.sample_rpm, level*.6) if level > 0 else Voice(False,0,0)


def parse_esfx(raw):
    """Parse a decontainerized v2 ESFX; ignore unused serialized-capacity tail."""
    if len(raw) < 4:
        raise ValueError('Truncated ESFX count')
    count, = struct.unpack_from('<I', raw)
    if count > 8 or len(raw) < 4+44*count:
        raise ValueError('Invalid ESFX count/extent')
    result = []
    for i in range(count):
        name, *values = struct.unpack_from('<16s7f', raw, 4+44*i)
        try:
            name = _sample_name(name.split(b'\0',1)[0].decode('ascii'))
        except UnicodeDecodeError as error:
            raise ValueError('Invalid ESFX sample encoding') from error
        if not all(math.isfinite(v) for v in values):
            raise ValueError('Nonfinite ESFX parameter')
        sample_rpm, low, high, outer_low, inner_low, inner_high, outer_high = values
        if sample_rpm <= 0 or not outer_low < inner_low < inner_high < outer_high:
            raise ValueError('Invalid ESFX RPM interval/calibration')
        result.append(EsfxRecord(name, *values))
    return tuple(result)


def encode_esfx(records):
    if len(records) > 8:
        raise ValueError('ESFX capacity exceeded')
    raw = struct.pack('<I', len(records))
    for record in records:
        fields = record._asdict()
        raw += struct.pack('<16s7f', fields.pop('sample').encode('ascii'), *fields.values())
    parse_esfx(raw) # reject invalid authored records as well as bad input
    return raw


def source_caller_gain(throttle, audio_random, *, viewed=False, local_mix=False):
    """Source gain before sample helper; caller supplies independent audio RNG.

    Quantized source stream5 advances even at high throttle. This pure contract
    deliberately draws no RNG and cannot perturb target gameplay streams. Native
    camera mapping must supply viewed/local_mix; it is not inferred here.
    """
    if not math.isfinite(throttle) or not .1 <= audio_random <= .6:
        raise ValueError('Invalid source throttle/audio-random input')
    if local_mix and not viewed:
        raise ValueError('Local source mix requires a viewed car')
    slope = 1.25*(throttle-.2) if throttle > .2 else 5*(.2-throttle)*audio_random
    gain = .09375 + .15625*slope
    if viewed:
        gain *= 2.32 if local_mix else 8/3
    return gain*2.2


def source_view_context(car_id, cameras):
    """Camera pairs are (viewed car ID, mode); first matching view wins.

    Source1EF09C uses1BB6A0 (viewed car),1BB680 (camera mode), not player
    input/control state. Target1D3F98/1D3F78 are equivalent getters with a
    different stride. Names0..16 match. Source jump-table hidden modes retained.
    """
    external_modes={9,14,15,17,18,19,20,22}
    for index,(viewed_car,mode) in enumerate(cameras):
        if viewed_car==car_id:
            return dict(viewed=True,local_mix=mode not in external_modes,view_index=index)
    return dict(viewed=False,local_mix=False,view_index=None)


def evaluate_engine(records, rpm, throttle, audio_random, *, viewed=False,
                    local_mix=False, silenced=False):
    gain = source_caller_gain(throttle,audio_random,viewed=viewed,local_mix=local_mix)
    result = []
    for i, record in enumerate(records):
        # Source1EF250..294: focused first sample excluded only when count>=2;
        # nonfocused remaining samples likewise excluded when count>=2.
        suppressed = silenced or (len(records)>=2 and ((i==0)==local_mix))
        if suppressed:
            result.append(Voice(False,0,0))
        else:
            result.append(record.evaluate(rpm, gain if i<2 or throttle>.2 else 0))
    return tuple(result)


def _sha(blob):
    return hashlib.sha256(blob).hexdigest()


def prepare_audio(profile_dir, sample_dir, native_res, out):
    """Build native-preserving archives with new, currently inactive ESFX assets.

    Never changes an installed disc/profile or existing candidate. Native engine
    presets remain referenced until the source-only runtime adapter is accepted.
    """
    from nascar_roster import Archive
    profile_dir,sample_dir,native_res,out=map(Path,(profile_dir,sample_dir,native_res,out))
    if out.exists() and any(out.iterdir()):
        raise ValueError('Audio preparation requires a new empty output directory')
    profiles={}
    for profile in sorted(set(FAMILY_PROFILES.values())):
        blob=(profile_dir/(profile+'e.ens')).read_bytes()
        records=parse_esfx(blob)
        if not records:
            raise ValueError('Required source engine profile is empty')
        profiles[profile]=(blob,records)
    names=sorted({r.sample for _,rs in profiles.values() for r in rs})
    samples={}
    for name in names:
        blob=(sample_dir/name).read_bytes()
        if len(blob)<32:
            raise ValueError(f'Truncated SFX0 sample: {name}')
        extent,header,channels,_,rate,_,_,_=struct.unpack_from('<8I',blob)
        if header!=32 or channels!=1 or rate!=22050 or extent%16 or header+extent!=len(blob):
            raise ValueError(f'Unsupported/incomplete source SFX0: {name}')
        samples[name]=blob
    # All source extents/parameters checked before creating candidate resources.
    race=Archive(native_res/'RACE.RES',out/'extracted/RACE')
    sound=Archive(native_res/'RACESND.RES',out/'extracted/RACESND')
    report={'runtime_enabled':False,'family_profiles':FAMILY_PROFILES.copy(),
            'profiles':{},'samples':[],'native_preservation':{}}
    for profile,(blob,records) in profiles.items():
        target='ns'+profile+'.ens'
        converted=encode_esfx(tuple(r._replace(sample=namespace_sample(r.sample)) for r in records))
        race.add(target,converted,version=2,fourcc='ESFX')
        report['profiles'][profile]={'source_file':str(profile_dir/(profile+'e.ens')),
            'source_sha256':_sha(blob),'source_size':len(blob),'target_name':target,
            'target_sha256':_sha(converted),'record_count':len(records),
            'ignored_exporter_tail_bytes':len(blob)-(4+44*len(records)),
            'records':[r._asdict() for r in records]}
    for name,blob in samples.items():
        target=namespace_sample(name)
        sound.add(target,blob,version=3,fourcc='SFX0')
        report['samples'].append({'source_name':name,'target_name':target,'size':len(blob),
            'source_sha256':_sha(blob),'target_sha256':_sha(sound.read(target)),
            'byte_identical':sound.read(target)==blob,'sample_rate':22050})
    for name,archive in [('RACE',race),('RACESND',sound)]:
        archive.assert_native_preserved()
        report['native_preservation'][name]=len(archive.original)
        archive.pack(out/'DATA/RES'/f'{name}.RES')
    report['acceptance_remaining']=['source-only mixer hook','native control/view/split-screen mapping',
        'source audio random stream policy','live RPM/throttle/voice verification','listening/native regression']
    (out/'audio-provenance.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profiles',type=Path,required=True)
    parser.add_argument('--samples',type=Path,required=True)
    parser.add_argument('--native-res',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    report=prepare_audio(args.profiles,args.samples,args.native_res,args.out)
    print(f'Prepared {len(report["profiles"])} profiles/{len(report["samples"])} original samples; playback not activated')

if __name__=='__main__':main()
