"""Synthetic checks for optional Battle catalogs and authored layout edits."""
import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from build_gameplay_mod import battle_catalog, respawn_layout, remove_layout_objects
from valt import compile_binary_valt, parse_binary_valt


def sec(name, pairs, parent=''):
    return {'name': name, 'parent': parent, 'pairs': pairs}


class GameplayModTests(unittest.TestCase):
    def test_battle_catalog_preserves_career_and_bounds_action(self):
        model = {'sections': [sec('eve', [('mode', 'career'), ('@contest', 'race')]),
                              sec('eve', [('@contest', 'race')]),
                              sec('eve', [('mode', 'multi'), ('@contest', 'race')]),
                              sec('race', [('event_type', 'normal_race')])]}
        result = battle_catalog(model, 'Welch')
        eves = [s for s in result['sections'] if s['name'] == 'eve']
        self.assertEqual(len(eves), 3)
        self.assertEqual(sum(k == '@contest' for k, _ in eves[1]['pairs']), 2)
        first = next(s for s in result['sections'] if s['name'] == eves[1]['pairs'][0][1])
        self.assertEqual(dict(first['pairs'])['event_type'], 'normal_race')
        self.assertRegex(first['name'], r'^\$[0-9]{4}\$contest$')
        self.assertEqual(eves[0]['pairs'], model['sections'][0]['pairs'])
        self.assertEqual(sum(dict(s['pairs']).get('mode') == 'multi' for s in eves), 1)
        battle = next(s for s in result['sections'] if s['name'] == '$9999$contest')
        self.assertEqual(dict(battle['pairs'])['max_cars'], '4')
        self.assertEqual(dict(battle['pairs'])['external_table'], 'c_battle')
        self.assertEqual(len(battle_catalog(result, 'Welch')['sections']), len(result['sections']))

    def test_native_root_count_exposes_new_eves_before_child_sections(self):
        model = {'unk1': 1, 'orig_sec_count': 3,
                 'sections': [sec('eve', [('@contest', 'race')]),
                              sec('race', [('event_type', 'normal_race')]),
                              sec('unused_child', [('foo', 'bar')])]}
        data = compile_binary_valt(battle_catalog(model, 'Welch'))
        root_count = struct.unpack_from('<I', data, 8)[0]
        sections = parse_binary_valt(data)['sections']
        self.assertEqual(sum(s['name'] == 'eve' for s in sections[:root_count]), 2)
        self.assertEqual(root_count, 2)

    def test_explicit_root_count_rejects_invalid_metadata(self):
        for value in (-1, 2, 1.5, True):
            with self.assertRaises(ValueError):
                compile_binary_valt({'root_count': value, 'sections': [sec('root', [])]})
        data = compile_binary_valt(respawn_layout({'node_size': 60, 'flags': 8 << 16,
            'nodes': [[i*20., 100., 1., 0.] + [0.] * 11 for i in range(8)]}))
        self.assertEqual(struct.unpack_from('<I', data, 8)[0], 1)

    def test_missing_battle_inherits_native_race_terrain(self):
        model = {'root_count': 1,
                 'sections': [sec('defaults', [('terrain_mask', '64'), ('terrain_disable_mask', '128')]),
                              sec('race', [('event_type', 'normal_race')], parent='defaults')]}
        result = battle_catalog(model, 'Welch')
        battle = next(s for s in result['sections'] if s['name'] == '$9999$contest')
        self.assertEqual(dict(battle['pairs'])['terrain_mask'], '64')
        self.assertEqual(dict(battle['pairs'])['terrain_disable_mask'], '128')

    def test_native_display_name_preserves_preview_movie_identity(self):
        model = {'root_count': 1,
                 'sections': [sec('eve', [('name', 'Woodly'), ('track', 'Afton'), ('@contest', 'race')]),
                              sec('race', [('event_type', 'normal_race')])]}
        eves = [s for s in battle_catalog(model, 'Afton')['sections'] if s['name'] == 'eve']
        self.assertEqual([dict(s['pairs'])['name'] for s in eves], ['Woodly', 'Woodly'])

    def test_native_battle_terrain_override_is_preserved(self):
        model = {'sections': [sec('race', [('event_type', 'normal_race')]), sec('native', [('event_type', 'battle'),
                                            ('terrain_mask', '64'),
                                            ('terrain_disable_mask', '128')])]}
        result = battle_catalog(model, 'HomeBase')
        battle = next(s for s in result['sections'] if s['name'] == '$9999$contest')
        self.assertEqual(dict(battle['pairs'])['terrain_mask'], '64')
        self.assertEqual(dict(battle['pairs'])['terrain_disable_mask'], '128')

    def test_respawns_use_primary_native_xz_and_tangent(self):
        nodes = [[i*20., 100., 1., 0.] + [0.] * 11 for i in range(8)]
        model = {'node_size': 60, 'flags': 8 << 16, 'nodes': nodes}
        layout = respawn_layout(model)
        root = layout['sections'][0]
        self.assertEqual(len(root['pairs']), 8)
        point = dict(layout['sections'][1]['pairs'])
        self.assertEqual(point, {'pt': '0, 100', 'dir': '1, 0'})
        model['flags'] = 20 << 16
        with self.assertRaises(ValueError): respawn_layout(model)
        model['flags'] = 8 << 16
        model['nodes'][0][0] = float('nan')
        with self.assertRaises(ValueError): respawn_layout(model)

    def test_remove_only_selected_native_layout_objects(self):
        model = {'sections': [sec('objects', [('@object', 'barrier'), ('@object', 'tree')]),
                              sec('barrier', [('dyno', 'Semi_Trailer')]),
                              sec('tree', [('dyno', 'Tree')])]}
        result, count = remove_layout_objects(model, {'semi_trailer'})
        self.assertEqual(count, 1)
        self.assertEqual(result['sections'][0]['pairs'], [('@object', 'tree')])
        self.assertEqual(model['sections'][0]['pairs'][0], ('@object', 'barrier'))
        self.assertEqual(result['sections'][1], model['sections'][1])


if __name__ == '__main__': unittest.main()
